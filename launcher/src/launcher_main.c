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

/* Persistent across re-entries: the data region stays mapped. */
static struct launcher_display g_display;
static struct hbx_render g_render;
static struct launcher_state g_state;
static uint32_t g_initialized;
static uint32_t g_last_sequence;
static uint32_t g_icon_signature;
/* Catalog rescan handshake: the pointer/count the request was made against,
 * and whether this entry already yielded to the host once. */
static uint32_t g_refresh_active;
static uint32_t g_refresh_yielded;
static uint32_t g_refresh_frames;
static const struct hbx_catalog *g_refresh_catalog;
static uint32_t g_refresh_count;
static char g_toast[64];
static char g_error[96];

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

static void publish_gpu(const struct hbx_ctx *ctx) {
    struct hbx_ctx *mutable_ctx = (struct hbx_ctx *)(uintptr_t)ctx;
    uint32_t i;
    mutable_ctx->gpu.instance = (uint64_t)g_display.instance;
    mutable_ctx->gpu.physical = (uint64_t)g_display.physical;
    mutable_ctx->gpu.device = (uint64_t)g_display.device;
    mutable_ctx->gpu.queue = (uint64_t)g_display.queue;
    mutable_ctx->gpu.queue_family = g_display.queue_family;
    mutable_ctx->gpu.width = g_display.width;
    mutable_ctx->gpu.height = g_display.height;
    mutable_ctx->gpu.color_format = (uint32_t)g_display.format;
    mutable_ctx->gpu.color_space = 0;
    mutable_ctx->gpu.swapchain = (uint64_t)g_display.swapchain;
    mutable_ctx->gpu.image_count = g_display.image_count;
    mutable_ctx->gpu.vi_window = ctx->gpu.vi_window;
    mutable_ctx->gpu.create_vi_surface = ctx->gpu.create_vi_surface;
    mutable_ctx->gpu.destroy_surface = ctx->gpu.destroy_surface;
    mutable_ctx->gpu.surface = (uint64_t)g_display.surface;
    mutable_ctx->gpu.present_state = ctx->gpu.present_state;
    mutable_ctx->gpu.present_state_bytes = ctx->gpu.present_state_bytes;
    for (i = 0; i < g_display.image_count && i < HBX_MAX_IMAGES; i++) {
        mutable_ctx->gpu.images[i] = (uint64_t)g_display.images[i];
        mutable_ctx->gpu.views[i] = (uint64_t)g_display.views[i];
    }
    mutable_ctx->ui = (uint64_t)(uintptr_t)&g_render;
}

static void append_uint(char *dst, uint32_t capacity, uint32_t value) {
    char digits[12];
    uint32_t count = 0;
    uint32_t length = (uint32_t)hbx_strlen(dst);
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value && count < 10u);
    while (count && length + 1u < capacity) dst[length++] = digits[--count];
    dst[length] = 0;
}

static void build_toast(const char *prefix, const char *name) {
    g_toast[0] = 0;
    append_string(g_toast, sizeof g_toast, prefix);
    append_string(g_toast, sizeof g_toast, name);
}


/* Frame-level diagnostic stages (args[6] 7..9), used after the display is up:
 *   7  build one launcher frame, no present
 *   8  present an empty frame (clear only)
 *   9  one full frame: input poll + draw + present
 * Each returns cleanly so the host reads the log ring. */
static int launcher_frame_probe(const struct hbx_ctx *ctx, uint32_t stage) {
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
                g_state.time = (float)((double)hbx_now_ns(ctx) * 1e-9);
                launcher_handle_input(&g_state, &input, &nav);
            }
            hbx_ui_engine_begin(&g_display.ui, 0xfffcf9f9u);
            if (stage >= 14u) hbx_launcher_draw(&g_display.ui, &g_state);
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
            g_state.time = (float)((double)hbx_now_ns(ctx) * 1e-9);
            launcher_handle_input(&g_state, &input, &nav);
            hbx_ui_engine_begin(&g_display.ui, 0xfffcf9f9u);
            hbx_launcher_draw(&g_display.ui, &g_state);
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
        hbx_ui_engine_begin(&g_display.ui, 0xfffcf9f9u);
        hbx_launcher_draw(&g_display.ui, &g_state);
        hbx_logf(ctx, "frame: draw ok (%u vertices)",
                 (unsigned)hbx_ui_engine_vertex_count(&g_display.ui));
    } else {
        hbx_ui_engine_begin(&g_display.ui, 0xfffcf9f9u);
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

    if (status != HBX_OK) return status;
    hbx_memset(&input, 0, sizeof input);

    max_frames = (uint32_t)ctx->args[3];
    if (max_frames == 0 || max_frames > 36000u) max_frames = 720;

    if (!g_initialized) {
        static const char *const stages[] = {
            "ok", "arguments", "dependencies", "instance", "physical-device",
            "vi-surface", "queue-family", "device", "swapchain", "ui-init", "attach"
        };
        int created;
        hbx_log(ctx, "hbl: first entry, creating display");
        uint32_t stage_limit = (uint32_t)ctx->args[6];
        created = launcher_display_create(&g_display, ctx, stage_limit);
        if (created >= 100) {
            hbx_logf(ctx, "hbl: stage %d complete (diagnostic stop)", created - 100);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }
        if (created != HBX_OK) {
            const char *stage = (created <= 0 && -created < (int)(sizeof stages / sizeof stages[0]))
                ? stages[-created] : "unknown";
            hbx_logf(ctx, "hbl: display create failed at %s (status %d, vk %d)", stage,
                     created, (int)g_display.last_result);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_ERR_GPU;
            return HBX_ERR_GPU;
        }
        hbx_ui_publish(&g_display.ui, &g_render);
        g_state.abi = 1;
        g_state.selected = 0;
        g_state.mode = LAUNCHER_MODE_LIBRARY;
        g_state.count = 0;
        g_state.refresh_spin = 0.35f;  /* still badge, not a spinning emblem */
        g_state.row_highlight[0] = 1.0f;
        g_initialized = 1;
        hbx_logf(ctx, "hbl: display ready %ux%u, %u images, format %u", g_display.width,
                 g_display.height, g_display.image_count, (unsigned)g_display.format);
        publish_gpu(ctx);
    }

    /* Exclusive-app handshake: release the display and return at once, or
     * rebuild it on the first entry after such an app. */
    {
        struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
        if (control && (control->flags & HBX_CTL_FLAG_RELEASE_DISPLAY)) {
            launcher_display_release(&g_display, ctx);
            control->flags &= ~HBX_CTL_FLAG_RELEASE_DISPLAY;
            publish_gpu(ctx);
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }
        if (g_display.released) {
            if (launcher_display_reacquire(&g_display, ctx) != HBX_OK) {
                hbx_logf(ctx, "hbl: display reacquire failed (vk %d)", (int)g_display.last_result);
                ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_ERR_GPU;
                return HBX_ERR_GPU;
            }
            publish_gpu(ctx);
        }
    }

    /* Refresh catalog pointers (the host rebuilds them when packages change). */
    g_state.catalog = (const struct hbx_catalog *)(uintptr_t)ctx->catalog;
    {
        const char *strings = NULL;
        uint32_t strings_bytes = 0;
        uint32_t count = validate_catalog(ctx, g_state.catalog, &strings, &strings_bytes);
        if (count == 0 && ctx->catalog) hbx_log(ctx, "hbl: catalog rejected by bounds checks");
        g_state.count = count;
        g_state.strings = strings;
        g_state.strings_bytes = strings_bytes;

        /* Rescan handshake.  Once the host has rebuilt the catalog (a new
         * block or a new count), drop the request, the spin and bit 1; bit 0
         * stays untouched because it is the host's stop request. */
        {
            struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
            const struct hbx_catalog *catalog =
                (const struct hbx_catalog *)(uintptr_t)ctx->catalog;
            if (g_refresh_active &&
                (catalog != g_refresh_catalog || count != g_refresh_count)) {
                hbx_log(ctx, "hbl: catalog rewritten, refresh complete");
                g_refresh_active = 0;
                g_refresh_yielded = 0;
                g_refresh_frames = 0;
                g_state.refreshing = 0;
                if (control) control->flags &= ~HBX_CTL_FLAG_REFRESH;
            } else if (g_refresh_active) {
                /* Still pending: keep the host request latched. */
                if (control) control->flags |= HBX_CTL_FLAG_REFRESH;
            } else {
                /* No request in flight; clear any stale bit from a previous
                 * session.  Clearing bit 1 never touches the stop bit. */
                g_state.refreshing = 0;
                if (control) control->flags &= ~HBX_CTL_FLAG_REFRESH;
            }
        }
    }
    if (g_state.selected >= g_state.count) g_state.selected = g_state.count ? g_state.count - 1 : 0;
    if (g_state.last_index >= g_state.count) g_state.last_index = 0;
    /* A launch request that was never consumed (the host re-entered without a
       resume) must not fire again on the first frame of this entry. */
    if (g_state.mode == LAUNCHER_MODE_LAUNCH) {
        g_state.mode = LAUNCHER_MODE_LIBRARY;
        g_state.transition = 0.0f;
    }
    publish_gpu(ctx);

    /* Icons can change between host rebuilds; re-upload when the set changes. */
    if (ctx->icons && g_state.catalog && g_state.catalog->icon_count) {
        uint32_t layers = g_state.catalog->icon_count;
        uint32_t signature = (uint32_t)ctx->icons ^ (layers * 2654435761u);
        if (signature != g_icon_signature) {
            if (layers > HBX_MAX_ICON_LAYERS) layers = HBX_MAX_ICON_LAYERS;
            hbx_ui_set_icons(&g_display.ui, (const void *)(uintptr_t)ctx->icons, layers);
            g_icon_signature = signature;
        }
    }

    /* Resume / error presentation. */
    if (ctx->flags & HBX_FLAG_RESUME) {
        uint32_t index = (uint32_t)ctx->args[1] ? (uint32_t)ctx->args[1] - 1u : 0;
        uint64_t app_status = ctx->args[2];
        if (ctx->args[1] && app_status != 0 && app_status != (uint64_t)HBX_ERR_STOPPED) {
            g_error[0] = 0;
            append_string(g_error, sizeof g_error, catalog_name(&g_state, index));
            append_string(g_error, sizeof g_error, " exited with status ");
            append_uint(g_error, sizeof g_error, (uint32_t)app_status);
            g_state.error_text[0] = 0;
            copy_string(g_state.error_text, sizeof g_state.error_text, g_error);
            g_state.mode = LAUNCHER_MODE_ERROR;
            g_state.transition = 1.0f;
            g_state.fade = 1.0f;      /* fade the launcher back in */
            g_state.last_index = index;
        } else if (ctx->args[1]) {
            build_toast("Returned from ", catalog_name(&g_state, index));
            g_state.toast_frames = 150;
            g_state.selected = index;
            g_state.mode = LAUNCHER_MODE_LIBRARY;
            g_state.transition = 0.0f;
            g_state.fade = 1.0f;      /* fade the launcher back in */
        }
    }
    if (g_state.selected >= g_state.count) g_state.selected = g_state.count ? g_state.count - 1 : 0;
    if (g_state.last_index >= g_state.count) g_state.last_index = 0;
    g_last_sequence = (uint32_t)ctx->args[0];

    {
        uint32_t stage = (uint32_t)ctx->args[6];
        if (stage >= 7u && stage <= 15u) {
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = (uint64_t)launcher_frame_probe(ctx, stage);
            return (int)((struct hbx_ctx *)(uintptr_t)ctx)->status;
        }
    }

    while (frames < max_frames) {
        struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;

        hbx_input_poll(ctx, &input);
        /* The proven launcher's loop discipline: fixed 15 ms sleep after each
         * present and animation time derived from the frame counter.  No tick
         * reads and no floating-point timestamp conversion per frame. */
        g_state.fps_milli = fps_milli;
        g_state.time = (float)frames * (1.0f / 60.0f);
        g_state.frame = frames;
        if (g_state.toast_frames) g_state.toast_frames--;
        copy_string(g_state.toast, sizeof g_state.toast, g_toast);
        if (g_state.toast_frames == 0) g_toast[0] = 0;

        if (g_state.mode == LAUNCHER_MODE_LAUNCH) {
            g_state.transition += 0.14f;
            if (g_state.transition >= 1.0f) g_state.transition = 1.0f;
        } else if (g_state.mode == LAUNCHER_MODE_ERROR) {
            g_state.transition += 0.10f;
            if (g_state.transition >= 1.0f) g_state.transition = 1.0f;
        }

        launcher_handle_input(&g_state, &input, &nav);
        launcher_step_animation(&g_state);

        /* Y was pressed: latch the request against the catalog we are showing
         * and raise bit 1 of the control page for the host to service. */
        if (g_state.refreshing) {
            struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
            if (!g_refresh_active) {
                g_refresh_active = 1;
                g_refresh_yielded = 0;
                g_refresh_frames = 0;
                g_refresh_catalog = g_state.catalog;
                g_refresh_count = g_state.count;
                hbx_logf(ctx, "hbl: refresh requested (%u titles)", (unsigned)g_state.count);
            }
            if (control) control->flags |= HBX_CTL_FLAG_REFRESH;
            g_refresh_frames++;
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

        hbx_ui_engine_begin(&g_display.ui, 0xfffcf9f9u);
        hbx_launcher_draw(&g_display.ui, &g_state);

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
            control->frame = g_display.ui.frame;
            control->fps_milli = fps_milli;
            /* Never clear control->flags: bit 0 is the host's stop request. */
        }

        /* Give the host a chance to service the rescan instead of spinning
         * "Refreshing..." for the whole frame budget.  Yield once per request;
         * re-entry keeps the bit latched until the catalog block changes. */
        if (g_refresh_active && !g_refresh_yielded && g_refresh_frames >= 20u &&
            g_state.mode == LAUNCHER_MODE_LIBRARY) {
            g_refresh_yielded = 1;
            hbx_log(ctx, "hbl: yielding to host for catalog rescan");
            ((struct hbx_ctx *)(uintptr_t)ctx)->status = HBX_OK;
            return HBX_OK;
        }

        if (g_state.mode == LAUNCHER_MODE_LAUNCH && g_state.transition >= 1.0f) {
            if (!g_state.catalog || g_state.last_index >= g_state.count) {
                /* The catalog changed underneath the request; go back to the
                   library instead of dereferencing a stale index. */
                hbx_log(ctx, "hbl: launch request dropped (catalog changed)");
                g_state.mode = LAUNCHER_MODE_LIBRARY;
                g_state.transition = 0.0f;
            } else {
                if (control) {
                    control->launch = g_state.last_index + 1u;
                    control->launch_hash = g_state.catalog->entries[g_state.last_index].id_hash;
                    control->last_status = 0;
                }
                hbx_logf(ctx, "hbl: launch request %u", g_state.last_index + 1u);
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
