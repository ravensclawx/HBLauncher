/* qemu entry for the _start contract test: run test_main, exit with its code. */
    .section .text.boot, "ax", %progbits
    .global _boot
_boot:
    bti c
    bl test_main
    mov x8, #93
    svc #0
1:  b 1b
