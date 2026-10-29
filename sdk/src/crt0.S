/*
 * crt0.S - freestanding entry for every HBLauncher Native image.
 *
 * x0 = struct hbx_ctx* at entry.  The host maps the data region zero-filled and
 * never remaps it while the image stays loaded, so .bss is zero on the first
 * entry and preserved across re-entries (the launcher relies on that to keep
 * its renderer and UI state alive between app launches).
 *
 * The host calls this like a normal function, so the AAPCS64 obligations are
 * honoured: the caller's frame pointer and link register are saved on the
 * *caller's* stack, callee-saved registers we do not touch stay intact, and
 * both SP and x29/x30 are restored before returning.
 */
    .section .text.entry, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    /* The host may enter with BR or BLR, so the landing pad must accept both
     * (the proven console app uses `bti jc` for the same reason).  `bti c`
     * crashes on a BR landing when the code pages are BTI-guarded. */
    bti     jc
    stp     x29, x30, [sp, #-16]!   /* save the caller's frame */
    mov     x29, sp
    mov     x9, x0                  /* ctx */

    /* Switch to the host-provided stack when it looks usable; otherwise stay
     * on the caller's stack rather than moving SP to a bad address. */
    ldr     x1, [x9, #0x70]         /* ctx->stack_base */
    ldr     x2, [x9, #0x78]         /* ctx->stack_size */
    cbz     x1, 1f
    cbz     x2, 1f
    tst     x1, #15
    b.ne    1f
    cmp     x2, #4096
    b.lo    1f
    add     x2, x1, x2
    mov     sp, x2
1:

    bl      hbx_app_main

    /* Restore the caller's SP, frame pointer and return address. */
    mov     sp, x29
    ldp     x29, x30, [sp], #16
    ret
    .size _start, .-_start

/* _spawn(params, ctx): threaded launch (see hbx_spawn in hbx_app.c).  The
 * host may enter with BR or BLR, hence `bti jc`. */
    .weak hbx_spawn                 /* absent in images without hbx_app.c */
    .global _spawn
    .type _spawn, %function
_spawn:
    bti     jc
    b       hbx_spawn
    .size _spawn, .-_spawn
