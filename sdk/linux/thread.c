/*
 * thread.c - clone/exit/futex for the Linux personality.
 *
 * clone() becomes an nn::os thread pinned round-robin over the available
 * cores (pthread_create in the SDK lands everything on core 0; pinned nn::os
 * threads gave 5.4x on six cores).  The thread starts on a small SDK-heap
 * boot stack, installs musl's TLS in TPIDR_EL0 and runs the musl function on
 * musl's own stack; exit jumps back to the boot stack, performs
 * CLONE_CHILD_CLEARTID (musl's thread list depends on it) and lets nn::os
 * finish.  Finished threads are reaped (WaitThread + DestroyThread) later.
 *
 * futex() maps to Horizon address arbitration (SVC 0x34/0x35).  If the kernel
 * refuses an address (memory it cannot arbitrate), we fall back to sleeping
 * and spurious wakeups, which futex users must tolerate anyway.
 */
#include "hbxl.h"

#define MAX_THREADS 128
#define BOOT_STACK (64u * 1024u)
#define NN_THREAD_BYTES 0x1000         /* nn::os::ThreadType is 0x1c0 on Switch 1; generous */

#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

struct frame { uint64_t words[22]; };

struct thread {
    uint8_t nn[NN_THREAD_BYTES] __attribute__((aligned(64)));  /* nn::os::ThreadType */
    volatile int state;          /* 0 free, 1 running, 2 finished (reap), 3 starting */
    int tid;
    int (*fn)(void *);
    void *arg;
    void *stack_top;
    void *tls;
    volatile int32_t *ctid;
    void *boot;
    void *unmap_base;
    size_t unmap_size;
    struct frame exit_frame;
};

int hbxl_frame_save(struct frame *f);
void hbxl_frame_jump(struct frame *f) __attribute__((noreturn));
void hbxl_set_tp(void *tp);
void hbxl_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);

static struct thread *g_threads[MAX_THREADS];
static volatile int g_lock;
static int g_next_tid = 2;
static int g_next_core;
static uint64_t g_core_mask = 1;
static int g_arbitration_broken;

static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) hbxl_svc_sleep(0);
}
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

void hbxl_threads_init(void) {
    g_core_mask = HBXL_CALL(IMP_OS_COREMASK, uint64_t (*)(void))();
    if (!g_core_mask) g_core_mask = 1;
    g_next_core = 1;
}

static struct thread *current(void) {
    void *nn = HBXL_CALL(IMP_OS_CURRENT, void *(*)(void))();
    int i;
    for (i = 0; i < MAX_THREADS; i++)
        if (g_threads[i] && (void *)g_threads[i]->nn == nn) return g_threads[i];
    return 0;                                            /* the host (main) thread */
}

long hbxl_gettid(void) {
    struct thread *t = current();
    return t ? t->tid : 1;
}

int hbxl_threads_live(void) {
    int i, n = 0;
    for (i = 0; i < MAX_THREADS; i++)
        if (g_threads[i] && (g_threads[i]->state == 1 || g_threads[i]->state == 3)) n++;
    return n;
}

static void destroy(int slot) {
    struct thread *t = g_threads[slot];
    HBXL_CALL(IMP_OS_WAIT, void (*)(void *))(t->nn);
    HBXL_CALL(IMP_OS_DESTROY, void (*)(void *))(t->nn);
    if (t->boot) HBXL_CALL(IMP_FREE, void (*)(void *))(t->boot);
    g_threads[slot] = 0;
    hbxl_munmap(t, sizeof *t);
}

/* Reap finished threads.  With `wait`, give running ones up to 2 s to end. */
void hbxl_threads_reap(int wait) {
    int i, rounds = wait ? 200 : 1;
    while (rounds--) {
        lock();
        for (i = 0; i < MAX_THREADS; i++)
            if (g_threads[i] && g_threads[i]->state == 2) destroy(i);
        unlock();
        if (!wait || !hbxl_threads_live()) break;
        hbxl_svc_sleep(10000000);
    }
}

static void thread_body(void *arg) {
    struct thread *t = arg;
    int r = t->fn(t->arg);
    hbxl_thread_exit(r);
}

static void thread_entry(void *arg) {
    struct thread *t = arg;
    hbxl_set_tp(t->tls);
    t->state = 1;
    if (hbxl_frame_save(&t->exit_frame) == 0)
        hbxl_call_on_stack(thread_body, t, t->stack_top);
    /* Back on the boot stack after exit. */
    hbxl_set_tp(0);
    if (t->unmap_base) hbxl_munmap(t->unmap_base, t->unmap_size);
    if (t->ctid) {
        __atomic_store_n(t->ctid, 0, __ATOMIC_SEQ_CST);
        hbxl_svc_signal_to_address(t->ctid, 0, 0, -1);
    }
    __atomic_store_n(&t->state, 2, __ATOMIC_RELEASE);
}

void hbxl_thread_exit(int code) {
    struct thread *t = current();
    (void)code;
    if (!t) {
        /* The host thread called exit(): treat it as the program ending. */
        hbxl_return_to_host(code);
    }
    hbxl_frame_jump(&t->exit_frame);
}

/* musl: detached threads unmap their own stack, then exit. */
void __unmapself(void *base, size_t size) {
    struct thread *t = current();
    if (t) { t->unmap_base = base; t->unmap_size = size; }
    hbxl_thread_exit(0);
}

int __clone(int (*fn)(void *), void *stack, int flags, void *arg, ...) {
    __builtin_va_list ap;
    int32_t *ptid, *ctid;
    void *tls;
    struct thread *t;
    int slot, core, prio, r, tid, tries;
    void *boot = 0;

    __builtin_va_start(ap, arg);
    ptid = __builtin_va_arg(ap, int32_t *);
    tls = __builtin_va_arg(ap, void *);
    ctid = __builtin_va_arg(ap, int32_t *);
    __builtin_va_end(ap);

    hbxl_threads_reap(0);
    t = hbxl_mmap(sizeof *t);
    if (!t) return -HBXL_EAGAIN;
    if (HBXL_CALL(IMP_MEMALIGN, int (*)(void **, uint64_t, uint64_t))(&boot, 4096, BOOT_STACK) != 0 || !boot) {
        hbxl_munmap(t, sizeof *t);
        return -HBXL_EAGAIN;
    }
    lock();
    for (slot = 0; slot < MAX_THREADS && g_threads[slot]; slot++) {}
    if (slot == MAX_THREADS) { unlock(); hbxl_munmap(t, sizeof *t); return -HBXL_EAGAIN; }
    tid = g_next_tid++;
    t->tid = tid;
    t->fn = fn;
    t->arg = arg;
    t->stack_top = stack;
    t->tls = tls;
    t->boot = boot;
    t->ctid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    t->state = 3;
    g_threads[slot] = t;
    /* Round-robin over the available cores, skipping core 0 (the host's). */
    for (tries = 0; tries < 64; tries++) {
        core = g_next_core++ % 64;
        if ((g_core_mask >> core) & 1) {
            if (core == 0 && (g_core_mask & ~1ull)) continue;
            break;
        }
    }
    unlock();

    if (flags & CLONE_PARENT_SETTID) *ptid = tid;
    if (flags & CLONE_CHILD_SETTID) *ctid = tid;
    prio = HBXL_CALL(IMP_OS_PRIORITY, int (*)(void *))(HBXL_CALL(IMP_OS_CURRENT, void *(*)(void))());
    r = HBXL_CALL(IMP_OS_CREATE, int (*)(void *, void (*)(void *), void *, void *, uint64_t, int, int))(
        t->nn, thread_entry, t, boot, BOOT_STACK, prio, core);
    if (r != 0) {
        hbxl_logf("hbxl: CreateThread failed %x (core %d)", (uint64_t)(uint32_t)r, (int64_t)core);
        lock();
        g_threads[slot] = 0;
        unlock();
        HBXL_CALL(IMP_FREE, void (*)(void *))(boot);
        hbxl_munmap(t, sizeof *t);
        return -HBXL_EAGAIN;
    }
    HBXL_CALL(IMP_OS_START, void (*)(void *))(t->nn);
    hbxl_logf("hbxl: thread %d started on core %d", (int64_t)tid, (int64_t)core);
    return tid;
}

/* ------------------------------------------------------------------ */
/* futex                                                              */
/* ------------------------------------------------------------------ */

#define RESULT_TIMED_OUT 0xEA01
#define RESULT_INVALID_STATE 0xFA01
#define RESULT_CANCELLED 0xEC01

struct kts { int64_t sec, nsec; };

static int64_t monotonic_now_ns(void) {
    struct kts ts;
    extern long __hbx_syscall(long, long, long, long, long, long, long);
    __hbx_syscall(113, 1, (long)&ts, 0, 0, 0, 0);
    return ts.sec * 1000000000ll + ts.nsec;
}

static long wait_on(volatile int32_t *addr, int32_t val, int64_t timeout_ns) {
    int is_main = hbxl_gettid() == 1;
    int64_t deadline = timeout_ns >= 0 ? monotonic_now_ns() + timeout_ns : -1;
    for (;;) {
        int64_t slice = timeout_ns;
        int32_t r;
        /* Every thread waits in slices so an exit can drain them: the host
         * thread goes home, the others end themselves (their stacks and heap
         * are about to be released). */
        if (hbxl_exiting || hbxl_stop_requested()) {
            if (is_main) hbxl_return_to_host(hbxl_exiting & 0xff);
            hbxl_thread_exit(0);
        }
        slice = 50000000;
        if (deadline >= 0) {
            int64_t left = deadline - monotonic_now_ns();
            if (left <= 0) return -HBXL_ETIMEDOUT;
            if (left < slice) slice = left;
        }
        if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) return -HBXL_EAGAIN;
        if (g_arbitration_broken) {
            hbxl_svc_sleep(200000);
            return 0;                                    /* spurious wakeup */
        }
        r = hbxl_svc_wait_for_address(addr, 2 /* WaitIfEqual */, val, slice);
        if (r == 0) return 0;
        if ((r & 0x3fffff) == RESULT_INVALID_STATE) return -HBXL_EAGAIN;
        if ((r & 0x3fffff) == RESULT_TIMED_OUT) {
            if (deadline >= 0 && monotonic_now_ns() >= deadline) return -HBXL_ETIMEDOUT;
            if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) return 0;
            continue;
        }
        if ((r & 0x3fffff) == RESULT_CANCELLED) return -HBXL_EINTR;
        hbxl_logf("hbxl: WaitForAddress failed %x at %x; using polling futexes",
                  (uint64_t)(uint32_t)r, (uint64_t)(uintptr_t)addr);
        g_arbitration_broken = 1;
    }
}

static long wake(volatile int32_t *addr, int32_t count) {
    if (g_arbitration_broken) return 0;
    hbxl_svc_signal_to_address(addr, 0 /* Signal */, 0, count <= 0 ? -1 : count);
    return 0;
}

long hbxl_futex(volatile int32_t *addr, int op, int32_t val, const void *timeout,
                volatile int32_t *addr2, int32_t val3) {
    int cmd = op & 127;
    const struct kts *ts = (const struct kts *)timeout;
    int64_t ns = -1;
    (void)val3;
    switch (cmd) {
    case 0:                                             /* FUTEX_WAIT (relative) */
        if (ts) ns = ts->sec * 1000000000ll + ts->nsec;
        return wait_on(addr, val, ns);
    case 9:                                             /* FUTEX_WAIT_BITSET (absolute) */
        if (ts) {
            ns = ts->sec * 1000000000ll + ts->nsec - monotonic_now_ns();
            if (op & 256) ns -= hbxl_vfs_time_offset_ns();   /* CLOCK_REALTIME */
            if (ns < 0) ns = 0;
        }
        return wait_on(addr, val, ns);
    case 1: case 10:                                    /* FUTEX_WAKE(_BITSET) */
        return wake(addr, val);
    case 3: case 4:                                     /* (CMP_)REQUEUE: wake everyone */
        wake(addr, -1);
        if (addr2) wake(addr2, -1);
        return 0;
    case 5:                                             /* FUTEX_WAKE_OP */
        wake(addr, -1);
        if (addr2) wake(addr2, -1);
        return 0;
    default:
        return -HBXL_ENOSYS;
    }
}
