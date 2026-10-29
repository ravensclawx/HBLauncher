/* qemu-aarch64 user-mode entry: run the tests, exit with the failure count. */
    .section .text.entry, "ax", %progbits
    .global _start
_start:
    bti c
    bl test_main
    mov x0, x0
    mov x8, #93            /* exit_group */
    svc #0
1:  b 1b
