/* Build a ctx over a static arena and run the SDK runtime tests. */
#include "hbx_sdk.h"

void hbx_test_all(const struct hbx_ctx *ctx);
void hbx_test_install_mocks(struct hbx_ctx *ctx, uintptr_t pad, uintptr_t touch);
int hbx_test_failures(void);
int hbx_test_checks(void);

static unsigned char arena[12u * 1024u * 1024u];
static struct hbx_ctx ctx;
static struct hbx_asset_table assets;

int test_main(void) {
    uintptr_t base = (uintptr_t)arena;
    uintptr_t heap = base;
    uintptr_t data = base + 8u * 1024u * 1024u;
    uintptr_t log = data + 1024u * 1024u;
    uintptr_t pad = log + 0x10000u;
    uintptr_t touch = pad + 0x1000u;
    struct hbx_log *ring;

    hbx_memset(arena, 0, sizeof arena);
    hbx_memset(&ctx, 0, sizeof ctx);
    ctx.magic = HBX_ABI_MAGIC;
    ctx.abi_version = HBX_ABI_VERSION;
    ctx.size = sizeof ctx;
    ctx.heap_base = heap;
    ctx.heap_size = 8u * 1024u * 1024u;
    ctx.data_base = data;
    ctx.data_size = 1024u * 1024u;
    ctx.assets = (uint64_t)(uintptr_t)&assets;
    ctx.log = log;
    ring = (struct hbx_log *)(uintptr_t)log;
    ring->ring = log + 16;
    ring->bytes = 0x8000;
    hbx_test_install_mocks(&ctx, pad, touch);

    hbx_test_all(&ctx);
    return hbx_test_failures();
}

int test_checks(void) { return hbx_test_checks(); }
