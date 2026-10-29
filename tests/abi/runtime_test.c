/*
 * runtime_test.c - offline tests for the SDK runtime contract.
 *
 * Built twice: freestanding AArch64 (run under qemu-aarch64) and as a host
 * x86-64 program.  The same source covers heap, input/touch mapping, nav,
 * touch hit testing, assets, log formatting and ABI validation.
 */
#include "hbx_app.h"
#include "hbx_sdk.h"

static int failures;
static int checks;

void hbx_test_report(int line, const char *expression);

#define EXPECT(condition) do { \
    checks++; \
    if (!(condition)) { failures++; hbx_test_report(__LINE__, #condition); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Mocks                                                              */
/* ------------------------------------------------------------------ */

static uint32_t mock_buttons;
static int32_t mock_lx, mock_ly;
static uint32_t mock_connected;
static int mock_calls;

static void mock_pad(struct hbx_pad_state *state, const uint32_t *id) {
    mock_calls++;
    hbx_memset(state, 0, sizeof *state);
    state->buttons = mock_buttons;
    state->stick_lx = mock_lx;
    state->stick_ly = mock_ly;
    state->connected = mock_connected;
    state->reserved0 = *id;
}

static uint32_t mock_touch_count;
static uint32_t mock_touch_x, mock_touch_y;

static void put32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void mock_touch(struct hbx_touch_state *state) {
    unsigned char *bytes = state->raw;
    unsigned char *entry = bytes + 0x18;
    hbx_memset(state, 0, sizeof *state);
    put32(bytes + 0x10, mock_touch_count);   /* nn::hid layout: count at 0x10 */
    if (mock_touch_count) {
        put32(entry + 4, 7);
        put32(entry + 8, mock_touch_x);
        put32(entry + 12, mock_touch_y);
    }
}

void hbx_test_install_mocks(struct hbx_ctx *ctx, uintptr_t pad, uintptr_t touch) {
    ctx->input.pad_handheld = (uint64_t)(uintptr_t)&mock_pad;
    ctx->input.touch = (uint64_t)(uintptr_t)&mock_touch;
    ctx->input.pad_state = pad;
    ctx->input.touch_state = touch;
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_context(const struct hbx_ctx *ctx) {
    struct hbx_ctx bad;
    hbx_memset(&bad, 0, sizeof bad);
    EXPECT(hbx_ctx_check(ctx) == HBX_OK);
    EXPECT(hbx_ctx_check(NULL) == HBX_ERR_ARG);
    EXPECT(hbx_ctx_check(&bad) == HBX_ERR_ABI);
    bad.magic = HBX_ABI_MAGIC;
    bad.abi_version = 99;
    bad.size = sizeof bad;
    EXPECT(hbx_ctx_check(&bad) == HBX_ERR_ABI);
    bad.abi_version = HBX_ABI_VERSION;
    bad.size = 1;
    EXPECT(hbx_ctx_check(&bad) == HBX_ERR_ABI);
}

static void test_heap(struct hbx_ctx *ctx) {
    struct hbx_heap heap;
    unsigned char *a, *b, *c;
    int i;

    hbx_heap_init(&heap, (void *)(uintptr_t)ctx->heap_base, ctx->heap_size);
    EXPECT(heap.magic == HBX_HEAP_MAGIC);
    EXPECT(hbx_heap_used(&heap) == 0);

    a = (unsigned char *)hbx_alloc(&heap, 100);
    EXPECT(a != NULL);
    EXPECT(((uintptr_t)a & 15u) == 0);
    for (i = 0; i < 100; i++) a[i] = (unsigned char)i;
    b = (unsigned char *)hbx_alloc(&heap, 4096);
    EXPECT(b != NULL);
    EXPECT((uintptr_t)b >= (uintptr_t)a + 100);
    c = (unsigned char *)hbx_alloc_aligned(&heap, 33, 4096);
    EXPECT(c != NULL);
    EXPECT(((uintptr_t)c & 4095u) == 0);
    EXPECT(hbx_heap_peak(&heap) >= 100 + 4096 + 33);

    hbx_free(&heap, b);
    hbx_free(&heap, a);
    hbx_free(&heap, c);
    EXPECT(hbx_heap_used(&heap) == 0);

    /* Reallocation after coalescing can satisfy a large request again. */
    a = (unsigned char *)hbx_alloc(&heap, 8192);
    EXPECT(a != NULL);
    hbx_free(&heap, a);

    /* Exhaustion returns NULL instead of corrupting state. */
    EXPECT(hbx_alloc(&heap, (uint64_t)1 << 40) == NULL);

    /* Double free and foreign pointers are ignored. */
    a = (unsigned char *)hbx_alloc(&heap, 64);
    hbx_free(&heap, a);
    hbx_free(&heap, a);
    hbx_free(&heap, (void *)(uintptr_t)0x10);
    EXPECT(hbx_heap_used(&heap) == 0);
    EXPECT(hbx_alloc(&heap, 0) != NULL);
}

static void test_input(struct hbx_ctx *ctx) {
    struct hbx_input_state state;
    int dx = 0, dy = 0, confirm = 0, cancel = 0;

    hbx_memset(&state, 0, sizeof state);
    mock_buttons = HBX_A | HBX_UP;
    mock_lx = 0;
    mock_ly = 0;
    mock_connected = 1;
    hbx_input_poll(ctx, &state);
    EXPECT(state.buttons == (HBX_A | HBX_UP));
    EXPECT(state.pressed == (HBX_A | HBX_UP));
    EXPECT(state.released == 0);
    EXPECT(state.connected == 1);
    EXPECT(hbx_input_nav(&state, &dx, &dy, &confirm, &cancel));
    EXPECT(dy == -1 && dx == 0 && confirm && !cancel);

    mock_buttons = HBX_UP;
    hbx_input_poll(ctx, &state);
    EXPECT(state.pressed == 0);
    EXPECT(state.released == HBX_A);
    EXPECT(hbx_input_nav(&state, &dx, &dy, &confirm, &cancel) == 0);

    /* Radial deadzone: small deflections are zero. */
    mock_buttons = 0;
    mock_lx = 1000;
    mock_ly = 1000;
    hbx_input_poll(ctx, &state);
    EXPECT(state.stick_lx == 0 && state.stick_ly == 0);

    /* Outside the deadzone the axis is scaled to -100..100. */
    mock_lx = 32767;
    mock_ly = 0;
    hbx_input_poll(ctx, &state);
    EXPECT(state.stick_lx == 100);

    /* Disconnected sources contribute nothing. */
    mock_connected = 0;
    mock_buttons = HBX_B;
    hbx_input_poll(ctx, &state);
    EXPECT(state.buttons == 0);
    EXPECT(state.connected == 0);

    /* Touch is copied and scaled 3:2 for hit testing. */
    mock_touch_count = 1;
    mock_touch_x = 100;   /* 150 in 1920 space */
    mock_touch_y = 100;   /* 150 in 1080 space */
    hbx_input_poll(ctx, &state);
    EXPECT(state.touch_count == 1);
    EXPECT(state.touch[0].id == 7);
    EXPECT(hbx_touch_hit(&state.touch[0], 140, 140, 20, 20));
    EXPECT(!hbx_touch_hit(&state.touch[0], 0, 0, 20, 20));
    mock_touch_count = 0;
    hbx_input_poll(ctx, &state);
    EXPECT(state.touch_count == 0);
}

static void test_time(const struct hbx_ctx *ctx) {
    uint64_t first = hbx_now_ns(ctx);
    uint64_t second;
    hbx_sleep_ns(1000000);  /* 1 ms */
    second = hbx_now_ns(ctx);
    EXPECT(second >= first);
#ifndef HBX_TEST_NO_SLEEP
    if (second > first) EXPECT(second - first >= 500000);
#endif
}

static void test_assets(struct hbx_ctx *ctx) {
    uint32_t size = 0;
    const void *data;
    struct hbx_asset_table *table;
    char *region = (char *)(uintptr_t)ctx->data_base;

    table = (struct hbx_asset_table *)(uintptr_t)ctx->assets;
    table->magic = HBX_ASSET_MAGIC;
    table->count = 1;
    table->entries[0].name_off = 512;
    table->entries[0].data_off = 1024;
    table->entries[0].size = 16;
    hbx_memcpy(region + 512, "levels/one.bin", 15);
    hbx_memset(region + 1024, 0xab, 16);

    data = hbx_asset(ctx, "levels/one.bin", &size);
    EXPECT(data != NULL && size == 16);
    EXPECT(data && *(const unsigned char *)data == 0xab);
    EXPECT(hbx_asset(ctx, "missing", &size) == NULL);
    EXPECT(hbx_asset(ctx, NULL, &size) == NULL);
}

static void test_log(const struct hbx_ctx *ctx) {
    struct hbx_log *log = (struct hbx_log *)(uintptr_t)ctx->log;
    hbx_log(ctx, "hello");
    hbx_logf(ctx, "n=%d hex=%x neg=%d str=%s pct=%%", 42, 0xbeef, -7, "ok");
    {
        char *ring = (char *)(uintptr_t)log->ring;
        EXPECT(log->head > 0);
        EXPECT(ring[0] == 'h');
    }
    (void)log;
}

int hbx_test_failures(void) { return failures; }
int hbx_test_checks(void) { return checks; }

void hbx_test_all(const struct hbx_ctx *ctx) {
    struct hbx_ctx *mutable_ctx = (struct hbx_ctx *)(uintptr_t)ctx;
    unsigned char *memory = (unsigned char *)(uintptr_t)ctx->heap_base;
    test_context(ctx);
    test_heap(mutable_ctx);
    test_input(mutable_ctx);
    test_time(ctx);
    test_assets(mutable_ctx);
    test_log(ctx);
    (void)memory;
}
