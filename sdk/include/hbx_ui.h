/*
 * hbx_ui.h - HBLauncher Native 2D UI ABI.
 *
 * The launcher owns the display and a GPU 2D renderer.  It publishes a
 * function table (`struct hbx_render`, ctx->ui) that apps can use to draw text,
 * rectangles, lines, circles and package icons into the same swapchain, with
 * the same font atlas and the same wave background the launcher uses.
 *
 * The table is a *call-through* ABI: the implementation lives in the launcher
 * image and is never copied into apps.  This keeps struct layouts private and
 * lets the renderer evolve with a single abi field.  Every call is a direct
 * function pointer invoke -- no IPC, no syscalls.
 *
 * Drawing model: immediate mode.  `begin` starts a frame's geometry,
 * the shape calls append triangles, `present` acquires a swapchain image,
 * records and submits one draw of all accumulated geometry, presents, and
 * waits for the queue.  Coordinates are in 1920x1080 pixels, origin at the
 * top-left, +y down.  Colours are 0xAABBGGRR (R in the low byte).
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#ifndef HBX_UI_H
#define HBX_UI_H

#include "hbx_sdk.h"

#define HBX_RENDER_ABI 1u

/* Font faces baked into the shared atlas. */
enum hbx_font {
    HBX_FONT_TITLE = 0,   /* 88 px bold, launcher wordmark */
    HBX_FONT_H1    = 1,   /* 34 px bold, row titles */
    HBX_FONT_H2    = 2,   /* 26 px semibold, section headers */
    HBX_FONT_BODY  = 3,   /* 20 px regular */
    HBX_FONT_SMALL = 4,   /* 17 px regular, descriptions */
    HBX_FONT_TINY  = 5,   /* 14 px regular, footer */
    HBX_FONT_COUNT = 6
};

/* `flags` for hbx_ui_line. */
#define HBX_LINE_SOFT   0x01u  /* feathered edges (default for hairline art) */
#define HBX_LINE_ROUND  0x02u  /* round caps */

/* Icon layer sentinel: draw a rounded tile with the title initial instead. */
#define HBX_ICON_NONE   0xffffffffu

/* Colour helpers. */
#define HBX_RGBA(r, g, b, a) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define HBX_RGB(r, g, b) HBX_RGBA((r), (g), (b), 255)

struct hbx_ui_ops {
    uint32_t abi;
    uint32_t size;

    /* Start a frame.  `background` is 0xAABBGGRR; pass 0 for a transparent
     * clear only if the call site has already cleared. */
    void (*begin)(void *self, uint32_t background);

    /* Filled rounded rectangle.  radius may be 0. */
    void (*rect)(void *self, float x, float y, float width, float height,
                 float radius, uint32_t color);

    /* Rounded rectangle outline of `thickness` pixels. */
    void (*stroke)(void *self, float x, float y, float width, float height,
                   float radius, float thickness, uint32_t color);

    /* Text at the top-left `x`,`y`.  Returns the advance width in pixels. */
    float (*text)(void *self, float x, float y, uint32_t font,
                  const char *utf8, uint32_t color);

    /* Measure text without drawing. */
    float (*text_width)(void *self, uint32_t font, const char *utf8);

    /* Line segment with the given width. */
    void (*line)(void *self, float x0, float y0, float x1, float y1,
                 float width, uint32_t color, uint32_t flags);

    /* Circle or ring.  `thickness` 0 fills. */
    void (*circle)(void *self, float cx, float cy, float radius,
                   float thickness, uint32_t color, uint32_t flags);

    /* App icon from the catalog (96x96, scaled to `size`).  Falls back to a
     * letter tile when the layer is HBX_ICON_NONE. */
    void (*icon)(void *self, float x, float y, float size, uint32_t layer,
                 uint32_t tint);

    /* RGBA8 image (e.g. an emulator framebuffer) scaled into the rectangle.
     * `pixels` is copied during the call; iw/ih must be <= 512. */
    void (*image)(void *self, float x, float y, float width, float height,
                  const void *pixels, uint32_t iw, uint32_t ih);

    /* Triangle list escape hatch: `count` vertices of
     * {x,y,u,v,color,tex} (see struct hbx_ui_vertex). */
    void (*vertices)(void *self, const void *vertices, uint32_t count);

    /* Submit the accumulated frame and present.  Returns HBX_OK, or
     * HBX_ERR_GPU when the frame was dropped. */
    int (*present)(void *self);

    /* Total frames presented by this renderer. */
    uint32_t (*frame)(void *self);

    /* Device/queue/view handles for apps that want to record their own Vulkan
     * work in the same frame. */
    uint64_t (*device)(void *self);
    uint64_t (*queue)(void *self);
};

struct hbx_render {
    uint32_t abi;
    uint32_t size;
    void    *self;
    const struct hbx_ui_ops *ops;
};

/* App-side convenience wrappers.  A NULL renderer is a no-op so apps stay
 * runnable on a host preview without a display. */
static inline const struct hbx_render *hbx_ui_render(const struct hbx_ctx *ctx) {
    const struct hbx_render *r;
    if (!ctx) return NULL;
    r = (const struct hbx_render *)(uintptr_t)ctx->ui;
    /* Reject a stale or mismatched table rather than calling through it. */
    if (!r || r->abi != HBX_RENDER_ABI || r->size < (uint32_t)sizeof *r ||
        !r->self || !r->ops)
        return NULL;
    if (r->ops->abi != HBX_RENDER_ABI ||
        r->ops->size < (uint32_t)sizeof *r->ops)
        return NULL;
    return r;
}

static inline int hbx_ui_available(const struct hbx_ctx *ctx) {
    return hbx_ui_render(ctx) != NULL;
}

static inline void hbx_ui_begin(const struct hbx_ctx *ctx, uint32_t background) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->begin(r->self, background);
}

static inline void hbx_ui_rect(const struct hbx_ctx *ctx, float x, float y,
                               float w, float h, float radius, uint32_t color) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->rect(r->self, x, y, w, h, radius, color);
}

static inline void hbx_ui_stroke(const struct hbx_ctx *ctx, float x, float y,
                                 float w, float h, float radius, float thickness,
                                 uint32_t color) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->stroke(r->self, x, y, w, h, radius, thickness, color);
}

static inline float hbx_ui_text(const struct hbx_ctx *ctx, float x, float y,
                                uint32_t font, const char *text, uint32_t color) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    return r ? r->ops->text(r->self, x, y, font, text, color) : 0.0f;
}

static inline float hbx_ui_text_width(const struct hbx_ctx *ctx, uint32_t font,
                                      const char *text) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    return r ? r->ops->text_width(r->self, font, text) : 0.0f;
}

static inline void hbx_ui_line(const struct hbx_ctx *ctx, float x0, float y0,
                               float x1, float y1, float width, uint32_t color,
                               uint32_t flags) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->line(r->self, x0, y0, x1, y1, width, color, flags);
}

static inline void hbx_ui_circle(const struct hbx_ctx *ctx, float cx, float cy,
                                 float radius, float thickness, uint32_t color,
                                 uint32_t flags) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->circle(r->self, cx, cy, radius, thickness, color, flags);
}

static inline void hbx_ui_icon(const struct hbx_ctx *ctx, float x, float y,
                               float size, uint32_t layer, uint32_t tint) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->icon(r->self, x, y, size, layer, tint);
}

static inline void hbx_ui_image(const struct hbx_ctx *ctx, float x, float y,
                                float w, float h, const void *pixels,
                                uint32_t iw, uint32_t ih) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    if (r) r->ops->image(r->self, x, y, w, h, pixels, iw, ih);
}

static inline int hbx_ui_present(const struct hbx_ctx *ctx) {
    const struct hbx_render *r = hbx_ui_render(ctx);
    return r ? r->ops->present(r->self) : HBX_ERR_UNSUPPORTED;
}

#endif /* HBX_UI_H */
