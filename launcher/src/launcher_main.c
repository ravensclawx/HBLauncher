/*
 * launcher_main.c - the native launcher's frame loop and app handoff.
 *
 * Lifecycle with the host:
 *
 *   host maps code RX + data RW, builds ctx, calls _start
 *   -> launcher creates the Vulkan display once, then runs frames
 *   -> the user picks a title; the launcher presents a loading panel and
 *      writes control->launch = index + 1, then returns
 *   -> the host unwinds the call, loads the app package, calls the app
 *   -> the app returns (B / its own exit), the host calls the launcher again
 *      with HBX_FLAG_RESUME and args[1] = index + 1
 *   -> the launcher shows a "returned from" toast and keeps running
 *
 * Every entry runs for at most args[3] frames (default 720 = 12 s), so the
 * host keeps control without a concurrent RPC while the launcher is running.
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_ui.h"
#include "launcher_ui.h"
#include "launcher_vk.h"

/* Persistent across re-entries.  The console payload can only map pages RX
 * (0x524f0012 finalises each page RX), so the launcher keeps no .data/.bss:
 * all mutable state lives in the loader-allocated RW persist block at
 * ctx->data_base, zero-filled once per session and preserved across
 * re-entries and app launches.  struct hbx_persist (SDK time/pace anchors)
 * must stay first. */
struct launcher_persist {
    struct hbx_persist sdk;
    struct launcher_display display;
    struct hbx_render render;
    struct launcher_state state;
    uint32_t initialized;
    uint32_t last_sequence;
    uint32_t icon_signature;
    /* Catalog rescan handshake: the pointer/count the request was made
     * against, and whether this entry already yielded to the host once. */
    uint32_t refresh_active;
    uint32_t refresh_yielded;
    uint32_t refresh_frames;
    const struct hbx_catalog *refresh_catalog;
    uint32_t refresh_count;
    char toast[64];
    char error[96];
};
_Static_assert(sizeof(struct launcher_persist) <= 8 * 1024,
               "launcher persist block exceeds its 8 KB budget");

static void copy_string(char *dst, uint32_t capacity, const char *src) {
    uint32_t i = 0;
    if (!dst || capacity == 0) return;
    while (src && *src && i + 1 < capacity) dst[i++] = *src++;
    dst[i] = 0;
}

static void append_string(char *dst, uint32_t capacity, const char *src) {
    uint32_t length = (uint32_t)hbx_strlen(dst);
    copy_string(dst + length, capacity > length ? capacity - length : 0, src);
}

static int nul_within(const char *text, uint64_t limit) {
    uint64_t i;
    for (i = 0; i < limit; i++) if (text[i] == 0) return 1;
    return 0;
}

/* Accept a catalog only when its counts and string ranges are trustworthy. */
static uint32_t validate_catalog(const struct hbx_ctx *ctx,
                                 const struct hbx_catalog *catalog,
                                 const char **strings, uint32_t *strings_bytes) {
    uint32_t i;
    *strings = NULL;
    *strings_bytes = 0;
    if (!catalog || catalog->magic != HBX_CATALOG_MAGIC) return 0;
    if (catalog->count > HBX_MAX_CATALOG) return 0;
    if (ctx->catalog_bytes < 32u + (uint64_t)catalog->count * 32u) return 0;
    if (catalog->strings_off > ctx->catalog_bytes) return 0;
    if (catalog->strings_bytes > ctx->catalog_bytes - catalog->strings_off) return 0;
    *strings = (const char *)(uintptr_t)(ctx->catalog + catalog->strings_off);
    *strings_bytes = catalog->strings_bytes;
    for (i = 0; i < catalog->count; i++) {
        uint32_t name_off = catalog->entries[i].name_off;
        uint32_t desc_off = catalog->entries[i].desc_off;
        if (name_off >= *strings_bytes) return 0;
        if (desc_off >= *strings_bytes) return 0;
        if (!nul_within(*strings + name_off, *strings_bytes - name_off)) return 0;
        if (!nul_within(*strings + desc_off, *strings_bytes - desc_off)) return 0;
    }
    return catalog->count;
}

static const char *catalog_name(const struct launcher_state *state, uint32_t index) {
    if (!state->catalog || !state->strings || index >= state->count) return "";
    return state->strings + state->catalog->entries[index].name_off;
}

static void publish_gpu(const struct hbx_ctx *ctx, const struct launcher_persist *P) {
    struct hbx_ctx *mutable_ctx = (struct hbx_ctx *)(uintptr_t)ctx;
    uint32_t i;
    mutable_ctx->gpu.instance = (uint64_t)P->display.instance;
    mutable_ctx->gpu.physical = (uint64_t)P->display.physical;
    mutable_ctx->gpu.device = (uint64_t)P->display.device;
    mutable_ctx->gpu.queue = (uint64_t)P->display.queue;
    mutable_ctx->gpu.queue_family = P->display.queue_family;
    mutable_ctx->gpu.width = P->display.width;
    mutable_ctx->gpu.height = P->display.height;
    mutable_ctx->gpu.color_format = (uint32_t)P->display.format;
    mutable_ctx->gpu.color_space = 0;
    mutable_ctx->gpu.swapchain = (uint64_t)P->display.swapchain;
    mutable_ctx->gpu.image_count = P->display.image_count;
    mutable_ctx->gpu.vi_window = ctx->gpu.vi_window;
    mutable_ctx->gpu.create_vi_surface = ctx->gpu.create_vi_surface;
    mutable_ctx->gpu.destroy_surface = ctx->gpu.destroy_surface;
    mutable_ctx->gpu.surface = (uint64_t)P->display.surface;
    mutable_ctx->gpu.present_state = ctx->gpu.present_state;
    mutable_ctx->gpu.present_state_bytes = ctx->gpu.present_state_bytes;
    for (i = 0; i < P->display.image_count && i < HBX_MAX_IMAGES; i++) {
        mutable_ctx->gpu.images[i] = (uint64_t)P->display.images[i];
        mutable_ctx->gpu.views[i] = (uint64_t)P->display.views[i];
    }
    mutable_ctx->ui = (uint64_t)(uintptr_t)&P->render;
}

static void append_uint(char *dst, uint32_t capacity, uint32_t value) {
    char digits[12];
    uint32_t count = 0;
    uint32_t length = (uint32_t)hbx_strlen(dst);
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value && count < 10u);
    while (count && length + 1u < capacity) dst[length++] = digits[--count];
    dst[length] = 0;
}

static void build_toast(struct launcher_persist *P, const char *prefix, const char *name) {
    P->toast[0] = 0;
    append_string(P->toast, sizeof P->toast, prefix);
    append_string(P->toast, sizeof P->toast, name);
}


/* Frame-level diagnostic stages (args[6] 7..9), used after the display is up:
 *   7  build one launcher frame, no present
 *   8  present an empty frame (clear only)
 *   9  one full frame: input poll + draw + present
 * Each returns cleanly so the host reads the log ring. */
static int launcher_frame_probe(const struct hbx_ctx *ctx, struct launcher_persist *P, uint32_t stage) {
    struct hbx_input_state input;
    hbx_memset(&input, 0, sizeof input);

    if (stage >= 13u) {
        /* 13: two empty presents (swapchain repeat only)
         * 14: two frames with draw, no input/state/pacing
         * 15: two frames with input + handle_input + time + frame_end */
        uint32_t frames = 2, i;
        struct launcher_nav nav;
        hbx_memset(&nav, 0, sizeof nav);
        nav.armed_index = 0xffffffffu;
        for (i = 0; i < frames; i++) {
            if (stage >= 15u) {
                hbx_input_poll(ctx, &input);
                P->state.time = (float)((double)hbx_now_ns(ctx) * 1e-9);
                launcher_handle_input(&P->state, &input, &nav);
            }
            hbx_ui_engine_begin(&P->display.ui, 0xfffcf9f9u);
            if (stage >= 14u) hbx_launcher_draw(&P->display.ui, &P->state);
            if (hbx_ui_present(ctx) != HBX_OK) { hbx_log(ctx, "probe: present failed"); return HBX_ERR_GPU; }
            if (stage >= 15u) hbx_frame_end(ctx);
        }
        hbx_logf(ctx, "probe: stage %u, %u frames", stage, frames);
        return HBX_OK;
    }
    if (stage >= 10u) {
        /* Loop-level stages: 10 = 1 frame + frame_end + one pace call;
         * 11 = 3 frames without pacing; 12 = 3 frames with pacing. */
        uint32_t frames = stage == 10u ? 1u : 3u;
        uint64_t deadline = 0;
        uint32_t i;
        struct launcher_nav nav;
        hbx_memset(&nav, 0, sizeof nav);
        nav.armed_index = 0xffffffffu;
        for (i = 0; i < frames; i++) {
            hbx_input_poll(ctx, &input);
            P->state.time = (float)((double)hbx_now_ns(ctx) * 1e-9);
            launcher_handle_input(&P->state, &input, &nav);
            hbx_ui_engine_begin(&P->display.ui, 0xfffcf9f9u);
            hbx_launcher_draw(&P->display.ui, &P->state);
            if (hbx_ui_present(ctx) != HBX_OK) { hbx_log(ctx, "loop: present failed"); return HBX_ERR_GPU; }
            hbx_frame_end(ctx);
            if (stage == 12u) hbx_pace(ctx, &deadline, 60);
        }
        hbx_logf(ctx, "loop: %u frames complete (stage %u)", frames, stage);
        return HBX_OK;
    }
    if (stage == 9u) {
        hbx_log(ctx, "frame: poll input");
        hbx_input_poll(ctx, &input);
        hbx_logf(ctx, "frame: input buttons=%x connected=%u", (unsigned)input.buttons,
                 (unsigned)input.connected);
    }
    if (stage == 7u || stage == 9u) {
        hbx_log(ctx, "frame: draw");
        hbx_ui_engine_begin(&P->display.ui, 0xfffcf9f9u);
        hbx_launcher_draw(&P->display.ui, &P->state);
        hbx_logf(ctx, "frame: draw ok (%u vertices)",
                 (unsigned)hbx_ui_engine_vertex_count(&P->display.ui));
    } else {
        hbx_ui_engine_begin(&P->display.ui, 0xfffcf9f9u);
    }
    if (stage == 8u || stage == 9u) {
        hbx_log(ctx, "frame: present");
        {
            int presented = hbx_ui_present(ctx);
            hbx_logf(ctx, "frame: present %d", presented);
            if (presented != HBX_OK) return HBX_ERR_GPU;
        }
    }
    hbx_logf(ctx, "frame: stage %u complete", stage);
    return HBX_OK;
}

int hbx_app_main(const struct hbx_ctx *ctx) {
    uint32_t frames = 0;
    uint32_t max_frames;
    struct launcher_nav nav;
    hbx_memset(&nav, 0, sizeof nav);
    nav.armed_index = 0xffffffffu;   /* no row armed for a second tap */
    uint32_t fps_milli = 66667;
    struct hbx_input_state input;
    hbx_status status = hbx_ctx_check(ctx);
    struct launcher_persist *P;

    if (status != HBX_OK) return status;
    P = (struct launcher_persist *)(void *)hbx_persist_of(ctx);
    if (!P || ctx->data_size < sizeof *P) {
        hbx_logf(ctx, "hbl: persist block missing or too small (%u < %u)",
                 (unsigned)ctx->data_size, (unsigned)sizeof *P);
        return HBX_ERR_ABI;
    }
    /* Resident-reuse signature: the last 8 bytes of the persist block carry
     * "HBLP" + the ABI version, so a later hbl-start can confirm the block
     * still belongs to this launcher before reusing it (the code image
     * itself is verified by hash; an SDK allocation cannot be). */
    {
        volatile uint32_t *signature =
            (volatile uint32_t *)(void *)((unsigned char *)P + ctx->data_size - 8u);
        signature[0] = 0x504c4248u;   /* "HBLP" */
        signature[1] = HBX_ABI_VERSION;
    }
    hbx_memset(&input, 0, sizeof input);

    max_frames = (uint32_t)ctx->args[3];
    if (max_frames == 0 || max_frames > 36000u) max_frames = 720;

    if (!P->initialized) {
        static const char *const stages[] = {
            "ok", "arguments", "dependencies", "instance", "physical-device",
            "vi-surface", "queue-family", "device", "swapchain", "ui-init", "attach"
        };
        int created;
        hbx_log(ctx, "hbl: first entry, creating display");
        uint32_t stage_limit = (uint32_t)ctx->args[6];
        created = launcher_display_create(&P->display, ctx, stage_limit);
        if (created >= 100) {
            hbx_logf(ctx, "hbl: stage %d complete (diagnostic stop)", created - 100);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }
        if (created != HBX_OK) {
            const char *stage = (created <= 0 && -created < (int)(sizeof stages / sizeof stages[0]))
                ? stages[-created] : "unknown";
            hbx_logf(ctx, "hbl: display create failed at %s (status %d, vk %d)", stage,
                     created, (int)P->display.last_result);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_ERR_GPU;
            return HBX_ERR_GPU;
        }
        hbx_ui_publish(&P->display.ui, &P->render);
        P->state.abi = 1;
        P->state.selected = 0;
        P->state.mode = LAUNCHER_MODE_LIBRARY;
        P->state.count = 0;
        P->state.refresh_spin = 0.35f;  /* still badge, not a spinning emblem */
        P->state.row_highlight[0] = 1.0f;
        P->initialized = 1;
        hbx_logf(ctx, "hbl: display ready %ux%u, %u images, format %u", P->display.width,
                 P->display.height, P->display.image_count, (unsigned)P->display.format);
        publish_gpu(ctx, P);
    }

    /* Exclusive-app handshake: release the display and return at once, or
     * rebuild it on the first entry after such an app. */
    {
        struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
        if (control && (control->flags & HBX_CTL_FLAG_RELEASE_DISPLAY)) {
            launcher_display_release(&P->display, ctx);
            control->flags &= ~HBX_CTL_FLAG_RELEASE_DISPLAY;
            publish_gpu(ctx, P);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }
        if (P->display.released) {
            if (launcher_display_reacquire(&P->display, ctx) != HBX_OK) {
                hbx_logf(ctx, "hbl: display reacquire failed (vk %d)", (int)P->display.last_result);
                ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_ERR_GPU;
                return HBX_ERR_GPU;
            }
            publish_gpu(ctx, P);
        }
    }

    /* Refresh catalog pointers (the host rebuilds them when packages change). */
    P->state.catalog = (const struct hbx_catalog *)(uintptr_t)ctx->catalog;
    {
        const char *strings = NULL;
        uint32_t strings_bytes = 0;
        uint32_t count = validate_catalog(ctx, P->state.catalog, &strings, &strings_bytes);
        if (count == 0 && ctx->catalog) hbx_log(ctx, "hbl: catalog rejected by bounds checks");
        P->state.count = count;
        P->state.strings = strings;
        P->state.strings_bytes = strings_bytes;

        /* Rescan handshake.  Once the host has rebuilt the catalog (a new
         * block or a new count), drop the request, the spin and bit 1; bit 0
         * stays untouched because it is the host's stop request. */
        {
            struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
            const struct hbx_catalog *catalog =
                (const struct hbx_catalog *)(uintptr_t)ctx->catalog;
            if (P->refresh_active &&
                (catalog != P->refresh_catalog || count != P->refresh_count)) {
                hbx_log(ctx, "hbl: catalog rewritten, refresh complete");
                P->refresh_active = 0;
                P->refresh_yielded = 0;
                P->refresh_frames = 0;
                P->state.refreshing = 0;
                if (control) control->flags &= ~HBX_CTL_FLAG_REFRESH;
            } else if (P->refresh_active) {
                /* Still pending: keep the host request latched. */
                if (control) control->flags |= HBX_CTL_FLAG_REFRESH;
            } else {
                /* No request in flight; clear any stale bit from a previous
                 * session.  Clearing bit 1 never touches the stop bit. */
                P->state.refreshing = 0;
                if (control) control->flags &= ~HBX_CTL_FLAG_REFRESH;
            }
        }
    }
    if (P->state.selected >= P->state.count) P->state.selected = P->state.count ? P->state.count - 1 : 0;
    if (P->state.last_index >= P->state.count) P->state.last_index = 0;
    /* A launch request that was never consumed (the host re-entered without a
       resume) must not fire again on the first frame of this entry. */
    if (P->state.mode == LAUNCHER_MODE_LAUNCH) {
        P->state.mode = LAUNCHER_MODE_LIBRARY;
        P->state.transition = 0.0f;
    }
    publish_gpu(ctx, P);

    /* Icons can change between host rebuilds; re-upload when the set changes. */
    if (ctx->icons && P->state.catalog && P->state.catalog->icon_count) {
        uint32_t layers = P->state.catalog->icon_count;
        uint32_t signature = (uint32_t)ctx->icons ^ (layers * 2654435761u);
        if (signature != P->icon_signature) {
            if (layers > HBX_MAX_ICON_LAYERS) layers = HBX_MAX_ICON_LAYERS;
            hbx_ui_set_icons(&P->display.ui, (const void *)(uintptr_t)ctx->icons, layers);
            P->icon_signature = signature;
        }
    }

    /* Resume / error presentation. */
    if (ctx->flags & HBX_FLAG_RESUME) {
        uint32_t index = (uint32_t)ctx->args[1] ? (uint32_t)ctx->args[1] - 1u : 0;
        uint64_t app_status = ctx->args[2];
        if (ctx->args[1] && app_status != 0 && app_status != (uint64_t)HBX_ERR_STOPPED) {
            P->error[0] = 0;
            append_string(P->error, sizeof P->error, catalog_name(&P->state, index));
            append_string(P->error, sizeof P->error, " exited with status ");
            append_uint(P->error, sizeof P->error, (uint32_t)app_status);
            P->state.error_text[0] = 0;
            copy_string(P->state.error_text, sizeof P->state.error_text, P->error);
            P->state.mode = LAUNCHER_MODE_ERROR;
            P->state.transition = 1.0f;
            P->state.fade = 1.0f;      /* fade the launcher back in */
            P->state.last_index = index;
        } else if (ctx->args[1]) {
            build_toast(P, "Returned from ", catalog_name(&P->state, index));
            P->state.toast_frames = 150;
            P->state.selected = index;
            P->state.mode = LAUNCHER_MODE_LIBRARY;
            P->state.transition = 0.0f;
            P->state.fade = 1.0f;      /* fade the launcher back in */
        }
    }
    if (P->state.selected >= P->state.count) P->state.selected = P->state.count ? P->state.count - 1 : 0;
    if (P->state.last_index >= P->state.count) P->state.last_index = 0;
    P->last_sequence = (uint32_t)ctx->args[0];

    {
        uint32_t stage = (uint32_t)ctx->args[6];
        if (stage >= 7u && stage <= 15u) {
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = (uint64_t)launcher_frame_probe(ctx, P, stage);
            return (int)((struct hbx_ctx *)(uintptr_t)ctx)->status;
        }
    }

    while (frames < max_frames) {
        struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;

        hbx_input_poll(ctx, &input);
        /* The proven launcher's loop discipline: fixed 15 ms sleep after each
         * present and animation time derived from the frame counter.  No tick
         * reads and no floating-point timestamp conversion per frame. */
        P->state.fps_milli = fps_milli;
        P->state.time = (float)frames * (1.0f / 60.0f);
        P->state.frame = frames;
        if (P->state.toast_frames) P->state.toast_frames--;
        copy_string(P->state.toast, sizeof P->state.toast, P->toast);
        if (P->state.toast_frames == 0) P->toast[0] = 0;

        if (P->state.mode == LAUNCHER_MODE_LAUNCH) {
            P->state.transition += 0.14f;
            if (P->state.transition >= 1.0f) P->state.transition = 1.0f;
        } else if (P->state.mode == LAUNCHER_MODE_ERROR) {
            P->state.transition += 0.10f;
            if (P->state.transition >= 1.0f) P->state.transition = 1.0f;
        }

        launcher_handle_input(&P->state, &input, &nav);
        launcher_step_animation(&P->state);

        /* Y was pressed: latch the request against the catalog we are showing
         * and raise bit 1 of the control page for the host to service. */
        if (P->state.refreshing) {
            struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
            if (!P->refresh_active) {
                P->refresh_active = 1;
                P->refresh_yielded = 0;
                P->refresh_frames = 0;
                P->refresh_catalog = P->state.catalog;
                P->refresh_count = P->state.count;
                hbx_logf(ctx, "hbl: refresh requested (%u titles)", (unsigned)P->state.count);
            }
            if (control) control->flags |= HBX_CTL_FLAG_REFRESH;
            P->refresh_frames++;
        }

        /* Exit gesture: Minus, or a stop request from the host. */
        if ((input.pressed & HBX_MINUS) || hbx_stop_requested(ctx)) {
            if (control) {
                control->launch = HBX_CTL_LAUNCH_EXIT;
                control->launch_hash = 0;
            }
            hbx_log(ctx, "hbl: exit requested");
            hbx_frame_end(ctx);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }

        hbx_ui_engine_begin(&P->display.ui, 0xfffcf9f9u);
        hbx_launcher_draw(&P->display.ui, &P->state);

        {
            int presented = hbx_ui_present(ctx);
            if (presented != HBX_OK) {
                hbx_logf(ctx, "hbl: present failed (%d)", presented);
                ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_ERR_GPU;
                return HBX_ERR_GPU;
            }
        }
        frames++;
        hbx_frame_end(ctx);
        if (control) {
            control->heartbeat = hbx_tick();
            control->frame = P->display.ui.frame;
            control->fps_milli = fps_milli;
            /* Never clear control->flags: bit 0 is the host's stop request. */
        }

        /* Give the host a chance to service the rescan instead of spinning
         * "Refreshing..." for the whole frame budget.  Yield once per request;
         * re-entry keeps the bit latched until the catalog block changes. */
        if (P->refresh_active && !P->refresh_yielded && P->refresh_frames >= 20u &&
            P->state.mode == LAUNCHER_MODE_LIBRARY) {
            P->refresh_yielded = 1;
            hbx_log(ctx, "hbl: yielding to host for catalog rescan");
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }

        if (P->state.mode == LAUNCHER_MODE_LAUNCH && P->state.transition >= 1.0f) {
            if (!P->state.catalog || P->state.last_index >= P->state.count) {
                /* The catalog changed underneath the request; go back to the
                   library instead of dereferencing a stale index. */
                hbx_log(ctx, "hbl: launch request dropped (catalog changed)");
                P->state.mode = LAUNCHER_MODE_LIBRARY;
                P->state.transition = 0.0f;
            } else {
                if (control) {
                    control->launch = P->state.last_index + 1u;
                    control->launch_hash = P->state.catalog->entries[P->state.last_index].id_hash;
                    control->last_status = 0;
                }
                hbx_logf(ctx, "hbl: launch request %u", P->state.last_index + 1u);
                ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
                return HBX_OK;
            }
        }

        hbx_sleep_ns(15000000ull);
    }

    /* Frame budget exhausted: give the host a chance to service health. */
    ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
    return HBX_OK;
}
