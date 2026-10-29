/*
 * qemu-aarch64 has no libc in this build, so report failures with a raw
 * write(2) syscall and let the exit code carry the count.
 */
static long sys_write(int fd, const void *buffer, unsigned long length) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buffer;
    register long x2 __asm__("x2") = (long)length;
    register long x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

void hbx_test_report(int line, const char *expression) {
    static const char hex[] = "0123456789abcdef";
    char buffer[24] = "FAIL 0x";
    int i;
    (void)expression;
    for (i = 0; i < 4; i++) buffer[7 + i] = hex[(line >> ((3 - i) * 4)) & 0xf];
    buffer[11] = '\n';
    sys_write(1, buffer, 12);
}
