/*
 * start.S - image entry for Linux-personality apps.
 *
 * The host calls _start(ctx) like a function on SmileBASIC's thread.  We save
 * everything the caller expects preserved (x19-x30, d8-d15, SP, TPIDR_EL0,
 * which musl takes over for its thread pointer), switch to the host-provided
 * stack and run hbxl_entry.  hbxl_return_to_host() restores that frame from
 * whatever stack the program is on and returns the exit code to the host.
 */
    .section .text.entry, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    bti     jc
    adrp    x9, hbxl_host_frame
    add     x9, x9, :lo12:hbxl_host_frame
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    stp     d8, d9,   [x9, #96]
    stp     d10, d11, [x9, #112]
    stp     d12, d13, [x9, #128]
    stp     d14, d15, [x9, #144]
    mov     x10, sp
    mrs     x11, tpidr_el0
    stp     x10, x11, [x9, #160]
    mrs     x11, tpidrro_el0
    str     x11, [x9, #176]

    /* Host stack: ctx->stack_base (0x70) + ctx->stack_size (0x78). */
    ldr     x1, [x0, #0x70]
    ldr     x2, [x0, #0x78]
    cbz     x1, 1f
    cbz     x2, 1f
    add     x2, x1, x2
    and     x2, x2, #~15
    mov     sp, x2
1:
    mov     x29, #0
    mov     x30, #0
    bl      hbxl_entry
    /* hbxl_entry never returns; fall into the restore path if it does. */
    mov     x0, #9
    b       hbxl_restore_host
    .size _start, .-_start

/* _spawn(params, ctx): threaded launch, same contract as sdk/src/crt0.S. */
    .global _spawn
    .type _spawn, %function
_spawn:
    bti     jc
    b       hbx_spawn
    .size _spawn, .-_spawn

    .global hbxl_restore_host
    .type hbxl_restore_host, %function
hbxl_restore_host:
    bti     c
    adrp    x9, hbxl_host_frame
    add     x9, x9, :lo12:hbxl_host_frame
    ldp     x10, x11, [x9, #160]
    mov     sp, x10
    msr     tpidr_el0, x11
    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x30, [x9, #80]
    ldp     d8, d9,   [x9, #96]
    ldp     d10, d11, [x9, #112]
    ldp     d12, d13, [x9, #128]
    ldp     d14, d15, [x9, #144]
    ret
    .size hbxl_restore_host, .-hbxl_restore_host

/* hbxl_call_on_stack(fn, arg, stack_top): run fn(arg) on another stack. */
    .global hbxl_call_on_stack
    .type hbxl_call_on_stack, %function
hbxl_call_on_stack:
    bti     c
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x9, x0
    mov     x0, x1
    and     x2, x2, #~15
    mov     x10, sp
    mov     sp, x2
    str     x10, [sp, #-16]!
    blr     x9
    ldr     x10, [sp], #16
    mov     sp, x10
    ldp     x29, x30, [sp], #16
    ret
    .size hbxl_call_on_stack, .-hbxl_call_on_stack


/* hbxl_frame_save(frame) -> 0; hbxl_frame_jump(frame) makes it return 1.
 * Frame: x19-x30, d8-d15, sp (21 words). */
    .global hbxl_frame_save
    .type hbxl_frame_save, %function
hbxl_frame_save:
    bti     c
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    stp     d8, d9,   [x0, #96]
    stp     d10, d11, [x0, #112]
    stp     d12, d13, [x0, #128]
    stp     d14, d15, [x0, #144]
    mov     x9, sp
    str     x9, [x0, #160]
    mov     x0, #0
    ret
    .size hbxl_frame_save, .-hbxl_frame_save

    .global hbxl_frame_jump
    .type hbxl_frame_jump, %function
hbxl_frame_jump:
    bti     c
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldp     d8, d9,   [x0, #96]
    ldp     d10, d11, [x0, #112]
    ldp     d12, d13, [x0, #128]
    ldp     d14, d15, [x0, #144]
    ldr     x9, [x0, #160]
    mov     sp, x9
    mov     x0, #1
    ret
    .size hbxl_frame_jump, .-hbxl_frame_jump

    .global hbxl_set_tp
    .type hbxl_set_tp, %function
hbxl_set_tp:
    bti     c
    msr     tpidr_el0, x0
    ret
    .size hbxl_set_tp, .-hbxl_set_tp

/* Horizon SVCs.  Each is a leaf; the kernel preserves x19-x30. */
    .global hbxl_svc_tick
    .type hbxl_svc_tick, %function
hbxl_svc_tick:
    bti     c
    svc     #0x1e
    ret
    .size hbxl_svc_tick, .-hbxl_svc_tick

    .global hbxl_svc_sleep
    .type hbxl_svc_sleep, %function
hbxl_svc_sleep:
    bti     c
    svc     #0x0b
    ret
    .size hbxl_svc_sleep, .-hbxl_svc_sleep

/* WaitForAddress(addr, type, value, timeout_ns) -> Result in w0. */
    .global hbxl_svc_wait_for_address
    .type hbxl_svc_wait_for_address, %function
hbxl_svc_wait_for_address:
    bti     c
    svc     #0x34
    ret
    .size hbxl_svc_wait_for_address, .-hbxl_svc_wait_for_address

/* SignalToAddress(addr, type, value, count) -> Result in w0. */
    .global hbxl_svc_signal_to_address
    .type hbxl_svc_signal_to_address, %function
hbxl_svc_signal_to_address:
    bti     c
    svc     #0x35
    ret
    .size hbxl_svc_signal_to_address, .-hbxl_svc_signal_to_address

/* musl's __set_thread_area is C-callable; it only writes TPIDR_EL0.  We keep
 * musl's own implementation. */

    .bss
    .align 4
    .global hbxl_host_frame
hbxl_host_frame:
    .space 192

    .section .note.GNU-stack, "", %progbits
