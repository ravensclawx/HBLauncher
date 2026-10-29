/*
 * hbx_app.h - HBLauncher Native SDK runtime helpers.
 *
 * Thin, freestanding helpers over the ABI in hbx_sdk.h: context validation,
 * input polling with edge detection and deadzones, a boundary-tag heap, time
 * and frame pacing, package assets, and a fixed-format logger.
 *
 * All functions are implemented in sdk/src/hbx_app.c and linked into the app
 * image.  They never call libc.
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#ifndef HBX_APP_H
#define HBX_APP_H

#include "hbx_sdk.h"

/* ------------------------------------------------------------------ */
/* Context                                                            */
/* ------------------------------------------------------------------ */

/* Returns HBX_OK when the context magic/version/size are usable. */
hbx_status hbx_ctx_check(const struct hbx_ctx *ctx);

/* Monotonic nanoseconds since the launcher started (ctx->uptime_ns plus the
 * hardware counter delta).  Never goes backwards. */
uint64_t hbx_now_ns(const struct hbx_ctx *ctx);

/* Raw CNTFRQ/CNTVCT tick, in the units of ctx->tick_hz. */
uint64_t hbx_tick(void);

/* Park the calling thread; nanoseconds, firmware sleep (SVC 0x0b). */
void hbx_sleep_ns(uint64_t nanoseconds);

/* Absolute 60/30 Hz pacing.  Call once per frame with a deadline variable
 * initialised to 0; the helper advances it by the exact period and sleeps only
 * the remaining time.  Returns the frame's start time in ns. */
uint64_t hbx_pace(const struct hbx_ctx *ctx, uint64_t *deadline, uint32_t hz);

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

struct hbx_input_state {
    uint32_t buttons;      /* held, HBX_* bits */
    uint32_t pressed;      /* edge this poll */
    uint32_t released;     /* falling edge this poll */
    int32_t  stick_lx;     /* -100..100, deadzoned */
    int32_t  stick_ly;
    int32_t  stick_rx;
    int32_t  stick_ry;
    uint32_t connected;    /* pad sources reporting connected */
    uint32_t touch_count;
    struct hbx_touch_point touch[HBX_MAX_TOUCH];
    /* private edge storage */
    uint32_t previous;
    uint32_t reserved[3];
};

/* Poll every configured pad source and the touchscreen.  Pads are OR-ed;
 * sticks come from the first connected source and use a radial deadzone. */
void hbx_input_poll(const struct hbx_ctx *ctx, struct hbx_input_state *state);

/* 16.16 fixed-point helper: 1 when the direction is held past `threshold`
 * (0..100).  Handy for repeat-on-hold menu navigation. */
int hbx_input_nav(const struct hbx_input_state *state, int *dx, int *dy, int *confirm, int *cancel);

/* True when a touch point lies inside a 1920x1080-space rectangle.  Touch
 * coordinates arrive in 1280x720 space and are scaled 3:2 by the helper. */
int hbx_touch_hit(const struct hbx_touch_point *point,
                  int32_t x, int32_t y, int32_t width, int32_t height);

/* ------------------------------------------------------------------ */
/* Heap                                                               */
/* ------------------------------------------------------------------ */

struct hbx_heap {
    uint64_t base;
    uint64_t size;
    uint64_t used;
    uint64_t peak;
    uint64_t first_free;   /* offset of the first free block, or 0 */
    uint32_t blocks;
    uint32_t magic;        /* HBX_HEAP_MAGIC when initialised */
};

#define HBX_HEAP_MAGIC 0x50414548u /* "HEAP" */

void  hbx_heap_init(struct hbx_heap *heap, void *base, uint64_t size);
void *hbx_alloc(struct hbx_heap *heap, uint64_t bytes);           /* 16-byte aligned */
void *hbx_alloc_aligned(struct hbx_heap *heap, uint64_t bytes, uint64_t alignment);
void  hbx_free(struct hbx_heap *heap, void *pointer);
uint64_t hbx_heap_used(const struct hbx_heap *heap);
uint64_t hbx_heap_peak(const struct hbx_heap *heap);

/* ------------------------------------------------------------------ */
/* Assets and log                                                     */
/* ------------------------------------------------------------------ */

/* Find a package asset by path (e.g. "levels/one.bin").  Returns NULL when the
 * loader did not provide it. */
const void *hbx_asset(const struct hbx_ctx *ctx, const char *name, uint32_t *size);

/* SDK imports (manifest "imports").  Prefer the host's verified table in
 * ctx->args[0]; otherwise derive the SDK module base from a host-resolved
 * audio entrypoint (InitializeAudioOutParameter sits at module offset
 * 0x1c7f30) and add the build-time offsets from hbx_imports.h.  Returns 2 for
 * a host table, 1 for derived addresses, 0 when neither is available. */
#define HBX_SDK_OFFSET_AUDIO_INIT 0x1c7f30ull
static inline int hbx_imports_resolve(const struct hbx_ctx *ctx, const uint64_t *offsets,
                                      uint64_t count, uint64_t *out) {
    const uint64_t *table = (const uint64_t *)(uintptr_t)ctx->args[0];
    uint64_t i, base;
    if (table && table[0] == count) {
        for (i = 0; i < count; i++) out[i] = table[i + 1];
        return 2;
    }
    if (!ctx->audio.initialize_parameter) return 0;
    base = ctx->audio.initialize_parameter - HBX_SDK_OFFSET_AUDIO_INIT;
    for (i = 0; i < count; i++) out[i] = base + offsets[i];
    return 1;
}

/* The package's persistent save block (see struct hbx_save_header), or NULL
 * when the package declares no "save_bytes". */
static inline struct hbx_save_header *hbx_save_block(const struct hbx_ctx *ctx,
                                                     uint32_t *size) {
    if (size) *size = (uint32_t)ctx->args[7];
    return ctx->args[6] && ctx->args[7] >= sizeof(struct hbx_save_header)
        ? (struct hbx_save_header *)(uintptr_t)ctx->args[6] : (struct hbx_save_header *)0;
}

/* Append one NUL-terminated line to the host-visible log ring. */
void hbx_log(const struct hbx_ctx *ctx, const char *line);
void hbx_logf(const struct hbx_ctx *ctx, const char *format, ...);

/* ------------------------------------------------------------------ */
/* Control                                                            */
/* ------------------------------------------------------------------ */

static inline int hbx_stop_requested(const struct hbx_ctx *ctx) {
    const struct hbx_control *control = (const struct hbx_control *)(uintptr_t)ctx->control;
    return ctx->exit != HBX_EXIT_NONE || (control && control->flags & 1u);
}

static inline void hbx_request_exit(const struct hbx_ctx *ctx) {
    ((struct hbx_ctx *)(uintptr_t)ctx)->exit = HBX_EXIT_REQUEST;
}

static inline void hbx_frame_end(const struct hbx_ctx *ctx) {
    struct hbx_control *control = (struct hbx_control *)(uintptr_t)ctx->control;
    if (control) {
        control->frame++;
        control->frames_total++;
    }
    ((struct hbx_ctx *)(uintptr_t)ctx)->frame++;
}

#endif /* HBX_APP_H */
