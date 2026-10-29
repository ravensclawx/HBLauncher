/*
 * runtime.c - entry, startup and the syscall dispatcher of the HBLauncher
 * Linux personality.  See hbxl.h.
 */
#include <stdarg.h>
#include "hbxl.h"
#include "hbx_sdk.h"

const struct hbx_ctx *hbxl_ctx;
/* crtbegin.o is not linked (-nostartfiles); C++ static destructors need it. */
void *__dso_handle = &__dso_handle;
uint64_t hbxl_imp[IMP_COUNT];
volatile int hbxl_exiting;

/* Module offsets in the SmileBASIC 4.4.9 / FW 23.0.1 SDK (sdk/sdk-symbols.json). */
static const uint64_t import_offsets[IMP_COUNT] = {
    0x577710ull, /* posix_memalign */
    0x55b360ull, /* operator delete -> free */
    0x129760ull, /* nnosCreateThreadWithCoreNumber */
    0x129790ull, /* nnosStartThread */
    0x1297b0ull, /* nnosWaitThread */
    0x129780ull, /* nnosDestroyThread */
    0x1297a0ull, /* nnosGetCurrentThread */
    0x1297f0ull, /* nnosGetThreadPriority */
    0x129850ull, /* nnosGetCurrentCoreNumber */
    0x1295e0ull, /* nn::os::GetThreadAvailableCoreMask */
    0x1415c0ull, /* nnsocketSocket */
    0x141540ull, /* nnsocketConnect */
    0x1414e0ull, /* nnsocketRecv */
    0x141500ull, /* nnsocketSend */
    0x1415f0ull, /* nnsocketClose */
    0x141760ull, /* nnsocketGetLastErrno */
    0x141590ull, /* nnsocketSetSockOpt */
    0x12c770ull, /* nn::os::SetUserExceptionHandler */
    0x12c7f0ull, /* nn::os::DefaultUserExceptionHandler */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

void hbxl_memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
}

void hbxl_memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
}

size_t hbxl_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Log ring of the host (ctx->log) plus the TCP console when connected. */
static void ring_append(const char *text, size_t n) {
    struct hbx_log *log;
    char *ring;
    size_t i;
    if (!hbxl_ctx || !hbxl_ctx->log) return;
    log = (struct hbx_log *)(uintptr_t)hbxl_ctx->log;
    ring = (char *)(uintptr_t)log->ring;
    if (!ring || !log->bytes) return;
    for (i = 0; i < n; i++) {                     /* same discipline as hbx_log */
        if (log->head >= log->bytes) log->head = 0;
        ring[log->head++] = text[i];
    }
    if (log->head < log->bytes) ring[log->head] = 0;
}

void hbxl_log(const char *text) {
    size_t n = hbxl_strlen(text);
    ring_append(text, n);
    ring_append("\n", 1);
    hbxl_vfs_write_console(2, text, n);
    hbxl_vfs_write_console(2, "\n", 1);
}

static void format_log(char *buf, size_t cap, const char *format, va_list ap);

void hbxl_logf(const char *format, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, format);
    format_log(buf, sizeof buf, format, ap);
    va_end(ap);
    hbxl_log(buf);
}

/* Log ring only (no socket): for the file-server transport itself. */
void hbxl_ring_logf(const char *format, ...) {
    char buf[256];
    size_t n;
    va_list ap;
    va_start(ap, format);
    format_log(buf, sizeof buf, format, ap);
    va_end(ap);
    n = hbxl_strlen(buf);
    ring_append(buf, n);
    ring_append("\n", 1);
}

static void format_log(char *buf, size_t cap, const char *format, va_list ap) {
    size_t o = 0;
    while (*format && o < cap - 24) {
        if (*format != '%') { buf[o++] = *format++; continue; }
        format++;
        if (*format == 'l') format++;
        if (*format == 's') {
            const char *s = va_arg(ap, const char *);
            while (s && *s && o < cap - 2) buf[o++] = *s++;
        } else if (*format == 'd' || *format == 'u' || *format == 'x') {
            char t[24];
            int k = 0, neg = 0;
            unsigned base = *format == 'x' ? 16u : 10u;
            uint64_t v;
            if (*format == 'd') { int64_t s = va_arg(ap, int64_t); neg = s < 0; v = neg ? (uint64_t)-s : (uint64_t)s; }
            else v = va_arg(ap, uint64_t);
            do { t[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            if (neg) buf[o++] = '-';
            while (k) buf[o++] = t[--k];
        }
        if (*format) format++;
    }
    buf[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

static uint64_t g_tick_hz = 19200000ull;

static uint64_t monotonic_ns(void) {
    uint64_t t = hbxl_svc_tick();
    return (t / g_tick_hz) * 1000000000ull + (t % g_tick_hz) * 1000000000ull / g_tick_hz;
}

struct kts { int64_t sec, nsec; };

static long sys_clock_gettime(long clock, struct kts *ts) {
    uint64_t ns = monotonic_ns();
    if (!ts) return -HBXL_EINVAL;
    if (clock == 0 || clock == 8 || clock == 5)       /* REALTIME, REALTIME_ALARM, REALTIME_COARSE */
        ns += (uint64_t)hbxl_vfs_time_offset_ns();
    ts->sec = (int64_t)(ns / 1000000000ull);
    ts->nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

static long sys_nanosleep(const struct kts *req) {
    int64_t ns;
    if (!req) return -HBXL_EINVAL;
    ns = req->sec * 1000000000ll + req->nsec;
    hbxl_svc_sleep(ns > 0 ? ns : 0);
    return 0;
}

static long sys_clock_nanosleep(long clock, long flags, const struct kts *req) {
    int64_t ns;
    if (!req) return -HBXL_EINVAL;
    ns = req->sec * 1000000000ll + req->nsec;
    if (flags & 1) {                                   /* TIMER_ABSTIME */
        struct kts now;
        sys_clock_gettime(clock, &now);
        ns -= now.sec * 1000000000ll + now.nsec;
    }
    hbxl_svc_sleep(ns > 0 ? ns : 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Misc syscalls                                                      */
/* ------------------------------------------------------------------ */

static uint64_t g_random_state = 0x9e3779b97f4a7c15ull;
static long sys_getrandom(uint8_t *buf, size_t n) {
    size_t i;
    g_random_state ^= hbxl_svc_tick();
    for (i = 0; i < n; i++) {
        g_random_state ^= g_random_state << 13;
        g_random_state ^= g_random_state >> 7;
        g_random_state ^= g_random_state << 17;
        buf[i] = (uint8_t)g_random_state;
    }
    return (long)n;
}

struct utsname_k { char f[6][65]; };
static long sys_uname(struct utsname_k *u) {
    static const char *const v[6] = {"Linux", "switch2", "6.1.0-hbxl", "#1 HBLauncher", "aarch64", "(none)"};
    int i;
    for (i = 0; i < 6; i++) {
        size_t n = hbxl_strlen(v[i]);
        hbxl_memset(u->f[i], 0, 65);
        hbxl_memcpy(u->f[i], v[i], n);
    }
    return 0;
}

static long sys_sched_getaffinity(long pid, size_t size, uint8_t *mask) {
    uint64_t m = HBXL_CALL(IMP_OS_COREMASK, uint64_t (*)(void))();
    size_t i;
    (void)pid;
    if (size < 8) return -HBXL_EINVAL;
    for (i = 0; i < size; i++) mask[i] = i < 8 ? (uint8_t)(m >> (i * 8)) : 0;
    return 8;
}

struct rlimit_k { uint64_t cur, max; };

/* ------------------------------------------------------------------ */
/* Dispatcher                                                         */
/* ------------------------------------------------------------------ */

/* Backtrace of the raising thread when the app kills itself (abort(),
 * std::terminate, failed asserts).  The exception handler never runs in this
 * process, so this is the only stack we get; symbolise the CODE+ offsets with
 * addr2line -e <elf> against the code base. */
struct _Unwind_Context;
typedef int (*hbxl_unwind_fn)(struct _Unwind_Context *, void *);
extern int _Unwind_Backtrace(hbxl_unwind_fn, void *) __attribute__((weak));
extern uint64_t _Unwind_GetIP(struct _Unwind_Context *) __attribute__((weak));
extern char __hbxl_text_start[], __hbxl_text_end[];

static int backtrace_frame(struct _Unwind_Context *ctx, void *arg) {
    int *depth = (int *)arg;
    uint64_t ip = _Unwind_GetIP(ctx);
    uint64_t text = (uint64_t)(uintptr_t)__hbxl_text_start, end = (uint64_t)(uintptr_t)__hbxl_text_end;
    if (ip >= text && ip < end)
        hbxl_logf("hbxl:   #%d 0x%x", (int64_t)*depth, ip);
    else
        hbxl_logf("hbxl:   #%d 0x%x (outside the image)", (int64_t)*depth, ip);
    return ++*depth >= 40 ? 5 : 0;                      /* _URC_END_OF_STACK stops it */
}

/* musl's raise/abort carry no unwind tables, so the unwinder stops there.
 * Also scan the raw stack: a word that points into the code image just after
 * a BL/BLR instruction is (almost certainly) a return address.  Logged as it
 * goes, so running off the end of the stack still leaves the earlier ones. */
static void hbxl_stack_scan(void) {
    uint64_t text = (uint64_t)(uintptr_t)__hbxl_text_start, end = (uint64_t)(uintptr_t)__hbxl_text_end;
    const uint64_t *sp = (const uint64_t *)__builtin_frame_address(0);
    int i, found = 0;
    hbxl_log("hbxl: stack scan (return addresses, innermost first):");
    for (i = 0; i < 16384 && found < 48; i++) {
        uint64_t w = sp[i];
        uint32_t insn;
        if (w < text + 4 || w >= end || (w & 3)) continue;
        insn = *(const uint32_t *)(uintptr_t)(w - 4);
        if ((insn & 0xfc000000u) == 0x94000000u || (insn & 0xfffffc1fu) == 0xd63f0000u) {
            hbxl_logf("hbxl:   ra 0x%x (sp+0x%x)", w, (uint64_t)i * 8);
            found++;
        }
    }
    hbxl_log("hbxl: end of stack scan");
}

static void log_backtrace(void) {
    int depth = 0;
    if (!_Unwind_Backtrace || !_Unwind_GetIP) return;
    hbxl_logf("hbxl: backtrace of tid %d:", hbxl_gettid());
    _Unwind_Backtrace(backtrace_frame, &depth);
    hbxl_stack_scan();
}

/* The host's stop request (control flags bit 0, as for native apps): the app
 * exits through the same drain as exit_group, back to the launcher. */
int hbxl_stop_requested(void) {
    const struct hbx_control *c;
    if (!hbxl_ctx || !hbxl_ctx->control) return 0;
    c = (const struct hbx_control *)(uintptr_t)hbxl_ctx->control;
    if (!(__atomic_load_n(&c->flags, __ATOMIC_ACQUIRE) & 1u)) return 0;
    if (!hbxl_exiting) {
        hbxl_log("hbxl: stop requested by the host");
        hbxl_exiting = 0x100;
    }
    return 1;
}

static uint8_t g_warned[512];
static int g_trace;                                     /* HBXL_TRACE=1 in /hbxl-env */
static int g_crashtest;                                 /* HBXL_CRASHTEST=1: fault on purpose */

static int env_is(const char *line, const char *key) {
    unsigned k = 0;
    while (key[k] && line[k] == key[k]) k++;
    return !key[k];
}

long __hbx_syscall(long n, long a, long b, long c, long d, long e, long f) {
    int handled = 0;
    long r;
    /* Log every syscall except the per-frame noise (futex, clocks, yield,
     * sleeps, read/write) so the last line before a crash says what ran. */
    if (g_trace && n != 98 && n != 113 && n != 124 && n != 101 && n != 115 &&
        n != 63 && n != 64 && n != 67 && n != 169)
        hbxl_logf("hbxl: sc %d %x %x %x tid %d", n, a, b, c, hbxl_gettid());
    if (!hbxl_exiting) hbxl_stop_requested();
    if (hbxl_exiting && n != 93 && n != 94) {
        if (hbxl_gettid() != 1) hbxl_thread_exit(0);   /* draining for exit */
        hbxl_return_to_host(hbxl_exiting & 0xff);
    }
    r = hbxl_vfs_syscall(n, a, b, c, d, e, f, &handled);
    if (handled) return r;
    switch (n) {
    case 222: {                                        /* mmap */
        void *p;
        if (a && (d & 0x10)) return -HBXL_ENOMEM;      /* MAP_FIXED unsupported */
        p = hbxl_mmap((size_t)b);
        if (!p) return -HBXL_ENOMEM;
        if (!(d & 0x20) && e >= 0) {                   /* file-backed: read it in */
            long got = hbxl_vfs_syscall(67, e, (long)p, b, f, 0, 0, &handled);
            (void)got;
        }
        return (long)p;
    }
    case 215: return hbxl_munmap((void *)a, (size_t)b);                 /* munmap */
    case 226: return 0;                                                 /* mprotect */
    case 233:                                                           /* madvise */
        if (c == 4) hbxl_memset((void *)a, 0, (size_t)b);              /* DONTNEED zero-fills */
        return 0;
    case 216: return -HBXL_ENOMEM;                                      /* mremap */
    case 214: return 0;                                                 /* brk: none */
    case 98:  return hbxl_futex((volatile int32_t *)a, (int)b, (int32_t)c, (const void *)d,
                                (volatile int32_t *)e, (int32_t)f);
    case 178: return hbxl_gettid();                                     /* gettid */
    case 172: return 1;                                                 /* getpid */
    case 173: return 0;                                                 /* getppid */
    case 174: case 175: case 176: case 177: return 0;                   /* uid/gid */
    case 96:  return hbxl_gettid();                                     /* set_tid_address */
    case 99:  return 0;                                                 /* set_robust_list */
    case 93:  hbxl_thread_exit((int)a);                                 /* exit */
    case 94:                                                            /* exit_group */
        hbxl_logf("hbxl: exit_group(%d)", a);
        hbxl_return_to_host(a);
    case 124: hbxl_svc_sleep(0); return 0;                              /* sched_yield */
    case 123: return sys_sched_getaffinity(a, (size_t)b, (uint8_t *)c);
    case 122: return 0;                                                 /* sched_setaffinity */
    case 113: return sys_clock_gettime(a, (struct kts *)b);
    case 114: if (b) { ((struct kts *)b)->sec = 0; ((struct kts *)b)->nsec = 1; } return 0;
    case 101: return sys_nanosleep((const struct kts *)a);
    case 115: return sys_clock_nanosleep(a, b, (const struct kts *)c);
    case 169: {                                                         /* gettimeofday */
        struct kts ts;
        sys_clock_gettime(0, &ts);
        if (a) { ((int64_t *)a)[0] = ts.sec; ((int64_t *)a)[1] = ts.nsec / 1000; }
        return 0;
    }
    case 278: return sys_getrandom((uint8_t *)a, (size_t)b);
    case 160: return sys_uname((struct utsname_k *)a);
    case 134: case 135: case 132: return 0;                             /* sigaction/mask/altstack */
    case 129: case 130: case 131:                                       /* kill/tkill/tgkill */
        hbxl_logf("hbxl: signal %d raised; exiting", n == 131 ? c : b);
        log_backtrace();
        hbxl_return_to_host(128 + (n == 131 ? c : b));
    case 261: case 163:                                                 /* prlimit64/getrlimit */
        if (n == 261 && d) { ((struct rlimit_k *)d)->cur = 8u << 20; ((struct rlimit_k *)d)->max = ~0ull; }
        if (n == 163 && b) { ((struct rlimit_k *)b)->cur = 8u << 20; ((struct rlimit_k *)b)->max = ~0ull; }
        return 0;
    case 283: return 0;                                                 /* membarrier */
    case 179: return -HBXL_ENOSYS;                                      /* sysinfo */
    case 291: return -HBXL_ENOSYS;                                      /* statx -> fstatat */
    case 29:                                                            /* ioctl */
        /* stdout/stderr claim to be a terminal so musl line-buffers them;
         * fully buffered logs lose their last kilobyte when the app dies. */
        if ((a == 1 || a == 2) && b == 0x5413 && c) {                   /* TIOCGWINSZ */
            ((uint16_t *)c)[0] = 50; ((uint16_t *)c)[1] = 160;
            ((uint16_t *)c)[2] = 0;  ((uint16_t *)c)[3] = 0;
            return 0;
        }
        return -25;                                                     /* ENOTTY */
    default:
        if (n >= 0 && n < 512 && !g_warned[n]) {
            g_warned[n] = 1;
            hbxl_logf("hbxl: unsupported syscall %d", n);
        }
        return -HBXL_ENOSYS;
    }
}

/* musl entry points we replace: syscalls with cancellation and the thread
 * stack self-unmap (see sdk/linux/thread.c for __clone). */
long __syscall_cp_asm(volatile int *cancel, long n, long a, long b, long c, long d, long e, long f) {
    (void)cancel;
    return __hbx_syscall(n, a, b, c, d, e, f);
}

/* ------------------------------------------------------------------ */
/* SDK heap for system services                                       */
/* ------------------------------------------------------------------ */

/* Some services (AudioOut buffers) need memory from the SDK heap rather
 * than the Vulkan-backed mmap pool. */
void *hbxl_sdk_memalign(size_t align, size_t size) {
    void *p = 0;
    if (HBXL_CALL(IMP_MEMALIGN, int (*)(void **, uint64_t, uint64_t))(&p, align, size) != 0) return 0;
    return p;
}

void hbxl_sdk_free(void *p) {
    if (p) HBXL_CALL(IMP_FREE, void (*)(void *))(p);
}

/* An SDK function by its offset in sdk/sdk-symbols.json (touch screen,
 * AudioIn, ...).  The SDK base follows from a resolved import. */
void *hbxl_sdk_proc(uint64_t offset) {
    return (void *)(uintptr_t)(hbxl_imp[IMP_MEMALIGN] - 0x577710ull + offset);
}

/* ------------------------------------------------------------------ */
/* Vulkan for Dawn and other loaders                                  */
/* ------------------------------------------------------------------ */

/* Dawn's Switch path (ports/dawn) and similar ICD-style loaders look this up
 * instead of dlopen("libvulkan.so"): forward to the console's loader from the
 * host-built dispatch table. */
typedef void (*hbxl_vk_void)(void);
typedef hbxl_vk_void (*hbxl_vk_gipa)(void *instance, const char *name);

hbxl_vk_void vk_icdGetInstanceProcAddr(void *instance, const char *name) {
    const uint64_t *d = (const uint64_t *)(uintptr_t)hbxl_ctx->gpu.dispatch;
    hbxl_vk_gipa gipa;
    if (!d) return 0;
    gipa = (hbxl_vk_gipa)(uintptr_t)d[HBXL_DISPATCH_GIPA];
    return gipa ? gipa(instance, name) : 0;
}

/* The display window for exclusive apps (Aurora's Dawn surface binding). */
void *hbx_switch2_vi_window(void) {
    return hbxl_ctx ? (void *)(uintptr_t)hbxl_ctx->gpu.vi_window : 0;
}

hbxl_vk_void vkGetInstanceProcAddr(void *instance, const char *name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

/* ------------------------------------------------------------------ */
/* Crash reports                                                      */
/* ------------------------------------------------------------------ */

extern char __hbxl_text_start[], __hbxl_text_end[];
static uint8_t g_exc_stack[64 * 1024] __attribute__((aligned(16)));
static uint64_t g_exc_info[256] __attribute__((aligned(16)));

/* nn::os::UserExceptionInfo's exact layout is not pinned for this SDK, so
 * dump every word and tag the ones that point into our code image; the PC
 * and LR are among them (symbolise with addr2line on the ELF). */
static void exception_handler(void *info) {
    const uint64_t *w = (const uint64_t *)info;
    uint64_t text = (uint64_t)(uintptr_t)__hbxl_text_start, end = (uint64_t)(uintptr_t)__hbxl_text_end;
    int i;
    hbxl_vfs_emergency();
    hbxl_logf("hbxl: EXCEPTION in tid %d, type 0x%x", hbxl_gettid(), w[0] & 0xffffffffu);
    for (i = 0; i < 80; i++) {
        if (w[i] >= text && w[i] < end)
            hbxl_logf("hbxl:   word %d = 0x%x  CODE+0x%x", (int64_t)i, w[i], w[i] - text);
        else if (w[i])
            hbxl_logf("hbxl:   word %d = 0x%x", (int64_t)i, w[i]);
    }
    hbxl_log("hbxl: end of exception report");
    HBXL_CALL(IMP_DEFAULT_EXCEPTION_HANDLER, void (*)(void *))(info);
}

static void install_exception_handler(void) {
    HBXL_CALL(IMP_SET_EXCEPTION_HANDLER, void (*)(void (*)(void *), void *, uint64_t, void *))(
        exception_handler, g_exc_stack, sizeof g_exc_stack, g_exc_info);
}

/* ------------------------------------------------------------------ */
/* Exit                                                               */
/* ------------------------------------------------------------------ */

void hbxl_restore_host(long code) __attribute__((noreturn));
void hbxl_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);
extern uint64_t hbxl_host_frame[24];

static void final_cleanup(void *arg) {
    hbxl_vfs_shutdown();
    /* Threads that never reached a wait or syscall still run on this memory:
     * leaking it beats crashing SmileBASIC under them. */
    if (!hbxl_threads_live()) hbxl_mem_release_all();
    hbxl_restore_host((long)(intptr_t)arg);
}

/* Only SmileBASIC's thread (tid 1) may return to the host.  Any other thread
 * records the code and ends itself; the main thread notices in its futex
 * waits (sliced) and returns. */
/* Cleanup that must happen even when the app is stopped or aborts instead of
 * returning normally: system sessions (AudioOut, ...) outlive the app and
 * the console only has a few of them. */
static void (*g_exit_hooks[8])(void);
static int g_exit_hook_count;

void hbxl_on_exit(void (*fn)(void)) {
    if (g_exit_hook_count < 8) g_exit_hooks[g_exit_hook_count++] = fn;
}

static void run_exit_hooks(void) {
    static int ran;
    int i;
    if (ran) return;
    ran = 1;
    for (i = g_exit_hook_count - 1; i >= 0; i--) g_exit_hooks[i]();
}

void hbxl_return_to_host(long code) {
    if (hbxl_gettid() != 1) {
        hbxl_exiting = (int)(code & 0xff) | 0x100;
        hbxl_thread_exit((int)code);
    }
    hbxl_exiting = (int)(code & 0xff) | 0x100;
    run_exit_hooks();
    hbxl_threads_reap(1);
    if (hbxl_threads_live())
        hbxl_logf("hbxl: %d thread(s) still running at exit; leaving app memory mapped",
                  hbxl_threads_live());
    hbxl_logf("hbxl: returning %d to the host (%u MiB still mapped)", code, hbxl_mem_used() >> 20);
    /* The program's stack lives in memory we are about to release: finish on
     * the host's stack, below the SP it entered with. */
    hbxl_call_on_stack(final_cleanup, (void *)(intptr_t)code,
                       (void *)(uintptr_t)(hbxl_host_frame[20] - 256));
    hbxl_restore_host(code);
}

/* ------------------------------------------------------------------ */
/* Startup                                                            */
/* ------------------------------------------------------------------ */

extern char __hbxl_text_start[], __hbxl_text_end[];
extern char __hbxl_eh_frame_hdr_start[], __hbxl_eh_frame_hdr_end[];
extern char __hbxl_tdata_start[], __hbxl_tdata_end[], __hbxl_tbss_end[];
extern char __hbxl_data_start[], __hbxl_bss_end[];
extern char __hbxl_tls_align[];

struct phdr64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static struct phdr64 g_phdrs[5];
static uint8_t g_random16[16];
static long g_initial[96];
static int g_argc = 1;

int main(int, char **, char **);
int __libc_start_main(int (*)(int, char **, char **), int, char **, void (*)(void),
                      void (*)(void), void (*)(void));

static void run_libc(void *unused) {
    (void)unused;
    if (g_crashtest) {
        hbxl_log("hbxl: crash test: writing to address 0x10");
        volatile uintptr_t bad = 0x10;
        *(volatile int *)bad = 1;
    }
    hbxl_log("hbxl: entering __libc_start_main");
    __libc_start_main(main, g_argc, (char **)g_initial, 0, 0, 0);
    hbxl_return_to_host(0);
}

static void resolve_imports(void) {
    uint64_t base, i;
    const uint64_t *table = (const uint64_t *)(uintptr_t)hbxl_ctx->args[0];
    (void)table;
    base = hbxl_ctx->audio.initialize_parameter - 0x1c7f30ull;
    for (i = 0; i < IMP_COUNT; i++) hbxl_imp[i] = base + import_offsets[i];
}

void hbxl_entry(const struct hbx_ctx *ctx) {
    size_t i = 0;
    void *stack;
    const size_t main_stack = 8u << 20;

    hbxl_ctx = ctx;
    if (ctx->tick_hz) g_tick_hz = ctx->tick_hz;
    if (!ctx->audio.initialize_parameter) {
        hbxl_log("hbxl: no SDK base (host resolved no audio entrypoints)");
        hbxl_restore_host(2);
    }
    resolve_imports();
    /* Connect the PC console first, so every later step is visible live. */
    if (hbxl_vfs_init() != 0) hbxl_log("hbxl: file server not reachable; files unavailable");
    hbxl_logf("hbxl: entry ok, sdk 0x%x, stack top 0x%x", hbxl_imp[IMP_MEMALIGN] - 0x577710ull,
              ctx->stack_base + ctx->stack_size);
    if (hbxl_mem_init() != 0) {
        hbxl_log("hbxl: no memory");
        hbxl_restore_host(3);
    }
    hbxl_log("hbxl: memory ready");
    hbxl_threads_init();
    hbxl_log("hbxl: threads ready");
    install_exception_handler();
    hbxl_log("hbxl: exception handler installed");

    /* Synthetic program headers for musl (TLS) and the unwinder (eh_frame). */
    g_phdrs[0].type = 1;                               /* PT_LOAD: the code image */
    g_phdrs[0].flags = 5;
    g_phdrs[0].vaddr = g_phdrs[0].paddr = (uint64_t)(uintptr_t)__hbxl_text_start;
    g_phdrs[0].filesz = g_phdrs[0].memsz = (uint64_t)(__hbxl_text_end - __hbxl_text_start);
    g_phdrs[0].align = 0x1000;
    g_phdrs[1].type = 1;                               /* PT_LOAD: data */
    g_phdrs[1].flags = 6;
    g_phdrs[1].vaddr = g_phdrs[1].paddr = (uint64_t)(uintptr_t)__hbxl_data_start;
    g_phdrs[1].filesz = g_phdrs[1].memsz = (uint64_t)(__hbxl_bss_end - __hbxl_data_start);
    g_phdrs[1].align = 0x1000;
    g_phdrs[2].type = 0x6474e550;                      /* PT_GNU_EH_FRAME */
    g_phdrs[2].flags = 4;
    g_phdrs[2].vaddr = g_phdrs[2].paddr = (uint64_t)(uintptr_t)__hbxl_eh_frame_hdr_start;
    g_phdrs[2].filesz = g_phdrs[2].memsz =
        (uint64_t)(__hbxl_eh_frame_hdr_end - __hbxl_eh_frame_hdr_start);
    g_phdrs[2].align = 4;
    g_phdrs[3].type = 7;                               /* PT_TLS */
    g_phdrs[3].flags = 4;
    g_phdrs[3].vaddr = g_phdrs[3].paddr = (uint64_t)(uintptr_t)__hbxl_tdata_start;
    g_phdrs[3].filesz = (uint64_t)(__hbxl_tdata_end - __hbxl_tdata_start);
    g_phdrs[3].memsz = (uint64_t)(__hbxl_tbss_end - __hbxl_tdata_start);
    g_phdrs[3].align = (uint64_t)(uintptr_t)__hbxl_tls_align;
    /* PT_GNU_STACK sets musl's default thread stack (else 128 KiB, and our
     * stacks have no guard page: Tint/Dawn recursion overflowed silently). */
    g_phdrs[4].type = 0x6474e551;                      /* PT_GNU_STACK */
    g_phdrs[4].flags = 6;
    g_phdrs[4].memsz = 8u << 20;
    sys_getrandom(g_random16, sizeof g_random16);

    /* argv, envp, auxv laid out the way the kernel would.  /hbxl-env on the
     * file server adds environment (one KEY=VALUE per line; earlier entries
     * win in getenv, so these override the defaults below) and
     * HBXL_ARGS=<words> adds command-line arguments. */
    {
        static char env_text[4096];
        char *lines[40];
        int nlines = 0, k;
        int handled = 0;
        long fd = hbxl_vfs_syscall(56, -100, (long)"/hbxl-env", 0, 0, 0, 0, &handled);
        if (fd >= 0) {
            long n = hbxl_vfs_syscall(63, fd, (long)env_text, sizeof env_text - 1, 0, 0, 0, &handled);
            char *p = env_text;
            hbxl_vfs_syscall(57, fd, 0, 0, 0, 0, 0, &handled);
            if (n > 0) {
                env_text[n] = 0;
                while (*p && nlines < 32) {
                    char *line = p;
                    while (*p && *p != '\n') p++;
                    if (*p) *p++ = 0;
                    if (*line && *line != '#') lines[nlines++] = line;
                }
            }
        }
        g_initial[i++] = (long)"hbxl-app";
        g_argc = 1;
        for (k = 0; k < nlines; k++) {
            char *w;
            if (!env_is(lines[k], "HBXL_ARGS=")) continue;
            hbxl_logf("hbxl: args %s", lines[k] + 10);
            for (w = lines[k] + 10; *w && g_argc < 16;) {
                while (*w == ' ') *w++ = 0;
                if (!*w) break;
                g_initial[i++] = (long)w;
                g_argc++;
                while (*w && *w != ' ') w++;
            }
        }
        g_initial[i++] = 0;
        for (k = 0; k < nlines; k++) {
            char *line = lines[k];
            if (env_is(line, "HBXL_ARGS=")) continue;
            g_initial[i++] = (long)line;
            hbxl_logf("hbxl: env %s", line);
            if (env_is(line, "HBXL_TRACE=1")) g_trace = 1;
            if (env_is(line, "HBXL_CRASHTEST=1")) g_crashtest = 1;
            if (env_is(line, "HBXL_PRELOAD=")) hbxl_vfs_set_preload(line + 13);
        }
    }
    g_initial[i++] = (long)"HOME=/home";
    g_initial[i++] = (long)"PWD=/";
    g_initial[i++] = (long)"LANG=C.UTF-8";
    /* SDL3: our offscreen video (Dawn owns the real vi surface) and the
     * AudioOut driver from ports/sdl3-hbx-switch2.diff. */
    g_initial[i++] = (long)"SDL_VIDEO_DRIVER=offscreen";
    g_initial[i++] = (long)"SDL_AUDIO_DRIVER=hbx";
    g_initial[i++] = 0;
    g_initial[i++] = 3;  g_initial[i++] = (long)g_phdrs;               /* AT_PHDR */
    g_initial[i++] = 4;  g_initial[i++] = sizeof(struct phdr64);       /* AT_PHENT */
    g_initial[i++] = 5;  g_initial[i++] = 5;                           /* AT_PHNUM */
    g_initial[i++] = 6;  g_initial[i++] = 4096;                        /* AT_PAGESZ */
    g_initial[i++] = 16; g_initial[i++] = (1 << 0) | (1 << 1) | (1 << 8); /* AT_HWCAP: fp asimd atomics */
    g_initial[i++] = 23; g_initial[i++] = 0;                           /* AT_SECURE */
    g_initial[i++] = 25; g_initial[i++] = (long)g_random16;            /* AT_RANDOM */
    g_initial[i++] = 0;  g_initial[i++] = 0;

    stack = hbxl_mmap(main_stack);
    if (!stack) hbxl_return_to_host(3);
    hbxl_logf("hbxl: main stack 0x%x", (uint64_t)(uintptr_t)stack);
    hbxl_logf("hbxl: starting main, %u MiB mapped so far", hbxl_mem_used() >> 20);
    hbxl_call_on_stack(run_libc, 0, (char *)stack + main_stack);
    hbxl_return_to_host(0);
}
