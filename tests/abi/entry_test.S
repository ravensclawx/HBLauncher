/*
 * Call _start with a prepared frame and verify the AAPCS64 contract:
 *   - the app's status comes back in w0,
 *   - the caller's SP is restored exactly,
 *   - callee-saved registers the trampoline does not own are preserved.
 *
 * This function is itself a well-behaved callee: it saves x19/x20 and its
 * frame so test_main's own registers survive.
 *
 * long test_call_entry(const struct hbx_ctx *ctx)
 *   returns the app status, or 0xd1/0xd2/0xd3 when the contract was violated.
 */
    .section .text, "ax", %progbits
    .global test_call_entry
    .type test_call_entry, %function
test_call_entry:
    bti c
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]

    str     x0, [x29, #32]          /* ctx across the call */
    mov     x10, sp
    str     x10, [x29, #40]         /* expected SP after return */
    movz    x19, #0x1234            /* callee-saved canaries */
    movz    x20, #0x5678

    ldr     x0, [x29, #32]
    bl      _start

    mov     x11, x0                 /* app status */
    ldr     x10, [x29, #40]
    cmp     sp, x10                 /* SP must be exactly restored */
    b.ne    1f
    movz    x13, #0x1234
    cmp     x19, x13
    b.ne    2f
    movz    x13, #0x5678
    cmp     x20, x13
    b.ne    3f
    mov     x0, x11
    b       9f
1:  movz    x0, #0xd1
    b       9f
2:  movz    x0, #0xd2
    b       9f
3:  movz    x0, #0xd3
9:
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #48
    ret
    .size test_call_entry, .-test_call_entry
