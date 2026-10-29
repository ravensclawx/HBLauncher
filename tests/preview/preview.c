/*
 * preview.c - render the launcher screen with real Vulkan on the workstation.
 *
 * This links the *same* geometry, layout, atlas and shaders as the console
 * build and renders into an offscreen image (lavapipe or any ICD), then writes
 * a PPM per frame.  It is the offline visual regression harness; it is not
 * part of the console image.
 *
 * Usage: preview <output-dir> [frames]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hbx_ui_internal.h"
#include "launcher_ui.h"

void *hbx_host_vulkan_open(void);
void hbx_host_vulkan_fill(struct hbx_vk_dispatch *d, VkInstance instance, VkDevice device);

static const uint32_t kWidth = 1920, kHeight = 1080;

static void die(const char *message, VkResult r) {
    fprintf(stderr, "%s (VkResult %d)\n", message, (int)r);
    exit(1);
}

static void write_ppm(const char *path, const unsigned char *rgba) {
    FILE *file = fopen(path, "wb");
    uint32_t y, x;
    if (!file) { perror(path); exit(1); }
    fprintf(file, "P6\n%u %u\n255\n", kWidth, kHeight);
    for (y = 0; y < kHeight; y++) {
        for (x = 0; x < kWidth; x++) {
            const unsigned char *pixel = rgba + ((size_t)y * kWidth + x) * 4;
            fputc(pixel[0], file);
            fputc(pixel[1], file);
            fputc(pixel[2], file);
        }
    }
    fclose(file);
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : "tests/preview/out";
    int frames = argc > 2 ? atoi(argv[2]) : 1;
    struct hbx_vk_dispatch dispatch;
    VkApplicationInfo app;
    VkInstanceCreateInfo instance_info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceDynamicRenderingFeatures dynamic;
    VkDeviceCreateInfo device_info;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkQueueFamilyProperties families[8];
    uint32_t family_count = 8, family = 0xffffffffu, i;
    VkImageCreateInfo image_info;
    VkImage image;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;
    VkDeviceMemory memory;
    VkImageViewCreateInfo view_info;
    VkImageView view;
    VkBufferCreateInfo buffer_info;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *mapped = NULL;
    VkCommandPoolCreateInfo pool_info;
    VkCommandPool pool;
    VkCommandBufferAllocateInfo command_alloc;
    VkCommandBuffer command;
    VkCommandBufferBeginInfo begin;
    VkBufferImageCopy copy;
    VkSubmitInfo submit;
    VkResult r;
    struct hbx_ui_impl *impl;
    struct launcher_state *state;
    struct hbx_catalog *catalog;
    char *strings;
    int frame;
    uint32_t memory_type = 0xffffffffu;

    if (!hbx_host_vulkan_open()) return 1;

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "hblauncher-preview";
    app.apiVersion = VK_API_VERSION_1_3;
    memset(&instance_info, 0, sizeof instance_info);
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    {
        PFN_vkCreateInstance create = (PFN_vkCreateInstance)dlsym(
            dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL), "vkCreateInstance");
        if (!create) { fprintf(stderr, "no vkCreateInstance\n"); return 1; }
        r = create(&instance_info, NULL, &instance);
        if (r != VK_SUCCESS) die("vkCreateInstance", r);
    }
    hbx_host_vulkan_fill(&dispatch, instance, VK_NULL_HANDLE);
    {
        uint32_t count = 1;
        dispatch.vkEnumeratePhysicalDevices(instance, &count, &physical);
        if (!physical) { fprintf(stderr, "no physical device\n"); return 1; }
        dispatch.vkGetPhysicalDeviceProperties(physical, &properties);
        printf("device: %s (Vulkan %u.%u.%u)\n", properties.deviceName,
               VK_VERSION_MAJOR(properties.apiVersion),
               VK_VERSION_MINOR(properties.apiVersion),
               VK_VERSION_PATCH(properties.apiVersion));
    }
    dispatch.vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    for (i = 0; i < family_count; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
    if (family == 0xffffffffu) { fprintf(stderr, "no graphics queue\n"); return 1; }

    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic.dynamicRendering = VK_TRUE;
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info;
        memset(&queue_info, 0, sizeof queue_info);
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        memset(&device_info, 0, sizeof device_info);
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &dynamic;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        r = dispatch.vkCreateDevice(physical, &device_info, NULL, &device);
        if (r != VK_SUCCESS) die("vkCreateDevice", r);
    }
    hbx_host_vulkan_fill(&dispatch, instance, device);
    dispatch.vkGetDeviceQueue(device, family, 0, &queue);

    /* Offscreen colour target. */
    memset(&image_info, 0, sizeof image_info);
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = kWidth;
    image_info.extent.height = kHeight;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    r = dispatch.vkCreateImage(device, &image_info, NULL, &image);
    if (r != VK_SUCCESS) die("vkCreateImage", r);
    dispatch.vkGetImageMemoryRequirements(device, image, &requirements);
    {
        VkPhysicalDeviceMemoryProperties memory_properties;
        dispatch.vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        for (i = 0; i < memory_properties.memoryTypeCount; i++)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memory_type = i;
                break;
            }
        if (memory_type == 0xffffffffu) { fprintf(stderr, "no device memory\n"); return 1; }
    }
    memset(&allocation, 0, sizeof allocation);
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    r = dispatch.vkAllocateMemory(device, &allocation, NULL, &memory);
    if (r != VK_SUCCESS) die("vkAllocateMemory", r);
    dispatch.vkBindImageMemory(device, image, memory, 0);

    memset(&view_info, 0, sizeof view_info);
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    r = dispatch.vkCreateImageView(device, &view_info, NULL, &view);
    if (r != VK_SUCCESS) die("vkCreateImageView", r);

    /* Readback buffer. */
    memset(&buffer_info, 0, sizeof buffer_info);
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = (VkDeviceSize)kWidth * kHeight * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    r = dispatch.vkCreateBuffer(device, &buffer_info, NULL, &readback);
    if (r != VK_SUCCESS) die("vkCreateBuffer", r);
    dispatch.vkGetBufferMemoryRequirements(device, readback, &requirements);
    {
        VkPhysicalDeviceMemoryProperties memory_properties;
        dispatch.vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        for (i = 0; i < memory_properties.memoryTypeCount; i++)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                memory_type = i;
                break;
            }
    }
    memset(&allocation, 0, sizeof allocation);
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    r = dispatch.vkAllocateMemory(device, &allocation, NULL, &readback_memory);
    if (r != VK_SUCCESS) die("readback memory", r);
    dispatch.vkBindBufferMemory(device, readback, readback_memory, 0);
    dispatch.vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);

    memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = family;
    r = dispatch.vkCreateCommandPool(device, &pool_info, NULL, &pool);
    if (r != VK_SUCCESS) die("vkCreateCommandPool", r);

    impl = calloc(1, sizeof *impl);
    {
        static unsigned char atlas[HBX_ATLAS_SIZE * HBX_ATLAS_SIZE];
        FILE *file = fopen(argv[1] ? "assets/font_atlas.r8" : "assets/font_atlas.r8", "rb");
        if (!file || fread(atlas, 1, sizeof atlas, file) != sizeof atlas) {
            fprintf(stderr, "cannot read assets/font_atlas.r8 (run tools/make_font_atlas.py)\n");
            return 1;
        }
        fclose(file);
        r = hbx_ui_init(impl, &dispatch, instance, physical, device, queue, family,
                        kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM, atlas, sizeof atlas, 4);
    }
    if (r != VK_SUCCESS) die("hbx_ui_init", r);
    printf("renderer ready: %u vertices max, atlas %ux%u\n", impl->vertex_capacity,
           HBX_ATLAS_SIZE, HBX_ATLAS_SIZE);

    /* Synthetic catalog for offline UI coverage. */
    {
        static const char *names[] = {
            "GPU Triangle", "Hello CPU", "Input + Touch",
            "Wave Demo", "Font Sampler", "Icon Lab", "Touch Keys", "Read me"
        };
        static const char *descs[] = {
            "SDK sample - animated triangle drawn on the GPU",
            "SDK sample - CPU logic with Vulkan UI drawing",
            "SDK sample - controller buttons, sticks and touch points",
            "SDK sample - the launcher wave field as an app",
            "SDK sample - atlas faces and text layout",
            "SDK sample - packaged icon layers",
            "SDK sample - on-screen keys and touch",
            "Copy an app package into apps/ to add it here"
        };
        /* The shipped catalog has two titles; extra synthetic entries let
         * the preview exercise the page-dot state offline. */
        uint32_t count = 8, offset = 0, index;
        catalog = calloc(1, sizeof *catalog);
        strings = calloc(1, 1024);
        catalog->magic = HBX_CATALOG_MAGIC;
        catalog->count = count;
        catalog->strings_off = 0;
        for (index = 0; index < count; index++) {
            size_t length = strlen(names[index]) + 1;
            memcpy(strings + offset, names[index], length);
            catalog->entries[index].name_off = offset;
            offset += (uint32_t)length;
            length = strlen(descs[index]) + 1;
            memcpy(strings + offset, descs[index], length);
            catalog->entries[index].desc_off = offset;
            offset += (uint32_t)length;
            catalog->entries[index].kind = index == 3 ? 1u : (index == 4 ? 2u : 0u);
            catalog->entries[index].icon_layer = HBX_ICON_NONE;
            catalog->entries[index].abi = 3;
        }
        catalog->strings_bytes = offset;
    }

    state = calloc(1, sizeof *state);
    state->abi = 1;
    state->catalog = catalog;
    state->strings = strings;
    state->selected = 0;
    state->mode = LAUNCHER_MODE_LIBRARY;
    state->refresh_spin = 0.35f;   /* the still badge angle */

    /* Frame plan (matches the launcher's shipped states):
     *  0 library, three titles, row 0 selected
     *  1 same, a little later (waves animate)
     *  2 selection moved to the last row
     *  3 fade-out while opening the selected title
     *  4 resumed: fade-in plus the "Returned from" toast
     *  5 refresh pending: spinning emblem + "Refreshing..."
     *  6 empty catalog
     *  7 eight titles: page dots
     */
    for (frame = 0; frame < frames; frame++) {
        char path[512];
        float t = (float)frame * 0.75f;
        state->time = t;
        state->fps_milli = 59900u;   /* the footer readout */
        state->count = 2;
        state->selected = 0;
        state->row_highlight[0] = 1.0f;
        state->row_highlight[1] = 0.0f;
        state->row_highlight[2] = 0.0f;
        state->mode = LAUNCHER_MODE_LIBRARY;
        state->transition = 0.0f;
        state->fade = 0.0f;
        state->refreshing = 0;
        state->refresh_spin = 0.35f;
        state->toast_frames = 0;
        if (frame >= 2 && frame <= 4) state->count = 3; /* Synthetic input demo coverage. */
        if (frame == 2) { state->selected = 2; state->row_highlight[0] = 0.0f;
                          state->row_highlight[2] = 1.0f; }
        if (frame == 3) { state->mode = LAUNCHER_MODE_LAUNCH; state->transition = 0.85f;
                          state->fade = 0.85f; state->last_index = 2;
                          state->selected = 2; state->row_highlight[0] = 0.0f;
                          state->row_highlight[2] = 1.0f; }
        if (frame == 4) { state->fade = 0.4f; state->toast_frames = 60;
                          state->selected = 2; state->row_highlight[0] = 0.0f;
                          state->row_highlight[2] = 1.0f;
                          strcpy(state->toast, "Returned from Input + Touch"); }
        if (frame == 5) { state->refreshing = 1; state->refresh_spin = 2.05f; }
        if (frame == 6) { state->count = 0; }
        if (frame == 7) { state->count = 8; state->selected = 4;
                          state->row_highlight[0] = 0.0f;
                          state->row_highlight[1] = 1.0f;
                          state->row_highlight[2] = 0.0f; }

        hbx_ui_engine_begin(impl, 0xfffcf9f9u);
        hbx_launcher_draw(impl, state);
        printf("frame %d: %u vertices%s\n", frame, hbx_ui_engine_vertex_count(impl),
               hbx_ui_engine_overflow(impl) ? " (OVERFLOW)" : "");
        r = hbx_ui_render_offscreen(impl, image, view, VK_FORMAT_R8G8B8A8_UNORM);
        if (r != VK_SUCCESS) die("render", r);

        memset(&command_alloc, 0, sizeof command_alloc);
        command_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_alloc.commandPool = pool;
        command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_alloc.commandBufferCount = 1;
        dispatch.vkAllocateCommandBuffers(device, &command_alloc, &command);
        memset(&begin, 0, sizeof begin);
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dispatch.vkBeginCommandBuffer(command, &begin);
        /* The frame ended in TRANSFER_SRC_OPTIMAL already. */
        memset(&copy, 0, sizeof copy);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = kWidth;
        copy.imageExtent.height = kHeight;
        copy.imageExtent.depth = 1;
        dispatch.vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        readback, 1, &copy);
        dispatch.vkEndCommandBuffer(command);
        memset(&submit, 0, sizeof submit);
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        dispatch.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        dispatch.vkQueueWaitIdle(queue);

        snprintf(path, sizeof path, "%s/frame-%02d.ppm", outdir, frame);
        write_ppm(path, (const unsigned char *)mapped);
        printf("wrote %s\n", path);
    }
    printf("preview complete\n");
    return 0;
}
