/*
 * hbx_app.c - HBLauncher Native SDK runtime (freestanding, no libc).
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#include "hbx_app.h"

/* ------------------------------------------------------------------ */
/* Threaded launch                                                    */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__) && !defined(HBX_TEST_NO_SLEEP)
/* The host launches an app with one short call to _spawn (crt0.S), which
 * lands here: create an nn::os thread that runs the image's ordinary entry,
 * then return at once.  When the entry returns, the thread publishes
 * (1 << 32) | status in ctx->status for the host to poll.  Long native calls
 * through the SmileBASIC bridge lost their reply after minutes on hardware;
 * this keeps every bridge call short. */
struct hbx_spawn {
    uint64_t create;             /* nnosCreateThreadWithCoreNumber */
    uint64_t start;              /* nnosStartThread */
    uint64_t thread;             /* nn::os::ThreadType storage (host block) */
    uint64_t stack;              /* thread boot stack (host block) */
    uint64_t stack_size;
    int32_t priority;
    int32_t core;
};

int _start(const struct hbx_ctx *ctx);

static void hbx_thread_main(void *arg) {
    struct hbx_ctx *ctx = (struct hbx_ctx *)arg;
    int status = _start(ctx);
    __atomic_store_n(&ctx->status, (1ull << 32) | (uint32_t)status, __ATOMIC_RELEASE);
}

int hbx_spawn(const struct hbx_spawn *p, const struct hbx_ctx *ctx) {
    typedef int (*create_fn)(void *, void (*)(void *), void *, void *, uint64_t, int, int);
    int r;
    ((struct hbx_ctx *)ctx)->status = 0;
    r = ((create_fn)(uintptr_t)p->create)((void *)(uintptr_t)p->thread, hbx_thread_main,
                                          (void *)ctx, (void *)(uintptr_t)p->stack,
                                          p->stack_size, p->priority, p->core);
    if (r != 0) return r;
    ((void (*)(void *))(uintptr_t)p->start)((void *)(uintptr_t)p->thread);
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__)
__attribute__((noinline)) uint64_t hbx_tick(void) {
#ifdef HBX_TEST_NO_SLEEP
    /* qemu-aarch64 user mode has no Horizon firmware: a monotonic stand-in. */
    static uint64_t synthetic;
    synthetic += 19200;
    return synthetic;
#else
    /* Firmware GetSystemTick (SVC 0x1e) returns the tick in x0.  The result
     * must be bound to x0 and every caller-saved register listed as clobbered:
     * with a plain "=r" output GCC kept a live ctx pointer in x0 across the
     * svc and dereferenced the tick value as a struct pointer (disassembly
     * proved it; that crash killed every app in hbx_pace on hardware). */
    register uint64_t value __asm__("x0");
    __asm__ volatile("svc #0x1e" : "+r"(value) : :
        "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
        "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
    return value;
#endif
}

__attribute__((noinline)) void hbx_sleep_ns(uint64_t nanoseconds) {
#ifdef HBX_TEST_NO_SLEEP
    /* qemu-aarch64 user mode has no Horizon firmware; tests do not park. */
    (void)nanoseconds;
#else
    /* Firmware SleepThread takes the duration in x0.  Same discipline as
     * hbx_tick: bind the argument and declare the caller-saved clobbers. */
    register uint64_t argument __asm__("x0") = nanoseconds;
    __asm__ volatile("svc #0x0b" : "+r"(argument) : :
        "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
        "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
#endif
}

/* CNTFRQ_EL0 is not readable here either; the host measures the tick rate
 * and passes it in ctx->tick_hz.  This is the Switch arch-timer nominal. */
#define HBX_DEFAULT_TICK_HZ 19200000ull

static inline uint64_t hbx_cntfrq(void) { return HBX_DEFAULT_TICK_HZ; }
#else
/* Host preview/test build: the same code paths, backed by the host clock. */
#include <time.h>
uint64_t hbx_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void hbx_sleep_ns(uint64_t nanoseconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)(nanoseconds / 1000000000ull);
    ts.tv_nsec = (long)(nanoseconds % 1000000000ull);
    nanosleep(&ts, NULL);
}
static inline uint64_t hbx_cntfrq(void) { return 1000000000ull; }
#endif

/* Session-relative monotonic time.
 *
 * The first call anchors the hardware counter to the host-provided session
 * time (ctx->now_ns).  Later calls only add the counter delta, so the value
 * stays small and precise instead of mixing "time since boot" with the
 * session origin.  The anchor lives in the image's .bss, which the loader
 * keeps mapped across entries, so time does not jump when the launcher
 * resumes after an app. */
static uint64_t g_time_base_ticks;
static uint64_t g_time_base_ns;
static uint32_t g_time_ready;

uint64_t hbx_now_ns(const struct hbx_ctx *ctx) {
    uint64_t hz = ctx && ctx->tick_hz ? ctx->tick_hz : hbx_cntfrq();
    uint64_t ticks = hbx_tick();
    uint64_t delta;
    if (!g_time_ready) {
        g_time_base_ticks = ticks;
        g_time_base_ns = ctx ? ctx->now_ns : 0;
        g_time_ready = 1;
    }
    if (hz == 0) return g_time_base_ns;
    delta = ticks - g_time_base_ticks;
    return g_time_base_ns + (delta / hz) * 1000000000ull +
           ((delta % hz) * 1000000000ull) / hz;
}

uint64_t hbx_pace(const struct hbx_ctx *ctx, uint64_t *deadline, uint32_t hz) {
    /* The console's vi surface cannot take back-to-back presents: the second
     * present on a freshly acquired image kills the process (verified on
     * hardware, 2026-10-28).  Enforce a minimum gap of 15 ms between frames in
     * addition to the requested period.  The launcher uses the same discipline
     * in its own loop. */
    static uint64_t last_frame_ns;
    const uint64_t minimum_gap = 15000000ull;
    uint64_t period;
    uint64_t now;
    uint64_t target;
    if (hz == 0) hz = 60;
    period = 1000000000ull / hz;
    now = hbx_now_ns(ctx);
    if (deadline == NULL) return now;
    if (*deadline == 0) {
        *deadline = now + period;
    } else {
        *deadline += period;
        if (now > *deadline + period) {
            /* More than a frame behind: resynchronise instead of bursting. */
            *deadline = now + period;
        }
    }
    target = *deadline;
    if (last_frame_ns && target < last_frame_ns + minimum_gap)
        target = last_frame_ns + minimum_gap;
    if (target > now) hbx_sleep_ns(target - now);
    last_frame_ns = hbx_now_ns(ctx);
    return now;
}

/* ------------------------------------------------------------------ */
/* Context                                                            */
/* ------------------------------------------------------------------ */

hbx_status hbx_ctx_check(const struct hbx_ctx *ctx) {
    if (!ctx) return HBX_ERR_ARG;
    if (ctx->magic != HBX_ABI_MAGIC) return HBX_ERR_ABI;
    if (ctx->abi_version < HBX_ABI_MIN_VERSION || ctx->abi_version > HBX_ABI_VERSION)
        return HBX_ERR_ABI;
    if (ctx->size != sizeof(struct hbx_ctx)) return HBX_ERR_ABI;
    return HBX_OK;
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static int32_t deadzone(int32_t raw) {
    /* Raw sticks are roughly -32767..32767.  Radial deadzone is applied by the
     * caller; this maps the magnitude to -100..100. */
    int64_t value = raw;
    if (value > 32767) value = 32767;
    if (value < -32767) value = -32767;
    return (int32_t)(value * 100 / 32767);
}

static uint32_t touch_read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t poll_sources(const struct hbx_ctx *ctx, struct hbx_pad_state *state,
                             int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry,
                             uint32_t *connected) {
    const hbx_fn_pad sources[5] = {
        (hbx_fn_pad)(uintptr_t)ctx->input.pad_handheld,
        (hbx_fn_pad)(uintptr_t)ctx->input.pad_full_key,
        (hbx_fn_pad)(uintptr_t)ctx->input.pad_joy_dual,
        (hbx_fn_pad)(uintptr_t)ctx->input.pad_joy_left,
        (hbx_fn_pad)(uintptr_t)ctx->input.pad_joy_right
    };
    const uint32_t ids[5] = {32u, 0u, 0u, 0u, 0u};
    uint32_t buttons = 0;
    uint32_t live = 0;
    int32_t sx = 0, sy = 0, ex = 0, ey = 0;
    int first = 1;
    uint32_t i;

    if (!state) return 0;
    for (i = 0; i < 5u; i++) {
        hbx_fn_pad source = sources[i];
        uint32_t id = ids[i];
        if (!source) continue;
        hbx_memset(state, 0, sizeof *state);
        source(state, &id);
        if (state->connected & 1u) {
            live++;
            buttons |= state->buttons;
            if (first) {
                sx = state->stick_lx;
                sy = state->stick_ly;
                ex = state->stick_rx;
                ey = state->stick_ry;
                first = 0;
            }
        }
    }
    *connected = live;
    *lx = sx; *ly = sy; *rx = ex; *ry = ey;
    return buttons;
}

void hbx_input_poll(const struct hbx_ctx *ctx, struct hbx_input_state *out) {
    struct hbx_pad_state scratch;
    struct hbx_pad_state *pad = &scratch;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint32_t connected = 0;
    uint32_t buttons;
    int64_t magnitude;

    if (!out) return;
    if (!ctx) {
        hbx_memset(out, 0, sizeof *out);
        return;
    }
    /* Use the host-provided scratch block when it is aligned. */
    if (ctx->input.pad_state && (ctx->input.pad_state & 3u) == 0)
        pad = (struct hbx_pad_state *)(uintptr_t)ctx->input.pad_state;
    hbx_memset(pad, 0, sizeof *pad);
    buttons = poll_sources(ctx, pad, &lx, &ly, &rx, &ry, &connected);

    /* Radial deadzone: 15% of full scale, then rescale to the full range. */
    magnitude = (int64_t)lx * lx + (int64_t)ly * ly;
    if (magnitude <= (int64_t)4915 * 4915) { lx = 0; ly = 0; }
    magnitude = (int64_t)rx * rx + (int64_t)ry * ry;
    if (magnitude <= (int64_t)4915 * 4915) { rx = 0; ry = 0; }

    out->buttons = buttons;
    out->pressed = buttons & ~out->previous;
    out->released = ~buttons & out->previous;
    out->previous = buttons;
    out->stick_lx = deadzone(lx);
    out->stick_ly = deadzone(ly);
    out->stick_rx = deadzone(rx);
    out->stick_ry = deadzone(ry);
    out->connected = connected;

    out->touch_count = 0;
    if (ctx->input.touch && ctx->input.touch_state) {
        unsigned char *touch = (unsigned char *)(uintptr_t)ctx->input.touch_state;
        hbx_fn_touch fn = (hbx_fn_touch)(uintptr_t)ctx->input.touch;
        uint32_t count, stride = 32u, i;
        hbx_memset(touch, 0, HBX_TOUCH_STATE_BYTES);
        fn((struct hbx_touch_state *)(uintptr_t)touch);
        count = touch_read_u32(touch + 0x10);
        if (count > HBX_MAX_TOUCH) {
            /* Older layout: count at +8 and a 40-byte entry stride. */
            uint32_t legacy = touch_read_u32(touch + 0x08);
            if (legacy <= HBX_MAX_TOUCH) {
                count = legacy;
                stride = 40u;
            } else {
                count = 0;
            }
        }
        for (i = 0; i < count; i++) {
            const unsigned char *entry = touch + 0x18 + (uint64_t)i * stride;
            out->touch[i].id = touch_read_u32(entry + 4);
            out->touch[i].x = (int32_t)touch_read_u32(entry + 8);
            out->touch[i].y = (int32_t)touch_read_u32(entry + 12);
        }
        out->touch_count = count;
    }
}

int hbx_input_nav(const struct hbx_input_state *state, int *dx, int *dy,
                  int *confirm, int *cancel) {
    int hx = 0, hy = 0;
    if (!state) return 0;
    if (state->pressed & (HBX_LEFT | HBX_RIGHT | HBX_UP | HBX_DOWN)) {
        if (state->pressed & HBX_LEFT) hx = -1;
        if (state->pressed & HBX_RIGHT) hx = 1;
        if (state->pressed & HBX_UP) hy = -1;
        if (state->pressed & HBX_DOWN) hy = 1;
    } else if (state->stick_lx > 55 || state->stick_lx < -55 ||
               state->stick_ly > 55 || state->stick_ly < -55) {
        /* Sticks report only the dominant axis to avoid diagonal chatter. */
        if (state->stick_lx > 55) hx = 1;
        else if (state->stick_lx < -55) hx = -1;
        else if (state->stick_ly > 55) hy = 1;
        else hy = -1;
    }
    if (dx) *dx = hx;
    if (dy) *dy = hy;
    if (confirm) *confirm = (state->pressed & (HBX_A | HBX_STICK_R | HBX_ZR)) != 0;
    if (cancel) *cancel = (state->pressed & (HBX_B | HBX_MINUS | HBX_ZL)) != 0;
    return hx != 0 || hy != 0;
}

int hbx_touch_hit(const struct hbx_touch_point *point,
                  int32_t x, int32_t y, int32_t width, int32_t height) {
    int32_t tx, ty;
    if (!point) return 0;
    tx = point->x * 3 / 2;
    ty = point->y * 3 / 2;
    return tx >= x && tx < x + width && ty >= y && ty < y + height;
}

/* ------------------------------------------------------------------ */
/* Heap: boundary-tag first-fit with forward/backward coalescing       */
/* ------------------------------------------------------------------ */

#define HEAP_ALIGN 16u
#define HEAP_MIN_SPLIT (HEAP_ALIGN * 4u)

struct heap_block {
    uint64_t size;      /* total block size including this header */
    uint64_t next_free; /* offset of the next free block, or HEAP_NO_BLOCK */
    uint32_t used;
    uint32_t magic;     /* HEAP_BLOCK_MAGIC while live */
    uint64_t padding;   /* keeps sizeof a multiple of HEAP_ALIGN */
};
#define HEAP_BLOCK_MAGIC 0x4b4c4248u /* "HBLK" */
#define HEAP_HEADER ((uint64_t)sizeof(struct heap_block))
#define HEAP_NO_BLOCK (~0ull)

static struct heap_block *block_at(struct hbx_heap *heap, uint64_t offset) {
    return (struct heap_block *)(uintptr_t)(heap->base + offset);
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void hbx_heap_init(struct hbx_heap *heap, void *base, uint64_t size) {
    struct heap_block *first;
    uint64_t address, aligned, skip, usable, capacity;
    if (!heap) return;
    hbx_memset(heap, 0, sizeof *heap);
    if (!base || size <= HEAP_HEADER + HEAP_MIN_SPLIT) return;
    address = (uint64_t)(uintptr_t)base;
    aligned = align_up(address, HEAP_ALIGN);
    skip = aligned - address;
    if (size <= skip + HEAP_HEADER + HEAP_MIN_SPLIT) return;
    /* Round the remaining bytes *down* so the arena never claims memory the
     * caller did not provide. */
    usable = ((size - skip) / HEAP_ALIGN) * HEAP_ALIGN;
    if (usable <= HEAP_HEADER + HEAP_MIN_SPLIT) return;
    capacity = usable - HEAP_HEADER;
    heap->base = aligned;
    heap->size = capacity;
    heap->magic = HBX_HEAP_MAGIC;
    first = block_at(heap, 0);
    first->size = capacity + HEAP_HEADER;
    first->next_free = HEAP_NO_BLOCK;
    first->used = 0;
    first->magic = HEAP_BLOCK_MAGIC;
    heap->first_free = 0;
    heap->blocks = 1;
}

/* Insert a free block into the address-ordered free list and merge with its
 * physical neighbours.  `offset` must already describe a valid free block. */
static void free_insert(struct hbx_heap *heap, uint64_t offset) {
    uint64_t scan = heap->first_free;
    uint64_t previous = HEAP_NO_BLOCK;
    struct heap_block *block = block_at(heap, offset);
    struct heap_block *neighbour;

    block->used = 0;
    block->magic = HEAP_BLOCK_MAGIC;

    while (scan != HEAP_NO_BLOCK && scan < offset) {
        previous = scan;
        scan = block_at(heap, scan)->next_free;
    }
    block->next_free = scan;
    if (previous != HEAP_NO_BLOCK) block_at(heap, previous)->next_free = offset;
    else heap->first_free = offset;

    /* Merge with the following free block. */
    if (scan == offset + block->size) {
        neighbour = block_at(heap, scan);
        if (neighbour->magic == HEAP_BLOCK_MAGIC && !neighbour->used) {
            block->size += neighbour->size;
            block->next_free = neighbour->next_free;
            neighbour->magic = 0;
        }
    }
    /* Merge with the preceding free block. */
    if (previous != HEAP_NO_BLOCK) {
        neighbour = block_at(heap, previous);
        if (!neighbour->used && previous + neighbour->size == offset) {
            neighbour->size += block->size;
            neighbour->next_free = block->next_free;
            block->magic = 0;
        }
    }
}

void *hbx_alloc_aligned(struct hbx_heap *heap, uint64_t bytes, uint64_t alignment) {
    uint64_t offset;
    uint64_t scan;
    uint64_t previous;
    if (!heap || heap->magic != HBX_HEAP_MAGIC) return NULL;
    if (bytes == 0) bytes = 1;
    if (alignment == 0) alignment = HEAP_ALIGN;
    if (alignment > 4096u || (alignment & (alignment - 1u))) return NULL;
    /* Reject anything that cannot fit before arithmetic that could wrap. */
    if (bytes > heap->size || bytes > heap->size - HEAP_HEADER) return NULL;
    bytes = align_up(bytes, HEAP_ALIGN);
    if (bytes > heap->size - HEAP_HEADER) return NULL;

    previous = HEAP_NO_BLOCK;
    for (scan = heap->first_free; scan != HEAP_NO_BLOCK;
         scan = block_at(heap, scan)->next_free) {
        struct heap_block *block = block_at(heap, scan);
        uint64_t original;
        uint64_t payload = (heap->base + scan + HEAP_HEADER);
        uint64_t aligned = align_up(payload, alignment);
        uint64_t gap = aligned - payload;
        uint64_t body_size;
        uint64_t total;
        uint64_t remainder;

        if (block->magic != HEAP_BLOCK_MAGIC || block->used) return NULL;
        original = block->size;
        /* A gap must either be zero or large enough to hold a free block. */
        while (gap > 0 && gap < HEAP_HEADER + HEAP_MIN_SPLIT)
            gap += alignment;
        body_size = HEAP_HEADER + bytes;
        total = gap + body_size;
        if (original < total) {
            previous = scan;
            continue;
        }
        /* A remainder too small to hold a block is absorbed by the allocation
         * instead of being dropped from the free list. */
        if (original - total < HEAP_HEADER + HEAP_MIN_SPLIT) body_size = original - gap;

        /* Unlink the block; its space is re-described below. */
        if (previous != HEAP_NO_BLOCK) block_at(heap, previous)->next_free = block->next_free;
        else heap->first_free = block->next_free;

        offset = scan;
        if (gap) {
            struct heap_block *lead = block_at(heap, offset);
            lead->size = gap;
            free_insert(heap, offset);
            offset += gap;
        }
        {
            struct heap_block *body = block_at(heap, offset);
            body->size = body_size;
            body->used = 1;
            body->next_free = HEAP_NO_BLOCK;
            body->magic = HEAP_BLOCK_MAGIC;
        }
        remainder = original - (offset + body_size - scan);
        if (remainder >= HEAP_HEADER + HEAP_MIN_SPLIT) {
            struct heap_block *tail = block_at(heap, offset + body_size);
            tail->size = remainder;
            tail->magic = HEAP_BLOCK_MAGIC;
            free_insert(heap, offset + body_size);
        }

        heap->used += bytes;
        if (heap->used > heap->peak) heap->peak = heap->used;
        heap->blocks++;
        return (void *)(uintptr_t)(heap->base + offset + HEAP_HEADER);
    }
    return NULL;
}

void *hbx_alloc(struct hbx_heap *heap, uint64_t bytes) {
    return hbx_alloc_aligned(heap, bytes, HEAP_ALIGN);
}

void hbx_free(struct hbx_heap *heap, void *pointer) {
    uint64_t address;
    uint64_t offset;
    struct heap_block *block;

    if (!heap || !pointer || heap->magic != HBX_HEAP_MAGIC) return;
    address = (uint64_t)(uintptr_t)pointer;
    if (address < heap->base + HEAP_HEADER || address >= heap->base + heap->size) return;
    offset = address - heap->base - HEAP_HEADER;
    if (offset >= heap->size) return;
    block = block_at(heap, offset);
    if (block->magic != HEAP_BLOCK_MAGIC || !block->used) return;

    heap->used -= block->size > HEAP_HEADER ? block->size - HEAP_HEADER : 0;
    if ((int64_t)heap->used < 0) heap->used = 0;
    free_insert(heap, offset);
}

uint64_t hbx_heap_used(const struct hbx_heap *heap) { return heap ? heap->used : 0; }
uint64_t hbx_heap_peak(const struct hbx_heap *heap) { return heap ? heap->peak : 0; }

/* ------------------------------------------------------------------ */
/* Assets                                                             */
/* ------------------------------------------------------------------ */

/* True when a NUL appears within `limit` bytes. */
static int nul_within(const char *text, uint64_t limit) {
    uint64_t i;
    for (i = 0; i < limit; i++) if (text[i] == 0) return 1;
    return 0;
}

const void *hbx_asset(const struct hbx_ctx *ctx, const char *name, uint32_t *size) {
    const struct hbx_asset_table *table;
    uint32_t i;
    if (size) *size = 0;
    if (!ctx || !ctx->assets || !ctx->data_base || !ctx->data_size || !name) return NULL;
    table = (const struct hbx_asset_table *)(uintptr_t)ctx->assets;
    if (table->magic != HBX_ASSET_MAGIC) return NULL;
    if (table->count > HBX_MAX_ASSETS) return NULL;
    for (i = 0; i < table->count; i++) {
        const struct hbx_asset_entry *entry = &table->entries[i];
        const char *candidate;
        /* Every offset must stay inside the region the loader described. */
        if (entry->name_off >= ctx->data_size) continue;
        if (entry->data_off > ctx->data_size ||
            entry->size > ctx->data_size - entry->data_off) continue;
        candidate = (const char *)(uintptr_t)(ctx->data_base + entry->name_off);
        if (!nul_within(candidate, ctx->data_size - entry->name_off)) continue;
        if (hbx_streq(candidate, name)) {
            if (size) *size = entry->size;
            return (const void *)(uintptr_t)(ctx->data_base + entry->data_off);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Log                                                                */
/* ------------------------------------------------------------------ */

static void log_write(struct hbx_log *log, const char *text, uint32_t length) {
    uint32_t i;
    if (!log || !log->ring || !log->bytes) return;
    for (i = 0; i < length; i++) {
        char *ring = (char *)(uintptr_t)log->ring;
        if (log->head >= log->bytes) log->head = 0;
        ring[log->head++] = text[i];
    }
    {
        char *ring = (char *)(uintptr_t)log->ring;
        if (log->head < log->bytes) ring[log->head] = 0;
    }
}

void hbx_log(const struct hbx_ctx *ctx, const char *line) {
    if (!ctx || !ctx->log) return;
    log_write((struct hbx_log *)(uintptr_t)ctx->log, line, (uint32_t)hbx_strlen(line));
    log_write((struct hbx_log *)(uintptr_t)ctx->log, "\n", 1);
}

struct log_sink {
    char *data;
    uint32_t length;
    uint32_t capacity;
};

static void sink_put(struct log_sink *sink, char value) {
    if (sink->length + 1u < sink->capacity) sink->data[sink->length++] = value;
}

static void sink_text(struct log_sink *sink, const char *text) {
    while (text && *text) sink_put(sink, *text++);
}

static void sink_pad(struct log_sink *sink, char pad, int count) {
    while (count-- > 0) sink_put(sink, pad);
}

static void sink_uint(struct log_sink *sink, uint64_t value, uint32_t base,
                      int width, char pad) {
    char digits[24];
    uint32_t count = 0;
    do {
        uint64_t digit = value % base;
        digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value /= base;
    } while (value && count < sizeof digits);
    sink_pad(sink, pad, width - (int)count);
    while (count) sink_put(sink, digits[--count]);
}

static void sink_int(struct log_sink *sink, int64_t value, int width, char pad) {
    if (value < 0) {
        sink_put(sink, '-');
        sink_uint(sink, (uint64_t)(-(value + 1)) + 1u, 10u, width - 1, pad);
    } else {
        sink_uint(sink, (uint64_t)value, 10u, width, pad);
    }
}

/*
 * Minimal %s %c %d %i %u %x %X %p %% formatting, bounded by the destination.
 * Every write goes through sink_put, which reserves room for the terminator;
 * numeric conversions render into a fixed 24-byte scratch and are appended, so
 * no conversion can overrun the buffer.  Width is saturated at 32.
 */
void hbx_logf(const struct hbx_ctx *ctx, const char *format, ...) {
    char buffer[256];
    struct log_sink sink;
    __builtin_va_list args;
    const char *p;

    if (!ctx || !ctx->log || !format) return;
    sink.data = buffer;
    sink.length = 0;
    sink.capacity = sizeof buffer;

    __builtin_va_start(args, format);
    for (p = format; *p; p++) {
        int width = 0;
        char pad = ' ';
        if (*p != '%') { sink_put(&sink, *p); continue; }
        p++;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') {
            if (width < 32) width = width * 10 + (*p - '0');
            if (width > 32) width = 32;
            p++;
        }
        if (!*p) { sink_put(&sink, '%'); break; }
        switch (*p) {
        case 's':
            sink_text(&sink, __builtin_va_arg(args, const char *));
            break;
        case 'c':
            sink_put(&sink, (char)__builtin_va_arg(args, int));
            break;
        case 'd': case 'i':
            sink_int(&sink, __builtin_va_arg(args, int), width, pad);
            break;
        case 'u':
            sink_uint(&sink, __builtin_va_arg(args, unsigned), 10u, width, pad);
            break;
        case 'x':
            sink_uint(&sink, __builtin_va_arg(args, unsigned), 16u, width, pad);
            break;
        case 'X': {
            char digits[24];
            uint32_t count = 0, i;
            uint64_t value = __builtin_va_arg(args, unsigned);
            do {
                uint64_t digit = value % 16u;
                digits[count++] = (char)(digit < 10 ? '0' + digit : 'A' + (digit - 10));
                value /= 16u;
            } while (value && count < sizeof digits);
            sink_pad(&sink, pad, width - (int)count);
            for (i = count; i > 0; i--) sink_put(&sink, digits[i - 1u]);
            break;
        }
        case 'p':
            sink_put(&sink, '0');
            sink_put(&sink, 'x');
            sink_uint(&sink, (uint64_t)(uintptr_t)__builtin_va_arg(args, void *),
                      16u, 0, ' ');
            break;
        case '%':
            sink_put(&sink, '%');
            break;
        default:
            sink_put(&sink, '%');
            sink_put(&sink, *p);
            break;
        }
    }
    __builtin_va_end(args);
    buffer[sink.length] = 0;
    log_write((struct hbx_log *)(uintptr_t)ctx->log, buffer, sink.length);
    log_write((struct hbx_log *)(uintptr_t)ctx->log, "\n", 1);
}
