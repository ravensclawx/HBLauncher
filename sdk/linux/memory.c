/*
 * memory.c - mmap/munmap for the Linux personality.
 *
 * Pages come from chunks: Vulkan host-visible *cached* memory (the large pool
 * measured at ~2.8 GiB on device) and, as a fallback, the SDK heap
 * (posix_memalign, ~240 MiB).  Uncached memory is never used: exclusive and
 * atomic accesses on it can fault.  Each chunk keeps a sorted list of free
 * page runs; freeing coalesces.  Linux guarantees zeroed anonymous memory, so
 * pages are cleared on allocation.
 */
#include "hbxl.h"
#include "hbx_sdk.h"
#include "hbx_vk.h"

_Static_assert(__builtin_offsetof(struct hbx_vk_dispatch, vkGetInstanceProcAddr) ==
               HBXL_DISPATCH_GIPA * sizeof(void *), "dispatch layout");

#define PAGE 4096u
#define MAX_CHUNKS 96
#define MAX_RUNS 1024
#define VK_CHUNK (256ull << 20)
#define SDK_CHUNK (64ull << 20)

struct run { uint32_t start, pages; };

struct chunk {
    uint8_t *base;
    uint64_t pages;
    uint64_t used;
    int kind;                    /* 1 vulkan, 2 sdk */
    VkDeviceMemory memory;
    uint32_t nruns;
    struct run runs[MAX_RUNS];
};

static struct chunk g_chunks[MAX_CHUNKS];
static int g_nchunks;
static volatile int g_lock;
static int32_t g_vk_type = -1;
static int g_vk_failed;
static uint64_t g_used_pages;

static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) hbxl_svc_sleep(0);
}
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

uint64_t hbxl_mem_used(void) { return g_used_pages * PAGE; }

static int add_chunk_vk(uint64_t bytes) {
    const struct hbx_vk_dispatch *d = (const struct hbx_vk_dispatch *)(uintptr_t)hbxl_ctx->gpu.dispatch;
    VkDevice device = (VkDevice)(uintptr_t)hbxl_ctx->gpu.device;
    VkMemoryAllocateInfo info;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *p = 0;
    struct chunk *c;
    if (g_vk_failed || g_vk_type < 0 || !d || !device || g_nchunks >= MAX_CHUNKS) return -1;
    hbxl_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = bytes;
    info.memoryTypeIndex = (uint32_t)g_vk_type;
    if (d->vkAllocateMemory(device, &info, NULL, &memory) != VK_SUCCESS) { g_vk_failed = 1; return -1; }
    if (d->vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p) {
        d->vkFreeMemory(device, memory, NULL);
        g_vk_failed = 1;
        return -1;
    }
    hbxl_ring_logf("hbxl: vulkan chunk %u MiB at 0x%x", bytes >> 20, (uint64_t)(uintptr_t)p);
    c = &g_chunks[g_nchunks++];
    hbxl_memset(c, 0, sizeof *c - sizeof c->runs);
    c->base = (uint8_t *)(((uintptr_t)p + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    c->pages = (bytes - (uint64_t)(c->base - (uint8_t *)p)) / PAGE;
    c->kind = 1;
    c->memory = memory;
    c->nruns = 1;
    c->runs[0].start = 0;
    c->runs[0].pages = (uint32_t)c->pages;
    return g_nchunks - 1;
}

static int add_chunk_sdk(uint64_t bytes) {
    void *p = 0;
    struct chunk *c;
    if (g_nchunks >= MAX_CHUNKS) return -1;
    if (HBXL_CALL(IMP_MEMALIGN, int (*)(void **, uint64_t, uint64_t))(&p, PAGE, bytes) != 0 || !p)
        return -1;
    hbxl_ring_logf("hbxl: sdk heap chunk %u MiB at 0x%x", bytes >> 20, (uint64_t)(uintptr_t)p);
    c = &g_chunks[g_nchunks++];
    hbxl_memset(c, 0, sizeof *c - sizeof c->runs);
    c->base = (uint8_t *)p;
    c->pages = bytes / PAGE;
    c->kind = 2;
    c->nruns = 1;
    c->runs[0].start = 0;
    c->runs[0].pages = (uint32_t)c->pages;
    return g_nchunks - 1;
}

int hbxl_mem_init(void) {
    const struct hbx_vk_dispatch *d = (const struct hbx_vk_dispatch *)(uintptr_t)hbxl_ctx->gpu.dispatch;
    if (d && d->vkGetPhysicalDeviceMemoryProperties && hbxl_ctx->gpu.physical) {
        VkPhysicalDeviceMemoryProperties props;
        uint32_t i;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        hbxl_memset(&props, 0, sizeof props);
        d->vkGetPhysicalDeviceMemoryProperties((VkPhysicalDevice)(uintptr_t)hbxl_ctx->gpu.physical, &props);
        for (i = 0; i < props.memoryTypeCount && i < 32; i++) {
            if ((props.memoryTypes[i].propertyFlags & want) == want) {
                g_vk_type = (int32_t)i;
                if (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
            }
        }
        hbxl_logf("hbxl: vulkan memory type %d (host cached)", (int64_t)g_vk_type);
    }
    /* Start with the proven SDK heap; Vulkan chunks are added on demand. */
    if (add_chunk_sdk(SDK_CHUNK) < 0 && add_chunk_vk(VK_CHUNK) < 0) return -1;
    return 0;
}

static void *take(struct chunk *c, uint32_t pages) {
    uint32_t i;
    for (i = 0; i < c->nruns; i++) {
        if (c->runs[i].pages >= pages) {
            uint8_t *p = c->base + (uint64_t)c->runs[i].start * PAGE;
            c->runs[i].start += pages;
            c->runs[i].pages -= pages;
            if (!c->runs[i].pages) {
                hbxl_memcpy(&c->runs[i], &c->runs[i + 1], (c->nruns - i - 1) * sizeof(struct run));
                c->nruns--;
            }
            c->used += pages;
            return p;
        }
    }
    return 0;
}

void *hbxl_mmap(size_t length) {
    uint32_t pages = (uint32_t)((length + PAGE - 1) / PAGE);
    void *p = 0;
    int i;
    if (!length) return 0;
    lock();
    for (i = 0; i < g_nchunks && !p; i++) p = take(&g_chunks[i], pages);
    if (!p) {
        uint64_t bytes = (uint64_t)pages * PAGE;
        int k = -1;
        if (bytes <= VK_CHUNK) k = add_chunk_vk(VK_CHUNK);
        else k = add_chunk_vk((bytes + PAGE * 16 + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1));
        if (k < 0) k = add_chunk_sdk(bytes > SDK_CHUNK ? bytes : SDK_CHUNK);
        if (k >= 0) p = take(&g_chunks[k], pages);
    }
    if (p) g_used_pages += pages;
    unlock();
    if (p) __builtin_memset(p, 0, (size_t)pages * PAGE);
    return p;
}

int hbxl_munmap(void *address, size_t length) {
    uint8_t *a = (uint8_t *)address;
    uint32_t pages = (uint32_t)((length + PAGE - 1) / PAGE);
    int i;
    if ((uintptr_t)a & (PAGE - 1)) return -HBXL_EINVAL;
    lock();
    for (i = 0; i < g_nchunks; i++) {
        struct chunk *c = &g_chunks[i];
        uint32_t start, k;
        if (a < c->base || a >= c->base + c->pages * PAGE) continue;
        start = (uint32_t)((uint64_t)(a - c->base) / PAGE);
        if (start + pages > c->pages) pages = (uint32_t)(c->pages - start);
        for (k = 0; k < c->nruns && c->runs[k].start < start; k++) {}
        /* Ignore double frees / overlaps with free runs. */
        if (k > 0 && c->runs[k - 1].start + c->runs[k - 1].pages > start) break;
        if (k < c->nruns && start + pages > c->runs[k].start) break;
        if (k > 0 && c->runs[k - 1].start + c->runs[k - 1].pages == start) {
            c->runs[k - 1].pages += pages;
            if (k < c->nruns && c->runs[k - 1].start + c->runs[k - 1].pages == c->runs[k].start) {
                c->runs[k - 1].pages += c->runs[k].pages;
                hbxl_memcpy(&c->runs[k], &c->runs[k + 1], (c->nruns - k - 1) * sizeof(struct run));
                c->nruns--;
            }
        } else if (k < c->nruns && start + pages == c->runs[k].start) {
            c->runs[k].start = start;
            c->runs[k].pages += pages;
        } else if (c->nruns < MAX_RUNS) {
            uint32_t m;
            for (m = c->nruns; m > k; m--) c->runs[m] = c->runs[m - 1];
            c->runs[k].start = start;
            c->runs[k].pages = pages;
            c->nruns++;
        }                                  /* else: leak the pages rather than corrupt */
        c->used -= pages;
        g_used_pages -= pages;
        break;
    }
    unlock();
    return 0;
}

void hbxl_mem_release_all(void) {
    const struct hbx_vk_dispatch *d = (const struct hbx_vk_dispatch *)(uintptr_t)hbxl_ctx->gpu.dispatch;
    VkDevice device = (VkDevice)(uintptr_t)hbxl_ctx->gpu.device;
    int i;
    for (i = 0; i < g_nchunks; i++) {
        if (g_chunks[i].kind == 1 && d && device) {
            d->vkUnmapMemory(device, g_chunks[i].memory);
            d->vkFreeMemory(device, g_chunks[i].memory, NULL);
        } else if (g_chunks[i].kind == 2) {
            HBXL_CALL(IMP_FREE, void (*)(void *))(g_chunks[i].base);
        }
    }
    g_nchunks = 0;
}
