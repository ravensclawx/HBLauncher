/*
 * app_preview.c - run an SDK app's real entry point on the workstation and
 * capture its last frame through the shipping renderer.
 *
 * The app source is compiled into this harness unchanged; only the context
 * (heap, atlas, input sources, renderer) is provided here.  `present` is
 * wrapped so the harness can stop the app after a fixed number of frames.
 *
 * Usage: app-preview <out.ppm> [frames]    (run from the repository root)
 *
 * Optional environment, for apps with menus and content:
 *   PREVIEW_SCRIPT  "frame:BUTTONS;frame:BUTTONS..."  pad buttons held from that
 *                   presented frame on (BUTTONS: names joined by '+', empty =
 *                   release), e.g. "30:A;32:;200:L+R;202:"
 *   PREVIEW_SHOTS   "frame:path,frame:path"  extra captures
 *   PREVIEW_ASSETS  "dir:.ext"  package every file in dir with that suffix
 *   PREVIEW_SAVE    path of a save block file (loaded, written back if dirty)
 *   PREVIEW_HEAP_MB app heap size (default 8)
 */
#include <dirent.h>
#include <dlfcn.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hbx_app.h"
#include "hbx_sdk.h"
#include "hbx_ui_internal.h"
#include "hbx_vk.h"

void *hbx_host_vulkan_open(void);
void hbx_host_vulkan_fill(struct hbx_vk_dispatch *d, VkInstance instance, VkDevice device);

int hbx_app_main(const struct hbx_ctx *ctx);

#define WIDTH 1920u
#define HEIGHT 1080u

static unsigned char arena[64u * 1024u * 1024u];
static unsigned char save_block[8u * 1024u * 1024u];
static unsigned char atlas_pixels[1024u * 1024u];
static unsigned char log_ring[0x8000];
static struct hbx_ctx ctx;
static struct hbx_ui_impl impl;
static struct hbx_render render;
static struct hbx_ui_ops wrapped_ops;
static uint32_t frames_presented;
static uint32_t frame_budget = 12;

static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView view;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *mapped;
    VkCommandPool pool;
    struct hbx_vk_dispatch dispatch;
} gpu;

static void die(const char *message, VkResult r) {
    fprintf(stderr, "%s (VkResult %d)\n", message, (int)r);
    exit(1);
}

static VkResult memory_type(uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *out) {
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t i;
    gpu.dispatch.vkGetPhysicalDeviceMemoryProperties(gpu.physical, &properties);
    for (i = 0; i < properties.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            *out = i;
            return VK_SUCCESS;
        }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void gpu_open(void) {
    VkApplicationInfo app;
    VkInstanceCreateInfo instance_info;
    PFN_vkCreateInstance create;
    VkPhysicalDeviceDynamicRenderingFeatures dynamic;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    VkQueueFamilyProperties families[8];
    uint32_t family_count = 8, i, type = 0;
    float priority = 1.0f;
    VkImageCreateInfo image_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;
    VkImageViewCreateInfo view_info;
    VkBufferCreateInfo buffer_info;
    VkCommandPoolCreateInfo pool_info;
    VkResult r;

    if (!hbx_host_vulkan_open()) exit(1);
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "hblauncher-app-preview";
    app.apiVersion = VK_API_VERSION_1_3;
    memset(&instance_info, 0, sizeof instance_info);
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    create = (PFN_vkCreateInstance)dlsym(
        dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL), "vkCreateInstance");
    if (!create) { fprintf(stderr, "no vkCreateInstance\n"); exit(1); }
    r = create(&instance_info, NULL, &gpu.instance);
    if (r != VK_SUCCESS) die("vkCreateInstance", r);
    hbx_host_vulkan_fill(&gpu.dispatch, gpu.instance, VK_NULL_HANDLE);
    {
        uint32_t count = 1;
        gpu.dispatch.vkEnumeratePhysicalDevices(gpu.instance, &count, &gpu.physical);
        if (!gpu.physical) { fprintf(stderr, "no device\n"); exit(1); }
    }
    gpu.dispatch.vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &family_count, families);
    for (i = 0; i < family_count; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gpu.family = i; break; }
    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic.dynamicRendering = VK_TRUE;
    memset(&queue_info, 0, sizeof queue_info);
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = gpu.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    memset(&device_info, 0, sizeof device_info);
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &dynamic;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    r = gpu.dispatch.vkCreateDevice(gpu.physical, &device_info, NULL, &gpu.device);
    if (r != VK_SUCCESS) die("vkCreateDevice", r);
    hbx_host_vulkan_fill(&gpu.dispatch, gpu.instance, gpu.device);
    gpu.dispatch.vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);

    memset(&image_info, 0, sizeof image_info);
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = WIDTH;
    image_info.extent.height = HEIGHT;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    r = gpu.dispatch.vkCreateImage(gpu.device, &image_info, NULL, &gpu.image);
    if (r != VK_SUCCESS) die("vkCreateImage", r);
    gpu.dispatch.vkGetImageMemoryRequirements(gpu.device, gpu.image, &requirements);
    if (memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    &type) != VK_SUCCESS) die("image memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    memset(&allocation, 0, sizeof allocation);
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    r = gpu.dispatch.vkAllocateMemory(gpu.device, &allocation, NULL, &gpu.image_memory);
    if (r != VK_SUCCESS) die("image memory", r);
    gpu.dispatch.vkBindImageMemory(gpu.device, gpu.image, gpu.image_memory, 0);

    memset(&view_info, 0, sizeof view_info);
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = gpu.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    r = gpu.dispatch.vkCreateImageView(gpu.device, &view_info, NULL, &gpu.view);
    if (r != VK_SUCCESS) die("vkCreateImageView", r);

    memset(&buffer_info, 0, sizeof buffer_info);
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = (VkDeviceSize)WIDTH * HEIGHT * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    r = gpu.dispatch.vkCreateBuffer(gpu.device, &buffer_info, NULL, &gpu.readback);
    if (r != VK_SUCCESS) die("readback buffer", r);
    gpu.dispatch.vkGetBufferMemoryRequirements(gpu.device, gpu.readback, &requirements);
    if (memory_type(requirements.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &type) != VK_SUCCESS) die("readback memory type", VK_ERROR_MEMORY_MAP_FAILED);
    memset(&allocation, 0, sizeof allocation);
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    r = gpu.dispatch.vkAllocateMemory(gpu.device, &allocation, NULL, &gpu.readback_memory);
    if (r != VK_SUCCESS) die("readback memory", r);
    gpu.dispatch.vkBindBufferMemory(gpu.device, gpu.readback, gpu.readback_memory, 0);
    gpu.dispatch.vkMapMemory(gpu.device, gpu.readback_memory, 0, VK_WHOLE_SIZE, 0, &gpu.mapped);

    memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = gpu.family;
    r = gpu.dispatch.vkCreateCommandPool(gpu.device, &pool_info, NULL, &gpu.pool);
    if (r != VK_SUCCESS) die("vkCreateCommandPool", r);
}

/* --- capture ------------------------------------------------------- */

static void write_ppm(const char *path) {
    VkCommandBufferAllocateInfo allocate_info;
    VkCommandBuffer command;
    VkCommandBufferBeginInfo begin;
    VkBufferImageCopy copy;
    VkSubmitInfo submit;
    const unsigned char *rgba = (const unsigned char *)gpu.mapped;
    FILE *file;
    uint32_t x, y;

    memset(&allocate_info, 0, sizeof allocate_info);
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = gpu.pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    gpu.dispatch.vkAllocateCommandBuffers(gpu.device, &allocate_info, &command);
    memset(&begin, 0, sizeof begin);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    gpu.dispatch.vkBeginCommandBuffer(command, &begin);
    memset(&copy, 0, sizeof copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = WIDTH;
    copy.imageExtent.height = HEIGHT;
    copy.imageExtent.depth = 1;
    gpu.dispatch.vkCmdCopyImageToBuffer(command, gpu.image,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        gpu.readback, 1, &copy);
    gpu.dispatch.vkEndCommandBuffer(command);
    memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    gpu.dispatch.vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE);
    gpu.dispatch.vkQueueWaitIdle(gpu.queue);

    file = fopen(path, "wb");
    if (!file) { perror(path); exit(1); }
    fprintf(file, "P6\n%u %u\n255\n", WIDTH, HEIGHT);
    for (y = 0; y < HEIGHT; y++)
        for (x = 0; x < WIDTH; x++) {
            const unsigned char *pixel = rgba + ((size_t)y * WIDTH + x) * 4;
            fputc(pixel[0], file); fputc(pixel[1], file); fputc(pixel[2], file);
        }
    fclose(file);
}

/* --- wrapped present: renders one frame, then stops after the budget ---- */

static void write_ppm(const char *path);

static uint32_t script_buttons;

static uint32_t button_bit(const char *name, size_t n) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"A", HBX_A}, {"B", HBX_B}, {"X", HBX_X}, {"Y", HBX_Y}, {"L", HBX_L}, {"R", HBX_R},
        {"ZL", HBX_ZL}, {"ZR", HBX_ZR}, {"PLUS", HBX_PLUS}, {"MINUS", HBX_MINUS},
        {"LEFT", HBX_LEFT}, {"UP", HBX_UP}, {"RIGHT", HBX_RIGHT}, {"DOWN", HBX_DOWN}};
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i].name) == n && strncmp(names[i].name, name, n) == 0) return names[i].bit;
    fprintf(stderr, "PREVIEW_SCRIPT: unknown button %.*s\n", (int)n, name);
    exit(2);
}

/* Apply the script entry for presented frame `frame`, and take any capture. */
static void preview_events(uint32_t frame) {
    const char *script = getenv("PREVIEW_SCRIPT");
    const char *shots = getenv("PREVIEW_SHOTS");
    while (script && *script) {
        char *end;
        unsigned long at = strtoul(script, &end, 10);
        const char *spec = *end == ':' ? end + 1 : end;
        const char *stop = strchr(spec, ';');
        if (!stop) stop = spec + strlen(spec);
        if (at == frame) {
            uint32_t bits = 0;
            const char *p = spec;
            while (p < stop) {
                const char *q = p;
                while (q < stop && *q != '+') q++;
                if (q > p) bits |= button_bit(p, (size_t)(q - p));
                p = q < stop ? q + 1 : q;
            }
            script_buttons = bits;
        }
        script = *stop ? stop + 1 : stop;
    }
    while (shots && *shots) {
        char *end;
        unsigned long at = strtoul(shots, &end, 10);
        const char *path = *end == ':' ? end + 1 : end;
        const char *stop = strchr(path, ',');
        char file[512];
        size_t n;
        if (!stop) stop = path + strlen(path);
        n = (size_t)(stop - path) < sizeof file - 1 ? (size_t)(stop - path) : sizeof file - 1;
        if (at == frame) {
            memcpy(file, path, n);
            file[n] = 0;
            write_ppm(file);
            printf("captured frame %u -> %s\n", frame, file);
        }
        shots = *stop ? stop + 1 : stop;
    }
}

static int wrapped_present(void *self) {
    struct hbx_ui_impl *ui = (struct hbx_ui_impl *)self;
    VkResult r = hbx_ui_render_offscreen(ui, gpu.image, gpu.view, VK_FORMAT_R8G8B8A8_UNORM);
    if (r != VK_SUCCESS) return HBX_ERR_GPU;
    frames_presented++;
    preview_events(frames_presented);
    if (frames_presented >= frame_budget)
        ((struct hbx_ctx *)&ctx)->exit = HBX_EXIT_REQUEST;
    return HBX_OK;
}

/* --- input mocks ---------------------------------------------------- */

static struct hbx_pad_state pad_scratch;
static struct hbx_touch_state touch_scratch;

static void mock_pad(struct hbx_pad_state *state, const uint32_t *id) {
    memset(state, 0, sizeof *state);
    state->buttons = script_buttons;
    state->connected = 1;
    state->reserved0 = *id;
}

static void put32(unsigned char *p, unsigned int value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void mock_touch(struct hbx_touch_state *state) {
    unsigned char *bytes = state->raw;
    memset(state, 0, sizeof *state);
    put32(bytes + 0x10, 1);          /* count (nn::hid layout) */
    put32(bytes + 0x18 + 4, 1);      /* finger id */
    put32(bytes + 0x18 + 8, 420);    /* x */
    put32(bytes + 0x18 + 12, 300);   /* y */
}

/* PREVIEW_ASSETS="dir:.ext": an hbx_asset_table at data_base, files after it. */
static void load_assets(void) {
    const char *spec = getenv("PREVIEW_ASSETS");
    char dir[256];
    const char *colon;
    struct hbx_asset_table *table;
    unsigned char *base = (unsigned char *)(uintptr_t)ctx.data_base;
    size_t cursor = (sizeof(struct hbx_asset_table) + 15u) & ~(size_t)15u;
    char names[HBX_MAX_ASSETS][256];
    int count = 0, i, j;
    DIR *d;
    struct dirent *e;
    if (!spec || !(colon = strchr(spec, ':'))) return;
    snprintf(dir, sizeof dir, "%.*s", (int)(colon - spec), spec);
    d = opendir(dir);
    if (!d) return;
    while ((e = readdir(d)) && count < (int)HBX_MAX_ASSETS) {
        size_t n = strlen(e->d_name), m = strlen(colon + 1);
        if (n > m && strcasecmp(e->d_name + n - m, colon + 1) == 0)
            snprintf(names[count++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    for (i = 0; i < count; i++)        /* sorted, like the host */
        for (j = i + 1; j < count; j++)
            if (strcasecmp(names[j], names[i]) < 0) {
                char t[256];
                memcpy(t, names[i], sizeof t); memcpy(names[i], names[j], sizeof t);
                memcpy(names[j], t, sizeof t);
            }
    table = (struct hbx_asset_table *)base;
    memset(table, 0, sizeof *table);
    table->magic = HBX_ASSET_MAGIC;
    for (i = 0; i < count; i++) {
        char path[600];
        FILE *f;
        long size;
        snprintf(path, sizeof path, "%.255s/%.255s", dir, names[i]);
        f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
        table->entries[table->count].name_off = (uint32_t)cursor;
        memcpy(base + cursor, names[i], strlen(names[i]) + 1);
        cursor = (cursor + strlen(names[i]) + 16u) & ~(size_t)15u;
        table->entries[table->count].data_off = (uint32_t)cursor;
        table->entries[table->count].size = (uint32_t)size;
        if (fread(base + cursor, 1, (size_t)size, f) != (size_t)size) size = 0;
        cursor = (cursor + (size_t)size + 15u) & ~(size_t)15u;
        fclose(f);
        table->count++;
    }
    ctx.assets = ctx.data_base;
    printf("preview: %u asset(s) from %s\n", table->count, dir);
}

static void load_save(void) {
    const char *path = getenv("PREVIEW_SAVE");
    FILE *f;
    if (!path) return;
    memset(save_block, 0, 16);
    f = fopen(path, "rb");
    if (f) { size_t n = fread(save_block, 1, sizeof save_block, f); (void)n; fclose(f); }
    ctx.args[6] = (uint64_t)(uintptr_t)save_block;
    ctx.args[7] = sizeof save_block;
}

static void store_save(void) {
    const char *path = getenv("PREVIEW_SAVE");
    const struct hbx_save_header *h = (const struct hbx_save_header *)save_block;
    FILE *f;
    if (!path || h->magic != HBX_SAVE_MAGIC || !h->dirty || h->bytes > sizeof save_block) return;
    ((struct hbx_save_header *)save_block)->dirty = 0;
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(save_block, 1, h->bytes, f);
    fclose(f);
    printf("preview: save block written (%u bytes)\n", h->bytes);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "tests/preview/out/app.ppm";
    FILE *file;
    int status;

    if (argc > 2) frame_budget = (uint32_t)atoi(argv[2]);
    if (frame_budget == 0) frame_budget = 1;

    file = fopen("assets/font_atlas.r8", "rb");
    if (!file || fread(atlas_pixels, 1, sizeof atlas_pixels, file) != sizeof atlas_pixels) {
        fprintf(stderr, "cannot read assets/font_atlas.r8\n");
        return 1;
    }
    fclose(file);
    gpu_open();

    if (hbx_ui_init(&impl, &gpu.dispatch, gpu.instance, gpu.physical, gpu.device,
                    gpu.queue, gpu.family, WIDTH, HEIGHT, VK_FORMAT_R8G8B8A8_UNORM,
                    atlas_pixels, sizeof atlas_pixels, 1) != VK_SUCCESS) {
        fprintf(stderr, "renderer init failed\n");
        return 1;
    }
    hbx_ui_publish(&impl, &render);
    wrapped_ops = *render.ops;
    wrapped_ops.present = wrapped_present;
    render.ops = &wrapped_ops;

    memset(arena, 0, sizeof arena);
    memset(&ctx, 0, sizeof ctx);
    memset(log_ring, 0, sizeof log_ring);
    ctx.magic = HBX_ABI_MAGIC;
    ctx.abi_version = HBX_ABI_VERSION;
    ctx.size = sizeof ctx;
    {
        const char *mb = getenv("PREVIEW_HEAP_MB");
        uint64_t heap = (uint64_t)(mb ? atoi(mb) : 8) * 1024u * 1024u;
        if (heap < 1024u * 1024u || heap > 40u * 1024u * 1024u) heap = 8u * 1024u * 1024u;
        ctx.heap_base = (uint64_t)(uintptr_t)arena;
        ctx.heap_size = heap;
        ctx.data_base = (uint64_t)(uintptr_t)(arena + heap);
        ctx.data_size = sizeof arena - heap;
    }
    load_assets();
    load_save();
    ctx.log = (uint64_t)(uintptr_t)log_ring;
    ctx.ui = (uint64_t)(uintptr_t)&render;
    ctx.atlas = (uint64_t)(uintptr_t)atlas_pixels;
    ctx.atlas_bytes = sizeof atlas_pixels;
    ctx.input.pad_handheld = (uint64_t)(uintptr_t)mock_pad;
    ctx.input.touch = (uint64_t)(uintptr_t)mock_touch;
    ctx.input.pad_state = (uint64_t)(uintptr_t)&pad_scratch;
    ctx.input.touch_state = (uint64_t)(uintptr_t)&touch_scratch;
    ctx.gpu.device = (uint64_t)(uintptr_t)gpu.device;
    ctx.gpu.queue = (uint64_t)(uintptr_t)gpu.queue;
    ctx.gpu.width = WIDTH;
    ctx.gpu.height = HEIGHT;
    ctx.gpu.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    ctx.gpu.dispatch = (uint64_t)(uintptr_t)&gpu.dispatch;
    {
        struct hbx_log *ring = (struct hbx_log *)(uintptr_t)log_ring;
        ring->ring = (uint64_t)(uintptr_t)(log_ring + 16);
        ring->bytes = sizeof log_ring - 16;
    }

    status = hbx_app_main(&ctx);
    store_save();
    printf("app returned %d after %u presented frames\n", status, frames_presented);
    printf("log: %s\n", log_ring + 16);
    write_ppm(out);
    printf("wrote %s\n", out);
    return status == 0 ? 0 : 1;
}
