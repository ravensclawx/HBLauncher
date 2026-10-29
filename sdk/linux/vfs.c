/*
 * vfs.c - files and console over TCP for the Linux personality.
 *
 * One connection to tools/hbxl-fileserver.py on the PC (10.42.0.1, port
 * HBXL_FS_PORT or 39501),
 * shared by all threads under a lock.  Requests and replies are small framed
 * messages:
 *
 *   request  u32 magic 'HBFS', u32 op, u32 length, u32 seq, payload
 *   reply    u32 magic 'HBFS', i32 status, u32 length, u32 seq, payload
 *
 * Linux file descriptors 3.. map to server handles plus a local offset;
 * 0 reads empty, 1/2 stream to the PC console (op LOG, no reply).
 */
#include "hbxl.h"

#define FS_MAGIC 0x53464248u
#define OP_HELLO 1
#define OP_OPEN 2
#define OP_CLOSE 3
#define OP_PREAD 4
#define OP_PWRITE 5
#define OP_STAT 6
#define OP_FSTAT 7
#define OP_LISTDIR 8
#define OP_LOG 9
#define OP_MKDIR 10
#define OP_UNLINK 11
#define OP_RENAME 12
#define OP_TRUNCATE 13

#define MAX_FDS 1024
#define PATH_MAX_K 1024
#define CHUNK (4u << 20)

struct fd {
    int used;
    int handle;
    int is_dir;
    int flags;
    uint64_t offset;
    char path[256];
    uint8_t *dir;               /* LISTDIR payload */
    uint32_t dir_len, dir_pos;
    int refs;
    int cached;                 /* served from g_preload, no server handle */
};

static struct fd g_fds[MAX_FDS];
static int g_sock = -1;
static volatile int g_lock;
static uint32_t g_seq;
static int64_t g_time_offset;
static uint8_t g_scratch[64 * 1024];

/* Set by the exception handler: the faulting thread may hold g_lock, so the
 * crash report bypasses it (other threads are stopped or about to be). */
static volatile int g_emergency;

/* Re-entrant: code under the lock may log (the allocator logs new chunks,
 * the preload logs progress), and logging takes the lock again. */
static volatile long g_lock_owner;
static int g_lock_depth;

static void lock(void) {
    long me;
    if (g_emergency) return;
    me = hbxl_gettid();
    if (__atomic_load_n(&g_lock_owner, __ATOMIC_ACQUIRE) == me) { g_lock_depth++; return; }
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) hbxl_svc_sleep(0);
    __atomic_store_n(&g_lock_owner, me, __ATOMIC_RELEASE);
    g_lock_depth = 1;
}
static void unlock(void) {
    if (g_emergency) return;
    if (--g_lock_depth > 0) return;
    __atomic_store_n(&g_lock_owner, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

void hbxl_vfs_emergency(void) { g_emergency = 1; }

int64_t hbxl_vfs_time_offset_ns(void) { return g_time_offset; }

/* ------------------------------------------------------------------ */
/* Transport                                                          */
/* ------------------------------------------------------------------ */

static int sock_errno(void) {
    return HBXL_CALL(IMP_ERRNO, int (*)(void))();
}

static int g_last_errno;

/* The socket service cannot read or write the app's Vulkan-backed memory
 * (EFAULT, errno 14), so all socket I/O goes through a bounce buffer taken
 * from the SDK heap.  EINTR is retried; EAGAIN is the 30 s receive timeout,
 * retried once (a pulled cable must fail in ~a minute, not hang the game
 * with the lock held); anything else drops the link. */
#define BOUNCE_BYTES (256u * 1024u)
static uint8_t *g_bounce;

static int send_raw(const uint8_t *p, size_t n) {
    int retries = 0;
    while (n) {
        int64_t r = HBXL_CALL(IMP_SEND, int64_t (*)(int, const void *, uint64_t, int))(g_sock, p, n, 0);
        if (r <= 0) {
            g_last_errno = r < 0 ? sock_errno() : 0;
            if (r < 0 && (g_last_errno == 4 || (g_last_errno == 11 && ++retries < 2))) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int recv_raw(uint8_t *p, size_t n) {
    int retries = 0;
    while (n) {
        int64_t r = HBXL_CALL(IMP_RECV, int64_t (*)(int, void *, uint64_t, int))(g_sock, p, n, 0);
        if (r <= 0) {
            g_last_errno = r < 0 ? sock_errno() : 0;
            if (r < 0 && (g_last_errno == 4 || (g_last_errno == 11 && ++retries < 2))) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int send_all(const void *data, size_t n) {
    const uint8_t *p = data;
    if (!g_bounce) return send_raw(p, n);
    while (n) {
        size_t k = n < BOUNCE_BYTES ? n : BOUNCE_BYTES;
        hbxl_memcpy(g_bounce, p, k);
        if (send_raw(g_bounce, k)) return -1;
        p += k;
        n -= k;
    }
    return 0;
}

static int recv_all(void *data, size_t n) {
    uint8_t *p = data;
    if (!g_bounce) return recv_raw(p, n);
    while (n) {
        size_t k = n < BOUNCE_BYTES ? n : BOUNCE_BYTES;
        if (recv_raw(g_bounce, k)) return -1;
        hbxl_memcpy(p, g_bounce, k);
        p += k;
        n -= k;
    }
    return 0;
}

/* Losing the file server silently loses every later read and log line, so
 * say why on the host's log ring (which needs no socket). */
static void drop_connection_why(const char *why, uint32_t op) {
    if (g_sock >= 0) {
        hbxl_ring_logf("hbxl: file server link lost (%s, op %u, errno %d, tid %d)", why,
                       (uint64_t)op, (int64_t)g_last_errno, hbxl_gettid());
        HBXL_CALL(IMP_CLOSE, int (*)(int))(g_sock);
    }
    g_sock = -1;
}

static void drop_connection(void) { drop_connection_why("shutdown", 0); }

/* One request/reply under the lock.  `out` receives up to `cap` bytes of
 * payload (the rest is drained).  Returns status, or -EIO when offline. */
static long transact(uint32_t op, const void *a, uint32_t alen, const void *b, uint32_t blen,
                     void *out, uint32_t cap, uint32_t *out_len) {
    uint32_t h[4], r[4];
    long status;
    if (g_sock < 0) return -HBXL_EIO;
    h[0] = FS_MAGIC; h[1] = op; h[2] = alen + blen; h[3] = ++g_seq;
    if (send_all(h, sizeof h) || (alen && send_all(a, alen)) || (blen && send_all(b, blen))) {
        drop_connection_why("send", op);
        return -HBXL_EIO;
    }
    if (recv_all(r, sizeof r)) { drop_connection_why("recv header", op); return -HBXL_EIO; }
    if (r[0] != FS_MAGIC) { drop_connection_why("bad reply magic", op); return -HBXL_EIO; }
    status = (int32_t)r[1];
    if (out_len) *out_len = r[2] < cap ? r[2] : cap;
    if (r[2]) {
        uint32_t take = r[2] < cap ? r[2] : cap;
        if (take && recv_all(out, take)) { drop_connection_why("recv payload", op); return -HBXL_EIO; }
        r[2] -= take;
        while (r[2]) {
            uint32_t k = r[2] < sizeof g_scratch ? r[2] : (uint32_t)sizeof g_scratch;
            if (recv_all(g_scratch, k)) { drop_connection_why("recv excess", op); return -HBXL_EIO; }
            r[2] -= k;
        }
    }
    return status;
}

void hbxl_vfs_write_console(int fd, const void *data, size_t length) {
    uint32_t h[4];
    uint32_t f = (uint32_t)fd;
    if (g_sock < 0 || !length) return;
    lock();
    h[0] = FS_MAGIC; h[1] = OP_LOG; h[2] = (uint32_t)(4 + length); h[3] = ++g_seq;
    if (send_all(h, sizeof h) || send_all(&f, 4) || send_all(data, length))
        drop_connection_why("send log", OP_LOG);
    unlock();
}

int hbxl_vfs_init(void) {
    if (!g_bounce) {
        void *b = 0;
        if (HBXL_CALL(IMP_MEMALIGN, int (*)(void **, uint64_t, uint64_t))(&b, 4096, BOUNCE_BYTES) == 0)
            g_bounce = b;
    }
    struct {
        uint8_t len, family;
        uint16_t port;
        uint32_t addr;
        uint8_t zero[8];
    } sa;
    struct { int64_t sec, usec; } timeout = {30, 0};
    int one = 1;
    int64_t now = 0;
    uint32_t got = 0;
    int s = HBXL_CALL(IMP_SOCKET, int (*)(int, int, int))(2, 1, 6);
    if (s < 0) return -1;
    hbxl_memset(&sa, 0, sizeof sa);
    sa.len = sizeof sa;
    sa.family = 2;
    {
        /* Each app may get its own file server (own root, saves, env): the
         * link passes --defsym=HBXL_FS_PORT=<port>; 39501 otherwise. */
        extern char HBXL_FS_PORT[] __attribute__((weak));
        uint32_t port = (uint32_t)(uintptr_t)HBXL_FS_PORT;
        if (!port || port > 65535) port = 39501;
        sa.port = (uint16_t)((port >> 8) | ((port & 0xff) << 8));
    }
    sa.addr = 10u | (42u << 8) | (0u << 16) | (1u << 24);
    HBXL_CALL(IMP_SETSOCKOPT, int (*)(int, int, int, const void *, uint32_t))(s, 0xffff, 0x1006, &timeout, sizeof timeout);
    HBXL_CALL(IMP_SETSOCKOPT, int (*)(int, int, int, const void *, uint32_t))(s, 6, 1, &one, sizeof one); /* TCP_NODELAY */
    if (HBXL_CALL(IMP_CONNECT, int (*)(int, const void *, uint32_t))(s, &sa, sizeof sa) < 0) {
        HBXL_CALL(IMP_CLOSE, int (*)(int))(s);
        return -1;
    }
    g_sock = s;
    if (transact(OP_HELLO, 0, 0, 0, 0, &now, sizeof now, &got) >= 0 && got == sizeof now) {
        uint64_t mono;
        extern long __hbx_syscall(long, long, long, long, long, long, long);
        struct { int64_t sec, nsec; } ts;
        __hbx_syscall(113, 1, (long)&ts, 0, 0, 0, 0);
        mono = (uint64_t)ts.sec * 1000000000ull + (uint64_t)ts.nsec;
        g_time_offset = now - (int64_t)mono;
    }
    hbxl_log("hbxl: file server connected");
    return 0;
}

void hbxl_vfs_shutdown(void) {
    int i;
    for (i = 3; i < MAX_FDS; i++) if (g_fds[i].used && g_fds[i].dir) hbxl_munmap(g_fds[i].dir, g_fds[i].dir_len);
    hbxl_memset(g_fds, 0, sizeof g_fds);
    drop_connection();
}

/* ------------------------------------------------------------------ */
/* Paths and descriptors                                              */
/* ------------------------------------------------------------------ */

static int join_path(int dirfd, const char *path, char *out) {
    size_t n = 0, k;
    if (!path) return -HBXL_EINVAL;
    if (path[0] != '/') {
        const char *base = "/";
        if (dirfd != -100) {
            if (dirfd < 3 || dirfd >= MAX_FDS || !g_fds[dirfd].used) return -HBXL_EBADF;
            base = g_fds[dirfd].path;
        }
        k = hbxl_strlen(base);
        if (k >= PATH_MAX_K - 2) return -HBXL_EINVAL;
        hbxl_memcpy(out, base, k);
        n = k;
        if (n && out[n - 1] != '/') out[n++] = '/';
    }
    k = hbxl_strlen(path);
    if (n + k >= PATH_MAX_K) return -36;               /* ENAMETOOLONG */
    hbxl_memcpy(out + n, path, k);
    out[n + k] = 0;
    return 0;
}

static int alloc_fd(void) {
    int i;
    for (i = 3; i < MAX_FDS; i++) if (!g_fds[i].used) { hbxl_memset(&g_fds[i], 0, sizeof g_fds[i]); g_fds[i].used = 1; return i; }
    return -24;                                         /* EMFILE */
}

struct kstat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    uint32_t unused[2];
};

struct fs_stat { uint64_t size; uint32_t mode, pad; int64_t mtime; };

static void fill_stat(struct kstat *st, const struct fs_stat *s, uint64_t ino) {
    hbxl_memset(st, 0, sizeof *st);
    st->dev = 1;
    st->ino = ino;
    st->mode = s->mode;
    st->nlink = 1;
    st->size = (int64_t)s->size;
    st->blksize = 4096;
    st->blocks = (int64_t)((s->size + 511) / 512);
    st->atime = st->mtime = st->ctime = s->mtime;
}

/* HBXL_PRELOAD=<path>: the first open copies that file into RAM and every
 * later open/read/fstat of it is served locally.  For the game disc, so the
 * game keeps running when the network goes away (handheld, no dock). */
static struct {
    char path[256];
    uint8_t *data;
    struct fs_stat st;
    int failed;
} g_preload;

void hbxl_vfs_set_preload(const char *path) {
    size_t k = hbxl_strlen(path);
    if (k > 255) k = 255;
    hbxl_memcpy(g_preload.path, path, k);
    g_preload.path[k] = 0;
}

static int same_path(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static long do_read(int fd, uint8_t *buf, size_t n, int64_t at, int advance);

/* Called with the lock held, right after `fd` was opened on the server. */
static void preload_fill(int fd) {
    uint32_t h = (uint32_t)g_fds[fd].handle, got = 0;
    uint64_t done = 0;
    if (transact(OP_FSTAT, &h, 4, 0, 0, &g_preload.st, sizeof g_preload.st, &got) < 0 ||
        got != sizeof g_preload.st || !g_preload.st.size) {
        g_preload.failed = 1;
        return;
    }
    g_preload.data = hbxl_mmap((size_t)g_preload.st.size);
    if (!g_preload.data) {
        hbxl_logf("hbxl: preload of %s (%u MiB) failed: no memory; streaming it instead",
                  g_preload.path, g_preload.st.size >> 20);
        g_preload.failed = 1;
        return;
    }
    hbxl_logf("hbxl: preloading %s (%u MiB) into RAM", g_preload.path, g_preload.st.size >> 20);
    while (done < g_preload.st.size) {
        uint64_t left = g_preload.st.size - done;
        size_t want = left < (64u << 20) ? (size_t)left : (64u << 20);
        long r = do_read(fd, g_preload.data + done, want, (int64_t)done, 0);
        if (r <= 0) {
            hbxl_logf("hbxl: preload read failed at %u MiB; streaming instead", done >> 20);
            hbxl_munmap(g_preload.data, (size_t)g_preload.st.size);
            g_preload.data = 0;
            g_preload.failed = 1;
            return;
        }
        done += (uint64_t)r;
    }
    hbxl_logf("hbxl: preload done; %s no longer needs the network", g_preload.path);
}

static long do_read(int fd, uint8_t *buf, size_t n, int64_t at, int advance) {
    struct fd *f;
    size_t total = 0;
    if (fd == 0) return 0;
    if (fd < 3 || fd >= MAX_FDS || !g_fds[fd].used) return -HBXL_EBADF;
    f = &g_fds[fd];
    if (f->is_dir) return -21;                          /* EISDIR */
    if (f->cached) {
        uint64_t off = at >= 0 ? (uint64_t)at : f->offset;
        if (off < g_preload.st.size) {
            total = g_preload.st.size - off < n ? (size_t)(g_preload.st.size - off) : n;
            hbxl_memcpy(buf, g_preload.data + off, total);
        }
        if (advance) f->offset += total;
        return (long)total;
    }
    while (total < n) {
        uint32_t want = (uint32_t)((n - total) < CHUNK ? (n - total) : CHUNK);
        uint32_t req[4], got = 0;
        uint64_t off = (at >= 0 ? (uint64_t)at : f->offset) + total;
        long r;
        req[0] = (uint32_t)f->handle; req[1] = want;
        hbxl_memcpy(&req[2], &off, 8);
        r = transact(OP_PREAD, req, 16, 0, 0, buf + total, want, &got);
        if (r < 0) return total ? (long)total : r;
        total += got;
        if (got < want) break;
    }
    if (advance) f->offset += total;
    return (long)total;
}

static long do_write(int fd, const uint8_t *buf, size_t n, int64_t at, int advance) {
    struct fd *f;
    size_t total = 0;
    if (fd == 1 || fd == 2) { hbxl_vfs_write_console(fd, buf, n); return (long)n; }
    if (fd < 3 || fd >= MAX_FDS || !g_fds[fd].used) return -HBXL_EBADF;
    f = &g_fds[fd];
    while (total < n) {
        uint32_t want = (uint32_t)((n - total) < CHUNK ? (n - total) : CHUNK);
        uint32_t req[4];
        uint64_t off = (at >= 0 ? (uint64_t)at : f->offset) + total;
        long r;
        req[0] = (uint32_t)f->handle; req[1] = (f->flags & 02000) ? 1u : 0u;   /* O_APPEND */
        hbxl_memcpy(&req[2], &off, 8);
        r = transact(OP_PWRITE, req, 16, buf + total, want, 0, 0, 0);
        if (r < 0) return total ? (long)total : r;
        total += (size_t)r;
        if ((uint32_t)r < want) break;
    }
    if (advance) f->offset += total;
    return (long)total;
}

struct iov { void *base; size_t len; };

/* Returns with *handled = 1 when the syscall is a file operation. */
long hbxl_vfs_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    char path[PATH_MAX_K];
    long r;
    (void)e; (void)f;
    *handled = 1;
    switch (n) {
    case 63:                                            /* read */
        lock(); r = do_read((int)a, (uint8_t *)b, (size_t)c, -1, 1); unlock(); return r;
    case 67:                                            /* pread64 */
        lock(); r = do_read((int)a, (uint8_t *)b, (size_t)c, d, 0); unlock(); return r;
    case 64:                                            /* write */
        if (a == 1 || a == 2) { hbxl_vfs_write_console((int)a, (const void *)b, (size_t)c); return c; }
        lock(); r = do_write((int)a, (const uint8_t *)b, (size_t)c, -1, 1); unlock(); return r;
    case 68:                                            /* pwrite64 */
        lock(); r = do_write((int)a, (const uint8_t *)b, (size_t)c, d, 0); unlock(); return r;
    case 65: case 66: {                                 /* readv / writev */
        const struct iov *v = (const struct iov *)b;
        long total = 0, i;
        for (i = 0; i < c; i++) {
            if (!v[i].len) continue;
            if (n == 66 && (a == 1 || a == 2)) { hbxl_vfs_write_console((int)a, v[i].base, v[i].len); r = (long)v[i].len; }
            else {
                lock();
                r = n == 65 ? do_read((int)a, v[i].base, v[i].len, -1, 1)
                            : do_write((int)a, v[i].base, v[i].len, -1, 1);
                unlock();
            }
            if (r < 0) return total ? total : r;
            total += r;
            if ((size_t)r < v[i].len) break;
        }
        return total;
    }
    case 56: {                                          /* openat(dirfd, path, flags, mode) */
        uint32_t hdr[2];
        int fd;
        r = join_path((int)a, (const char *)b, path);
        if (r) return r;
        hdr[0] = (uint32_t)c; hdr[1] = (uint32_t)d;
        lock();
        fd = alloc_fd();
        if (fd < 0) { unlock(); return fd; }
        g_fds[fd].cached = 0;
        if (g_preload.data && (c & 3) == 0 && same_path(path, g_preload.path)) {
            g_fds[fd].handle = -1;                      /* served from RAM */
            g_fds[fd].is_dir = 0;
            g_fds[fd].flags = (int)c;
            g_fds[fd].refs = 1;
            g_fds[fd].offset = 0;
            g_fds[fd].cached = 1;
            hbxl_memcpy(g_fds[fd].path, path, hbxl_strlen(path) + 1);
            unlock();
            return fd;
        }
        r = transact(OP_OPEN, hdr, 8, path, (uint32_t)hbxl_strlen(path) + 1, 0, 0, 0);
        if (r < 0) { g_fds[fd].used = 0; unlock(); return r; }
        g_fds[fd].handle = (int)(r & 0x7fffffff);
        g_fds[fd].is_dir = (r >> 30) & 1;
        g_fds[fd].handle &= 0x3fffffff;
        g_fds[fd].flags = (int)c;
        g_fds[fd].refs = 1;
        { size_t k = hbxl_strlen(path); if (k > 255) k = 255; hbxl_memcpy(g_fds[fd].path, path, k); g_fds[fd].path[k] = 0; }
        if (g_preload.path[0] && !g_preload.data && !g_preload.failed && !g_fds[fd].is_dir &&
            (c & 3) == 0 && same_path(path, g_preload.path))
            preload_fill(fd);
        unlock();
        return fd;
    }
    case 57: {                                          /* close */
        int fd = (int)a;
        uint32_t h;
        if (fd >= 0 && fd < 3) return 0;
        if (fd < 0 || fd >= MAX_FDS || !g_fds[fd].used) return -HBXL_EBADF;
        lock();
        if (--g_fds[fd].refs <= 0 && !g_fds[fd].cached) {
            h = (uint32_t)g_fds[fd].handle;
            transact(OP_CLOSE, &h, 4, 0, 0, 0, 0, 0);
            if (g_fds[fd].dir) hbxl_munmap(g_fds[fd].dir, g_fds[fd].dir_len);
        }
        g_fds[fd].used = 0;
        unlock();
        return 0;
    }
    case 62: {                                          /* lseek(fd, off, whence) */
        int fd = (int)a;
        struct fs_stat s;
        uint32_t got;
        if (fd < 3 || fd >= MAX_FDS || !g_fds[fd].used) return -29; /* ESPIPE for stdio */
        if (c == 0) g_fds[fd].offset = (uint64_t)b;
        else if (c == 1) g_fds[fd].offset += (uint64_t)b;
        else if (c == 2 && g_fds[fd].cached) g_fds[fd].offset = g_preload.st.size + (uint64_t)b;
        else if (c == 2) {
            uint32_t h = (uint32_t)g_fds[fd].handle;
            lock();
            r = transact(OP_FSTAT, &h, 4, 0, 0, &s, sizeof s, &got);
            unlock();
            if (r < 0) return r;
            g_fds[fd].offset = s.size + (uint64_t)b;
        } else return -HBXL_EINVAL;
        return (long)g_fds[fd].offset;
    }
    case 80: {                                          /* fstat */
        int fd = (int)a;
        struct fs_stat s;
        uint32_t got = 0, h;
        if (fd >= 0 && fd < 3) {
            hbxl_memset((void *)b, 0, sizeof(struct kstat));
            ((struct kstat *)b)->mode = 0020620;         /* char device */
            return 0;
        }
        if (fd < 0 || fd >= MAX_FDS || !g_fds[fd].used) return -HBXL_EBADF;
        if (g_fds[fd].cached) {
            fill_stat((struct kstat *)b, &g_preload.st, (uint64_t)fd + 100);
            return 0;
        }
        h = (uint32_t)g_fds[fd].handle;
        lock();
        r = transact(OP_FSTAT, &h, 4, 0, 0, &s, sizeof s, &got);
        unlock();
        if (r < 0) return r;
        fill_stat((struct kstat *)b, &s, (uint64_t)fd + 100);
        return 0;
    }
    case 79: {                                          /* newfstatat(dirfd, path, st, flags) */
        struct fs_stat s;
        uint32_t got = 0;
        if (b && !((const char *)b)[0] && (d & 0x1000)) /* AT_EMPTY_PATH */
            return hbxl_vfs_syscall(80, a, c, 0, 0, 0, 0, handled);
        r = join_path((int)a, (const char *)b, path);
        if (r) return r;
        if (g_preload.data && same_path(path, g_preload.path)) {
            fill_stat((struct kstat *)c, &g_preload.st, 1);
            return 0;
        }
        lock();
        r = transact(OP_STAT, path, (uint32_t)hbxl_strlen(path) + 1, 0, 0, &s, sizeof s, &got);
        unlock();
        if (r < 0) return r;
        fill_stat((struct kstat *)c, &s, 1);
        return 0;
    }
    case 48: case 439: {                                /* faccessat(2) */
        struct fs_stat s;
        uint32_t got = 0;
        r = join_path((int)a, (const char *)b, path);
        if (r) return r;
        lock();
        r = transact(OP_STAT, path, (uint32_t)hbxl_strlen(path) + 1, 0, 0, &s, sizeof s, &got);
        unlock();
        return r < 0 ? r : 0;
    }
    case 61: {                                          /* getdents64(fd, buf, count) */
        int fd = (int)a;
        struct fd *fe;
        uint8_t *out = (uint8_t *)b;
        size_t cap = (size_t)c, used = 0;
        if (fd < 3 || fd >= MAX_FDS || !g_fds[fd].used) return -HBXL_EBADF;
        fe = &g_fds[fd];
        if (!fe->is_dir) return -20;                    /* ENOTDIR */
        lock();
        if (!fe->dir) {
            uint32_t h = (uint32_t)fe->handle, got = 0;
            uint8_t *tmp = hbxl_mmap(1u << 20);
            if (!tmp) { unlock(); return -HBXL_ENOMEM; }
            r = transact(OP_LISTDIR, &h, 4, 0, 0, tmp, 1u << 20, &got);
            if (r < 0) { hbxl_munmap(tmp, 1u << 20); unlock(); return r; }
            fe->dir = tmp;
            fe->dir_len = got;
            fe->dir_pos = 0;
        }
        /* Payload entries: u8 type, u16 length, name bytes. */
        while (fe->dir_pos + 3 <= fe->dir_len) {
            uint8_t type = fe->dir[fe->dir_pos];
            uint16_t len = (uint16_t)(fe->dir[fe->dir_pos + 1] | (fe->dir[fe->dir_pos + 2] << 8));
            size_t reclen = (19 + len + 1 + 7) & ~(size_t)7;
            uint64_t ino = fe->dir_pos + 1;
            int64_t off = (int64_t)fe->dir_pos + 3 + len;
            if (used + reclen > cap) break;
            hbxl_memcpy(out + used, &ino, 8);
            hbxl_memcpy(out + used + 8, &off, 8);
            out[used + 16] = (uint8_t)reclen;
            out[used + 17] = (uint8_t)(reclen >> 8);
            out[used + 18] = type;
            hbxl_memcpy(out + used + 19, fe->dir + fe->dir_pos + 3, len);
            hbxl_memset(out + used + 19 + len, 0, reclen - 19 - len);
            used += reclen;
            fe->dir_pos += 3u + len;
        }
        unlock();
        if (!used && fe->dir_pos < fe->dir_len) return -HBXL_EINVAL;
        return (long)used;
    }
    case 34: case 35: {                                 /* mkdirat / unlinkat */
        r = join_path((int)a, (const char *)b, path);
        if (r) return r;
        lock();
        r = transact(n == 34 ? OP_MKDIR : OP_UNLINK, path, (uint32_t)hbxl_strlen(path) + 1, 0, 0, 0, 0, 0);
        unlock();
        return r;
    }
    case 38: case 276: {                                /* renameat(2) */
        char other[PATH_MAX_K];
        size_t k1, k2;
        r = join_path((int)a, (const char *)b, path);
        if (r) return r;
        r = join_path((int)c, (const char *)d, other);
        if (r) return r;
        k1 = hbxl_strlen(path) + 1;
        k2 = hbxl_strlen(other) + 1;
        lock();
        r = transact(OP_RENAME, path, (uint32_t)k1, other, (uint32_t)k2, 0, 0, 0);
        unlock();
        return r;
    }
    case 46: {                                          /* ftruncate */
        uint32_t req[3];
        if (a < 3 || a >= MAX_FDS || !g_fds[a].used) return -HBXL_EBADF;
        req[0] = (uint32_t)g_fds[a].handle;
        hbxl_memcpy(&req[1], &b, 8);
        lock();
        r = transact(OP_TRUNCATE, req, 12, 0, 0, 0, 0, 0);
        unlock();
        return r;
    }
    case 82: case 83: return 0;                         /* fsync / fdatasync */
    case 25:                                            /* fcntl */
        if (b == 1 || b == 2) return 0;                 /* F_GETFD/F_SETFD */
        if (b == 3) return (a >= 3 && a < MAX_FDS && g_fds[a].used) ? g_fds[a].flags : 2;
        if (b == 4) return 0;                           /* F_SETFL */
        if (b == 0 || b == 1030) {                      /* F_DUPFD(_CLOEXEC) */
            int fd;
            if (a < 3) return (long)a;
            lock();
            fd = alloc_fd();
            if (fd >= 0) { g_fds[fd] = g_fds[a]; g_fds[a].refs++; g_fds[fd].refs = g_fds[a].refs; }
            unlock();
            return fd;
        }
        return -HBXL_EINVAL;
    case 23: case 24:                                   /* dup / dup3 */
        if (a < 3) return n == 23 ? a : b;
        return hbxl_vfs_syscall(25, a, 0, 0, 0, 0, 0, handled);
    case 17:                                            /* getcwd */
        if (c < 2) return -34;
        ((char *)a)[0] = '/'; ((char *)a)[1] = 0;
        return 2;
    case 49: return 0;                                  /* chdir */
    case 78: {                                          /* readlinkat */
        static const char exe[] = "/hbxl-app";
        if (b && !__builtin_strcmp((const char *)b, "/proc/self/exe")) {
            size_t k = sizeof exe - 1 < (size_t)d ? sizeof exe - 1 : (size_t)d;
            hbxl_memcpy((void *)c, exe, k);
            return (long)k;
        }
        return -HBXL_EINVAL;
    }
    default:
        *handled = 0;
        return 0;
    }
}
