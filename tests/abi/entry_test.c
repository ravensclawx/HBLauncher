/*
 * entry_test.c - verify the _start trampoline contract on AArch64.
 *
 * _start must: run the app on the host-provided stack, return the app status
 * in w0, and restore the caller's SP, x29, x30 and the callee-saved registers
 * it does not own.
 */
#include "hbx_sdk.h"

extern long test_call_entry(const struct hbx_ctx *ctx);

static unsigned char app_stack[64 * 1024] __attribute__((aligned(4096)));
static struct hbx_ctx ctx;
static uint64_t observed_sp;
static uint64_t observed_magic;

int hbx_app_main(const struct hbx_ctx *c) {
    __asm__ volatile("mov %0, sp" : "=r"(observed_sp));
    observed_magic = c->magic;
    if (c->stack_base != (uint64_t)(uintptr_t)app_stack) return 91;
    if (c->stack_size != sizeof app_stack) return 92;
    return 42;
}

int test_checks(void) { return 4; }

int test_main(void) {
    long result;
    hbx_memset(&ctx, 0, sizeof ctx);
    ctx.magic = HBX_ABI_MAGIC;
    ctx.abi_version = HBX_ABI_VERSION;
    ctx.size = sizeof ctx;
    ctx.stack_base = (uint64_t)(uintptr_t)app_stack;
    ctx.stack_size = sizeof app_stack;

    result = test_call_entry(&ctx);
    if (result != 42) return (int)(result & 0xff);                       /* status not returned */
    if (observed_magic != HBX_ABI_MAGIC) return 2;    /* x0 not delivered */
    if (observed_sp != (uint64_t)(uintptr_t)(app_stack + sizeof app_stack))
        return 3;                                     /* wrong stack */
    return 0;
}
