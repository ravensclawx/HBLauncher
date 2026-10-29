/*
 * hbx_sdk.h - HBLauncher Native SDK application ABI (version 3)
 *
 * An SDK app is a freestanding AArch64 image with two loadable parts:
 *
 *   code.bin   read+execute at a fixed, page-aligned base chosen by the loader
 *   data.bin   read+write at a fixed, page-aligned base chosen by the loader
 *
 * The loader maps both, zeroes the .bss tail, places the package assets after
 * the static data, and calls the app entry point with a pointer to
 * `struct hbx_ctx` in x0.  The entry returns an `hbx_status` in w0.  The app
 * may also request an early exit by writing `HBX_EXIT_REQUEST` to ctx->exit.
 *
 * Everything in this header is layout-stable.  Offsets are asserted at compile
 * time; the host loader re-derives them from this file's generated JSON
 * (tools/abi-export.py) and refuses to run against a mismatched ABI.
 *
 * No libc, no dynamic linking, no writable globals beyond the mapped data
 * region.  The SDK runtime is freestanding and uses only this ABI.
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#ifndef HBX_SDK_H
#define HBX_SDK_H

#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert     /* C++ users (Linux-personality apps) */
#endif

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Identity and limits                                                */
/* ------------------------------------------------------------------ */

/* "HBX3" in little-endian byte order. */
#define HBX_ABI_MAGIC        0x33425848u
#define HBX_ABI_VERSION      3u
#define HBX_ABI_MIN_VERSION  3u

#define HBX_MAX_IMAGES       8u
#define HBX_MAX_TOUCH        16u
#define HBX_MAX_CATALOG      64u
#define HBX_MAX_ICON_LAYERS  16u
#define HBX_ICON_SIZE        96u
#define HBX_MAX_ASSETS       64u
#define HBX_PATH_MAX         96u

/* Catalog entry kinds. */
#define HBX_KIND_NATIVE      0u
#define HBX_KIND_NES         1u
#define HBX_KIND_SM64        2u

/* ctx->flags bits. */
#define HBX_FLAG_RESUME      0x00000001u  /* entry follows an app return */
#define HBX_FLAG_DEBUG       0x00000002u  /* verbose logging requested */
#define HBX_FLAG_NO_AV       0x00000004u  /* audio/input stripped build */

/* ctx->exit values. */
#define HBX_EXIT_NONE        0u
#define HBX_EXIT_REQUEST     1u

/* Control page flags/commands (host-readable, app-writable). */
#define HBX_CTL_MAGIC        0x4c544348u  /* "HCTL" */
#define HBX_CTL_LAUNCH_NONE  0u
#define HBX_CTL_LAUNCH_EXIT  0xffffffffu

/* Status codes shared by the launcher and apps. */
typedef enum hbx_status {
    HBX_OK                = 0,
    HBX_ERR_ARG           = 1,
    HBX_ERR_ABI           = 2,
    HBX_ERR_HEAP          = 3,
    HBX_ERR_GPU           = 4,
    HBX_ERR_ASSET         = 5,
    HBX_ERR_TIMEOUT       = 6,
    HBX_ERR_UNSUPPORTED   = 7,
    HBX_ERR_STOPPED       = 8,
    HBX_ERR_INTERNAL      = 9
} hbx_status;

/* ------------------------------------------------------------------ */
/* Basic shared blocks                                                */
/* ------------------------------------------------------------------ */

/* 64-byte controller block written by a nn::hid GetNpadState* entrypoint.
 * Layout matches the archived HB CPU SDK and the SM64 NES-port mapping:
 *   +0  u64 reserved0
 *   +8  u32 buttons, in nn::hid NpadButton bit order:
 *       A=0x00001 B=0x00002 X=0x00004 Y=0x00008
 *       StickL=0x00010 StickR=0x00020 L=0x00040 R=0x00080
 *       ZL=0x00100 ZR=0x00200 Plus=0x00400 Minus=0x00800
 *       Left=0x01000 Up=0x02000 Right=0x04000 Down=0x08000
 *       LeftSL=0x10000 LeftSR=0x20000 RightSL=0x40000 RightSR=0x80000
 *   +12 u32 reserved1
 *   +16 s32 left stick x, +20 y, +24 right stick x, +28 y
 *   +32 u32 connected (bit 0)
 */
struct hbx_pad_state {
    uint64_t reserved0;
    uint32_t buttons;
    uint32_t reserved1;
    int32_t  stick_lx;
    int32_t  stick_ly;
    int32_t  stick_rx;
    int32_t  stick_ry;
    uint32_t connected;
    uint32_t reserved2[7];
};
_Static_assert(sizeof(struct hbx_pad_state) == 64, "hbx_pad_state");

enum hbx_button {
    HBX_A        = 0x00001,
    HBX_B        = 0x00002,
    HBX_X        = 0x00004,
    HBX_Y        = 0x00008,
    HBX_STICK_L  = 0x00010,
    HBX_STICK_R  = 0x00020,
    HBX_L        = 0x00040,
    HBX_R        = 0x00080,
    HBX_ZL       = 0x00100,
    HBX_ZR       = 0x00200,
    HBX_PLUS     = 0x00400,
    HBX_MINUS    = 0x00800,
    HBX_LEFT     = 0x01000,
    HBX_UP       = 0x02000,
    HBX_RIGHT    = 0x04000,
    HBX_DOWN     = 0x08000,
    HBX_SL       = 0x10000,
    HBX_SR       = 0x20000
};

/*
 * Raw block passed to nn::hid::GetTouchScreenState<16>.  The exact layout is
 * the system's, not ours, so the SDK treats it as bytes and reads it with
 * explicit offsets:
 *
 *   nn::hid (libnx-compatible)         legacy 40-byte variant
 *   count   @ 0x10                     count   @ 0x08
 *   entries @ 0x18, stride 32          entries @ 0x18, stride 40
 *   entry:  +0 attributes, +4 finger id, +8 x, +12 y
 *
 * The SDK tries the 32-byte layout first and falls back when the count is
 * implausible.  664 bytes covers the larger variant.
 */
#define HBX_TOUCH_STATE_BYTES 664u
struct hbx_touch_state {
    unsigned char raw[HBX_TOUCH_STATE_BYTES];
};
_Static_assert(sizeof(struct hbx_touch_state) == HBX_TOUCH_STATE_BYTES, "hbx_touch_state");

struct hbx_touch_point {
    uint32_t id;
    int32_t  x;   /* 0..1279 */
    int32_t  y;   /* 0..719  */
};

/* ------------------------------------------------------------------ */
/* Catalog and assets (host-written, read-only to the app)            */
/* ------------------------------------------------------------------ */

struct hbx_catalog_entry {
    uint32_t id_hash;    /* FNV-1a 32 of the manifest id */
    uint32_t name_off;   /* byte offset into the catalog string pool */
    uint32_t desc_off;
    uint32_t kind;       /* HBX_KIND_* */
    uint32_t version;    /* (major << 16) | (minor << 8) | patch */
    uint32_t flags;
    uint32_t icon_layer; /* index into the icon texture array, or -1 */
    uint32_t abi;        /* app ABI version */
};
_Static_assert(sizeof(struct hbx_catalog_entry) == 32, "catalog entry");

struct hbx_catalog {
    uint32_t magic;      /* HBX_CATALOG_MAGIC */
    uint32_t count;      /* <= HBX_MAX_CATALOG */
    uint32_t strings_off;
    uint32_t strings_bytes;
    uint32_t icon_count;
    uint32_t icon_available; /* 1 when the loader uploaded icon pixels */
    uint32_t reserved[2];
    struct hbx_catalog_entry entries[HBX_MAX_CATALOG];
};
#define HBX_CATALOG_MAGIC 0x47415443u /* "CTAG" */

/* Package asset table, placed by the loader at data_base + 0 and addressed by
 * ctx->assets.  Name and data offsets are relative to ctx->data_base. */
struct hbx_asset_entry {
    uint32_t name_off;
    uint32_t data_off;
    uint32_t size;
    uint32_t flags;      /* bit 0: 4-byte aligned pixel data */
};
struct hbx_asset_table {
    uint32_t magic;      /* HBX_ASSET_MAGIC */
    uint32_t count;
    uint32_t reserved[2];
    struct hbx_asset_entry entries[HBX_MAX_ASSETS];
};
#define HBX_ASSET_MAGIC 0x54415341u /* "ASAT" */

/* Persistent save block.  A package that declares "save_bytes" gets a RAM
 * block at ctx->args[6] (size in ctx->args[7]) preloaded by the host from
 * saves/<id>.sav on the PC.  When the app returns with `dirty` set, the host
 * writes the first `bytes` bytes back.  Nothing is written on the console.
 * A block that does not start with HBX_SAVE_MAGIC is fresh: initialise it. */
struct hbx_save_header {
    uint32_t magic;      /* HBX_SAVE_MAGIC */
    uint32_t version;    /* app-defined layout version */
    uint32_t bytes;      /* bytes in use, header included */
    uint32_t dirty;      /* non-zero: the host persists the block */
};
#define HBX_SAVE_MAGIC 0x56534248u /* "HBSV" */

/* ------------------------------------------------------------------ */
/* Vulkan handoff                                                     */
/* ------------------------------------------------------------------ */

/*
 * The launcher owns the Vulkan display: instance, physical device, device,
 * queue, vi surface and the 1920x1080 FIFO swapchain (plus its image views).
 * Apps borrow that display.  They may create their own pipelines, buffers,
 * descriptors, command pools and per-image semaphores through `dispatch`,
 * draw into `views[index]`, and present with queue submit + present.
 *
 * An app must not destroy, recreate or resize the swapchain or its views, and
 * must not call queuePresentKHR without a matching acquire from the same
 * queue.  The loader waits for the queue to go idle between app and launcher.
 */
struct hbx_gpu {
    uint64_t instance;
    uint64_t physical;
    uint64_t device;
    uint64_t queue;
    uint32_t queue_family;
    uint32_t image_count;
    uint32_t width;              /* 1920 */
    uint32_t height;             /* 1080 */
    uint32_t color_format;       /* VkFormat of the swapchain images */
    uint32_t color_space;
    uint64_t swapchain;
    uint64_t images[HBX_MAX_IMAGES];
    uint64_t views[HBX_MAX_IMAGES];
    uint64_t dispatch;           /* struct hbx_vk_dispatch * */
    uint64_t vi_window;          /* vi native window (0 when offscreen) */
    uint64_t create_vi_surface;  /* vkCreateViSurfaceNN entrypoint */
    uint64_t destroy_surface;    /* vkDestroySurfaceKHR entrypoint */
    uint64_t surface;            /* vi surface owned by the display owner */
    uint64_t present_state;      /* 256-byte presenter state block */
    uint64_t present_state_bytes;
    uint64_t reserved;
};

/* ------------------------------------------------------------------ */
/* Host services                                                      */
/* ------------------------------------------------------------------ */

typedef void (*hbx_fn_pad)(struct hbx_pad_state *state, const uint32_t *id);
typedef void (*hbx_fn_touch)(struct hbx_touch_state *state);

struct hbx_input {
    uint64_t pad_handheld;   /* GetNpadState(NpadHandheld)  id 32 */
    uint64_t pad_full_key;   /* GetNpadState(NpadFullKey)   id 0  */
    uint64_t pad_joy_dual;   /* GetNpadState(NpadJoyDual)   id 0  */
    uint64_t pad_joy_left;
    uint64_t pad_joy_right;
    uint64_t touch;          /* GetTouchScreenState<16>           */
    uint64_t pad_state;      /* 64-byte host scratch             */
    uint64_t touch_state;    /* 664-byte host scratch            */
};

struct hbx_log {
    uint64_t ring;
    uint32_t bytes;
    uint32_t head;
};

struct hbx_control {
    uint32_t magic;          /* HBX_CTL_MAGIC */
    uint32_t launch;         /* launcher -> host: 0 none, 1..N index, EXIT */
    uint32_t launch_hash;    /* id hash of the requested title             */
    uint32_t last_status;    /* last app/host status                       */
    uint32_t frame;          /* current presentation frame                 */
    uint32_t flags;
    uint32_t fps_milli;      /* measured frame rate x 1000                 */
    uint32_t reserved0;
    uint64_t heartbeat;      /* monotonic tick of the last presented frame */
    uint64_t frames_total;
    uint64_t launches;
    uint64_t reserved1[5];
};
_Static_assert(sizeof(struct hbx_control) == 96, "hbx_control");

/* ------------------------------------------------------------------ */
/* Audio (host-resolved nn::audio AudioOut entrypoints)               */
/* ------------------------------------------------------------------ */

struct hbx_audio {
    uint64_t initialize_parameter;
    uint64_t open_default_audio_out;
    uint64_t sample_rate;
    uint64_t channel_count;
    uint64_t sample_format;
    uint64_t set_buffer_info;
    uint64_t append_buffer;
    uint64_t start;
    uint64_t stop;
    uint64_t buffer_count;
    uint64_t flush_buffers;
    uint64_t released_buffer;
    uint64_t close;              /* CloseAudioOut: the device has a destructor */
};
_Static_assert(sizeof(struct hbx_audio) == 104, "hbx_audio size");

/* ------------------------------------------------------------------ */
/* The application context                                            */
/* ------------------------------------------------------------------ */

struct hbx_ctx {
    /* 0x000 */ uint32_t magic;
    /* 0x004 */ uint32_t abi_version;
    /* 0x008 */ uint32_t size;          /* sizeof(struct hbx_ctx) */
    /* 0x00C */ uint32_t flags;         /* HBX_FLAG_* */
    /* 0x010 */ uint64_t status;        /* out: last app status */
    /* 0x018 */ uint64_t exit;          /* out: HBX_EXIT_REQUEST stops the app */
    /* 0x020 */ uint64_t frame;         /* out: frames presented by this entry */
    /* 0x028 */ uint64_t now_ns;        /* in: monotonic ns at entry */
    /* 0x030 */ uint64_t tick_hz;       /* in: CNTFRQ_EL0, 0 if unknown */
    /* 0x038 */ uint64_t uptime_ns;     /* in: ns since the launcher started */
    /* 0x040 */ uint64_t heap_base;
    /* 0x048 */ uint64_t heap_size;
    /* 0x050 */ uint64_t data_base;    /* package asset region (loader-owned RW) */
    /* 0x058 */ uint64_t data_size;   /* bytes in that region */
    /* 0x060 */ uint64_t code_base;
    /* 0x068 */ uint64_t code_size;
    /* 0x070 */ uint64_t stack_base;    /* host-provided app stack */
    /* 0x078 */ uint64_t stack_size;
    /* 0x080 */ uint64_t control;       /* struct hbx_control * (host RW) */
    /* 0x088 */ uint64_t catalog;       /* struct hbx_catalog * */
    /* 0x090 */ uint64_t catalog_bytes;
    /* 0x098 */ uint64_t icons;         /* RGBA8, icon_count * 96 * 96 * 4 */
    /* 0x0A0 */ uint64_t assets;        /* struct hbx_asset_table * or 0 */
    /* 0x0A8 */ uint64_t log;           /* struct hbx_log * */
    /* 0x0B0 */ uint64_t ui;            /* struct hbx_render * (launcher UI) */
    /* 0x0B8 */ uint64_t atlas;         /* R8 glyph atlas pixels (1024x1024) */
    /* 0x0C0 */ uint64_t atlas_bytes;
    /* 0x0C8 */ struct hbx_gpu gpu;
    /* 0x1C8 */ struct hbx_input input;
    /* 0x208 */ uint64_t args[8];      /* launcher/A entry arguments */
    /* 0x248 */ uint64_t icons_bytes;    /* bytes available at ctx->icons */
    /* 0x250 */ struct hbx_audio audio;  /* zero when the host has no audio */
};

_Static_assert(sizeof(struct hbx_gpu) == 0x100, "hbx_gpu size");
_Static_assert(offsetof(struct hbx_ctx, gpu) == 0x0C8, "ctx.gpu");
_Static_assert(offsetof(struct hbx_ctx, input) == 0x1C8, "ctx.input");
_Static_assert(offsetof(struct hbx_ctx, args) == 0x208, "ctx.args");
_Static_assert(offsetof(struct hbx_ctx, audio) == 0x250, "ctx.audio");
_Static_assert(sizeof(struct hbx_ctx) == 0x2B8, "hbx_ctx size");

/* The entry point every SDK app exports. */
typedef int (*hbx_app_entry)(const struct hbx_ctx *ctx);
int hbx_app_main(const struct hbx_ctx *ctx);

/* ------------------------------------------------------------------ */
/* Inline helpers (no libc)                                           */
/* ------------------------------------------------------------------ */

static inline void hbx_memset(void *dst, int value, size_t bytes) {
    unsigned char *p = (unsigned char *)dst;
    while (bytes--) *p++ = (unsigned char)value;
}

static inline void hbx_memcpy(void *dst, const void *src, size_t bytes) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (bytes--) *d++ = *s++;
}

static inline int hbx_memcmp(const void *a, const void *b, size_t bytes) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (bytes--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

static inline size_t hbx_strlen(const char *s) {
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static inline int hbx_streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline uint32_t hbx_hash32(const char *s) {
    uint32_t h = 2166136261u;
    while (s && *s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* Link-time aligned placement. */
#define HBX_ALIGN_UP(value, align) \
    (((uint64_t)(value) + (uint64_t)(align) - 1u) & ~((uint64_t)(align) - 1u))

#endif /* HBX_SDK_H */
