/*
 * hbxl.h - HBLauncher Linux personality (internal).
 *
 * Lets an ordinary static aarch64-linux-musl program run as an HBLauncher
 * app.  The toolchain's musl routes every syscall through __hbx_syscall (see
 * sdk/linux/musl-hbx-syscall-hooks.diff); this runtime implements the ones a
 * game port needs on top of the console SDK and Horizon SVCs:
 *
 *   memory   mmap/munmap from a page allocator over Vulkan host-cached memory
 *            (falls back to the SDK heap)
 *   threads  clone/exit/futex via nn::os threads pinned to cores and the
 *            WaitForAddress/SignalToAddress SVCs
 *   files    openat/read/pread/lseek/fstat/getdents over TCP to the PC
 *            (tools/hbxl-fileserver.py); stdout/stderr stream there too
 *   time     clock_gettime/nanosleep from the system tick and SleepThread
 */
#ifndef HBXL_H
#define HBXL_H

#include <stdint.h>
#include <stddef.h>

struct hbx_ctx;
extern const struct hbx_ctx *hbxl_ctx;

/* SDK imports, resolved from the module base at startup. */
enum {
    IMP_MEMALIGN, IMP_FREE, IMP_OS_CREATE, IMP_OS_START, IMP_OS_WAIT, IMP_OS_DESTROY,
    IMP_OS_CURRENT, IMP_OS_PRIORITY, IMP_OS_CORE, IMP_OS_COREMASK, IMP_SOCKET,
    IMP_CONNECT, IMP_RECV, IMP_SEND, IMP_CLOSE, IMP_ERRNO, IMP_SETSOCKOPT,
    IMP_SET_EXCEPTION_HANDLER, IMP_DEFAULT_EXCEPTION_HANDLER, IMP_COUNT
};
extern uint64_t hbxl_imp[IMP_COUNT];
#define HBXL_CALL(index, type) ((type)(uintptr_t)hbxl_imp[index])

/* Index of vkGetInstanceProcAddr in struct hbx_vk_dispatch (checked in memory.c). */
#define HBXL_DISPATCH_GIPA 2

/* Horizon SVCs (raw; caller-saved registers declared clobbered). */
uint64_t hbxl_svc_tick(void);
void hbxl_svc_sleep(int64_t ns);
int32_t hbxl_svc_wait_for_address(volatile int32_t *addr, int32_t type, int32_t value, int64_t ns);
int32_t hbxl_svc_signal_to_address(volatile int32_t *addr, int32_t type, int32_t value, int32_t count);

/* Logging that never enters libc (safe inside syscalls). */
void hbxl_log(const char *text);
void hbxl_logf(const char *format, ...);
void hbxl_ring_logf(const char *format, ...);

/* Memory */
int hbxl_mem_init(void);
void *hbxl_mmap(size_t length);
int hbxl_munmap(void *address, size_t length);
void hbxl_mem_release_all(void);
uint64_t hbxl_mem_used(void);

/* Threads */
void hbxl_threads_init(void);
long hbxl_futex(volatile int32_t *addr, int op, int32_t val, const void *timeout,
                volatile int32_t *addr2, int32_t val3);
long hbxl_gettid(void);
void hbxl_thread_exit(int code) __attribute__((noreturn));
int hbxl_threads_live(void);
void hbxl_threads_reap(int wait);

/* Files over TCP */
int hbxl_vfs_init(void);
long hbxl_vfs_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled);
void hbxl_vfs_write_console(int fd, const void *data, size_t length);
void hbxl_vfs_shutdown(void);
void hbxl_vfs_emergency(void);
void hbxl_vfs_set_preload(const char *path);
int64_t hbxl_vfs_time_offset_ns(void);

/* Exit back to the host (from the main thread). */
void hbxl_return_to_host(long code) __attribute__((noreturn));
extern volatile int hbxl_exiting;
int hbxl_stop_requested(void);
void hbxl_on_exit(void (*fn)(void));

/* Small helpers */
void hbxl_memset(void *d, int c, size_t n);
void hbxl_memcpy(void *d, const void *s, size_t n);
size_t hbxl_strlen(const char *s);

#define HBXL_ENOSYS 38
#define HBXL_ENOMEM 12
#define HBXL_EINVAL 22
#define HBXL_EAGAIN 11
#define HBXL_ETIMEDOUT 110
#define HBXL_EBADF 9
#define HBXL_ENOENT 2
#define HBXL_EIO 5
#define HBXL_EINTR 4

#endif
