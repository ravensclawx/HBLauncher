/*
 * Native probe - staged hardware smoke test.
 *
 * ctx->args[1] selects how far to go; ctx->args[2] reports the last step that
 * completed.  Start with step 0 (entry + ABI on the caller's stack) and raise
 * the step until something fails: the crash point then names the feature.
 *
 *   0  entry, ctx validation, write a marker back into the context
 *   1  same, but on the host-provided stack (the host sets stack_base for 1+)
 *   2  + hbx_tick (CNTVCT read)
 *   3  + logging
 *   4  + heap init / alloc / free
 *   5  + formatted logging (the full path)
 *   6  + persist block write/readback (the loader-placed RW region)
 */
#include "hbx_sdk.h"
#include "hbx_app.h"

#define PROBE_MARKER 0x5A5A1234u

int hbx_app_main(const struct hbx_ctx *ctx) {
    struct hbx_ctx *mutable_ctx = (struct hbx_ctx *)(uintptr_t)ctx;
    uint32_t step = (uint32_t)ctx->args[1];
    uint32_t reached = 0;

    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    reached = 1;

    if (step >= 2) { hbx_tick(); reached = 2; }
    if (step >= 3) { hbx_log(ctx, "probe: log"); reached = 3; }
    if (step >= 4) {
        struct hbx_heap heap;
        void *block;
        hbx_heap_init(&heap, (void *)(uintptr_t)ctx->heap_base, ctx->heap_size);
        block = hbx_alloc(&heap, 128);
        if (!block) return HBX_ERR_HEAP;
        hbx_memset(block, 0x5a, 128);
        hbx_free(&heap, block);
        reached = 4;
    }
    if (step >= 5) {
        hbx_logf(ctx, "probe: ctx=%p heap=%p size=%u", (void *)(uintptr_t)ctx,
                 (void *)(uintptr_t)ctx->heap_base, (unsigned)ctx->size);
        reached = 5;
    }
    if (step >= 6) {
        /* The persist block is the image's writable state: an SDK-allocated
           Normal/RW block at ctx->data_base.  Write a pattern well past the
           SDK header, read it back and verify - this is the hardware gate
           for the RW placement (mapped pages are RX and would fault). */
        struct hbx_persist *persist = hbx_persist_of(ctx);
        volatile uint64_t *scratch;
        uint64_t i;
        if (!persist || ctx->data_size < 1024u) return HBX_ERR_ABI;
        scratch = (volatile uint64_t *)(void *)((unsigned char *)persist + 512);
        for (i = 0; i < 64u; i++) scratch[i] = 0x5A00000000000000ull | i;
        for (i = 0; i < 64u; i++)
            if (scratch[i] != (0x5A00000000000000ull | i)) return HBX_ERR_INTERNAL;
        hbx_logf(ctx, "probe: persist rw ok (%u bytes)", (unsigned)ctx->data_size);
        reached = 6;
    }

    mutable_ctx->frame = PROBE_MARKER;
    mutable_ctx->args[2] = reached;
    return 0x5A;
}
