/*
 * launcher_vk.c - console Vulkan bring-up.
 *
 * The call order and every struct field follow the configuration that is
 * already known to produce a visible frame on this console (sm64-console's
 * sm64_vk.c device init and vi presenter):
 *
 *   instance (VK_KHR_surface + VK_NN_vi_surface, Vulkan 1.4)
 *   -> vkCreateViSurfaceNN on the host-created vi native window
 *   -> physical device + a queue family with graphics and present support
 *   -> dynamic-rendering feature chain
 *   -> device with VK_KHR_swapchain
 *   -> 1920x1080 FIFO swapchain, image views, acquire/render semaphores
 *
 * No Vulkan object is created at compile time; every entrypoint arrives in the
 * host-built dispatch table, so the module base can move between processes.
 */
#include "launcher_vk.h"
#include "hbx_app.h"

#define VK_HD_ERR(status) ((status) ? (status) : -1)

static VkResult create_instance(const struct hbx_ctx *ctx, const struct hbx_vk_dispatch *d,
                                VkInstance *instance) {
    VkApplicationInfo app;
    VkInstanceCreateInfo info;
    const char *extensions[2];
    (void)ctx;

    extensions[0] = HBX_VK_KHR_SURFACE_EXTENSION_NAME;
    extensions[1] = HBX_VK_NN_VI_SURFACE_EXTENSION_NAME;

    /* Do not enumerate instance extensions here: the proven console path
     * enables the two names directly, and calling the module's
     * vkEnumerateInstanceExtensionProperties with a NULL instance was the
     * first thing that killed the process on hardware (2026-10-28).
     * vkCreateInstance reports a missing extension as an error, not a crash. */

    hbx_memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "HBLauncherNative";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app.pEngineName = "hbx";
    app.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);
    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    return d->vkCreateInstance(&info, NULL, instance);
}

static int find_queue_family(const struct hbx_ctx *ctx, const struct hbx_vk_dispatch *d,
                             VkPhysicalDevice physical, VkSurfaceKHR surface,
                             uint32_t *family) {
    VkQueueFamilyProperties families[16];
    uint32_t count = 0, i;
    if (!d->vkGetPhysicalDeviceQueueFamilyProperties) return 0;
    d->vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (count == 0) return 0;
    if (count > 16u) count = 16u;
    hbx_memset(families, 0, sizeof families);
    d->vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (i = 0; i < count && i < 16u; i++) {
        VkBool32 supported = VK_FALSE;
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (d->vkGetPhysicalDeviceSurfaceSupportKHR) {
            if (d->vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported) !=
                VK_SUCCESS)
                continue;
            if (!supported) continue;
        }
        *family = i;
        return 1;
    }
    (void)ctx;
    return 0;
}

static VkResult create_device(const struct hbx_ctx *ctx, const struct hbx_vk_dispatch *d,
                              VkPhysicalDevice physical, uint32_t family, VkDevice *device) {
    VkPhysicalDeviceDynamicRenderingFeatures dynamic;
    VkPhysicalDeviceFeatures2 supported;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo info;
    const char *extensions[1];
    float priority = 1.0f;

    /* Exactly the proven SM64 form: one dynamic-rendering feature, no sync2
     * chain in the query, and a hard failure when the driver does not offer
     * it (instead of forcing the bit and letting vkCreateDevice fail). */
    hbx_memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamic.dynamicRendering = VK_TRUE;
    if (d->vkGetPhysicalDeviceFeatures2) {
        hbx_memset(&supported, 0, sizeof supported);
        supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supported.pNext = &dynamic;
        d->vkGetPhysicalDeviceFeatures2(physical, &supported);
        if (!dynamic.dynamicRendering) return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    extensions[0] = HBX_VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    hbx_memset(&queue_info, 0, sizeof queue_info);
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &dynamic;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    (void)ctx;
    return d->vkCreateDevice(physical, &info, NULL, device);
}

static VkResult create_swapchain(const struct hbx_ctx *ctx, const struct hbx_vk_dispatch *d,
                                 struct launcher_display *display) {
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[16];
    VkSurfaceFormatKHR chosen;
    VkCompositeAlphaFlagBitsKHR composite;
    VkSwapchainCreateInfoKHR info;
    VkImageViewCreateInfo view_info;
    VkSemaphoreCreateInfo semaphore;
    uint32_t count = 16, i, images;
    VkResult r;
    (void)ctx;

    hbx_memset(&caps, 0, sizeof caps);
    r = d->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(display->physical, display->surface, &caps);
    if (r != VK_SUCCESS) return r;
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    /* The surface decides the extent.  When currentExtent is undefined, clamp
     * our preferred size into the supported range instead of assuming it. */
    if (caps.currentExtent.width != UINT32_MAX &&
        caps.currentExtent.height != UINT32_MAX) {
        display->width = caps.currentExtent.width;
        display->height = caps.currentExtent.height;
    } else {
        if (display->width < caps.minImageExtent.width)
            display->width = caps.minImageExtent.width;
        if (display->height < caps.minImageExtent.height)
            display->height = caps.minImageExtent.height;
        if (display->width > caps.maxImageExtent.width)
            display->width = caps.maxImageExtent.width;
        if (display->height > caps.maxImageExtent.height)
            display->height = caps.maxImageExtent.height;
    }

    hbx_memset(formats, 0, sizeof formats);
    count = 0;
    r = d->vkGetPhysicalDeviceSurfaceFormatsKHR(display->physical, display->surface, &count, NULL);
    if (r != VK_SUCCESS) return r;
    if (count == 0) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (count > 16u) count = 16u;
    r = d->vkGetPhysicalDeviceSurfaceFormatsKHR(display->physical, display->surface, &count, formats);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
    hbx_memset(&chosen, 0, sizeof chosen);
    chosen.format = VK_FORMAT_UNDEFINED;
    chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    for (i = 0; i < count; i++) {
        if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
            formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_UNDEFINED) {
            chosen = formats[i];
            if (chosen.format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    display->format = chosen.format;

    images = caps.minImageCount + 1u;
    if (caps.maxImageCount != 0 && images > caps.maxImageCount) images = caps.maxImageCount;
    if (images == 0 || images > HBX_MAX_IMAGES) return VK_ERROR_INITIALIZATION_FAILED;

    composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & composite) == 0) {
        composite = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                  (0u - caps.supportedCompositeAlpha));
        if (composite == 0) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = display->surface;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent.width = display->width;
    info.imageExtent.height = display->height;
    info.imageArrayLayers = 1;
    /* The proven display bring-up requested COLOR_ATTACHMENT|TRANSFER_SRC and
     * that is the mask actually recorded as presenting on this console. */
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = composite;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    r = d->vkCreateSwapchainKHR(display->device, &info, NULL, &display->swapchain);
    if (r != VK_SUCCESS) return r;

    /* Query the real count first and require a complete enumeration: a
     * truncated list could omit an index that acquire later returns. */
    count = 0;
    r = d->vkGetSwapchainImagesKHR(display->device, display->swapchain, &count, NULL);
    if (r != VK_SUCCESS) return r;
    if (count == 0 || count > HBX_MAX_IMAGES) return VK_ERROR_INITIALIZATION_FAILED;
    display->image_count = count;
    r = d->vkGetSwapchainImagesKHR(display->device, display->swapchain, &count, display->images);
    if (r != VK_SUCCESS) return r;
    if (count > display->image_count) return VK_ERROR_INITIALIZATION_FAILED;

    for (i = 0; i < count; i++) {
        hbx_memset(&view_info, 0, sizeof view_info);
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = display->images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = display->format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        r = d->vkCreateImageView(display->device, &view_info, NULL, &display->views[i]);
        if (r != VK_SUCCESS) return r;
    }

    hbx_memset(&semaphore, 0, sizeof semaphore);
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    /* The acquire semaphore is consumed by every submit; the render-finished
     * semaphores are per image and created by the renderer. */
    return d->vkCreateSemaphore(display->device, &semaphore, NULL, &display->acquire);
}

int launcher_display_create(struct launcher_display *display, const struct hbx_ctx *ctx,
                            uint32_t stage_limit) {
    const struct hbx_vk_dispatch *d;
    VkResult r;
    uint32_t icon_layers;

    if (!display || !ctx) return -1;
    d = hbx_vk(ctx);
    if (!d || !ctx->gpu.create_vi_surface || !ctx->gpu.vi_window) {
        display->last_result = VK_ERROR_EXTENSION_NOT_PRESENT;
        return -2;
    }
    /* Resumable: a staged run returns after its stage and the next entry
     * continues from here instead of rebuilding (the handles live in .bss). */
    if (display->stage_done == 0 && !display->initialized) {
        hbx_memset(display, 0, sizeof *display);
        display->width = ctx->gpu.width ? ctx->gpu.width : 1920;
        display->height = ctx->gpu.height ? ctx->gpu.height : 1080;
    }

    /* 1: instance */
    if (display->stage_done < 1) {
        hbx_log(ctx, "vk: create instance");
        r = create_instance(ctx, d, &display->instance);
        hbx_logf(ctx, "vk: instance %d", (int)r);
        if (r != VK_SUCCESS) { display->last_result = r; return -3; }
        display->stage_done = 1;
    }
    if (stage_limit == 1u) return 101;

    /* 2: vi surface + physical device */
    if (display->stage_done < 2) {
        HbxViSurfaceCreateInfoNN info;
        PFN_vkCreateViSurfaceNN create_surface =
            (PFN_vkCreateViSurfaceNN)(uintptr_t)ctx->gpu.create_vi_surface;
        uint32_t devices = 0;
        hbx_memset(&info, 0, sizeof info);
        info.sType = (VkStructureType)HBX_VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
        info.window = (void *)(uintptr_t)ctx->gpu.vi_window;
        hbx_log(ctx, "vk: create vi surface");
        r = create_surface(display->instance, &info, NULL, &display->surface);
        hbx_logf(ctx, "vk: surface %d", (int)r);
        if (r != VK_SUCCESS) { display->last_result = r; return -5; }
        r = d->vkEnumeratePhysicalDevices(display->instance, &devices, NULL);
        hbx_logf(ctx, "vk: physical count %d (%u)", (int)r, devices);
        if (r != VK_SUCCESS || devices == 0) {
            display->last_result = r != VK_SUCCESS ? r : VK_ERROR_INITIALIZATION_FAILED;
            return -4;
        }
        devices = 1;
        r = d->vkEnumeratePhysicalDevices(display->instance, &devices, &display->physical);
        if (r != VK_SUCCESS || display->physical == VK_NULL_HANDLE) {
            display->last_result = r != VK_SUCCESS ? r : VK_ERROR_INITIALIZATION_FAILED;
            return -4;
        }
        display->stage_done = 2;
    }
    if (stage_limit == 2u) return 102;

    /* 3: queue family, device, queue */
    if (display->stage_done < 3) {
        if (!find_queue_family(ctx, d, display->physical, display->surface,
                               &display->queue_family)) {
            display->last_result = VK_ERROR_FEATURE_NOT_PRESENT;
            return -6;
        }
        hbx_logf(ctx, "vk: queue family %u", display->queue_family);
        r = create_device(ctx, d, display->physical, display->queue_family, &display->device);
        hbx_logf(ctx, "vk: device %d", (int)r);
        if (r != VK_SUCCESS) { display->last_result = r; return -7; }
        display->queue = VK_NULL_HANDLE;
        d->vkGetDeviceQueue(display->device, display->queue_family, 0, &display->queue);
        if (display->queue == VK_NULL_HANDLE) {
            display->last_result = VK_ERROR_INITIALIZATION_FAILED;
            return -7;
        }
        display->stage_done = 3;
    }
    if (stage_limit == 3u) return 103;

    /* 4: swapchain, views, acquire semaphore */
    if (display->stage_done < 4) {
        hbx_log(ctx, "vk: create swapchain");
        r = create_swapchain(ctx, d, display);
        hbx_logf(ctx, "vk: swapchain %d", (int)r);
        if (r != VK_SUCCESS) { display->last_result = r; return -8; }
        display->stage_done = 4;
    }
    if (stage_limit == 4u) return 104;

    /* 5: renderer objects (textures, samplers, descriptors, pipeline) */
    if (display->stage_done < 5) {
        icon_layers = 0;
        if (ctx->catalog && ctx->catalog_bytes >= 32u) {
            const struct hbx_catalog *catalog =
                (const struct hbx_catalog *)(uintptr_t)ctx->catalog;
            if (catalog->magic == HBX_CATALOG_MAGIC && catalog->icon_count > 0) {
                uint64_t available = ctx->icons_bytes / (HBX_ICON_SIZE * HBX_ICON_SIZE * 4u);
                icon_layers = catalog->icon_count;
                if (icon_layers > HBX_MAX_ICON_LAYERS) icon_layers = HBX_MAX_ICON_LAYERS;
                if ((uint64_t)icon_layers > available) icon_layers = (uint32_t)available;
            }
        }
        hbx_logf(ctx, "vk: ui init, %u icon layers", icon_layers);
        r = hbx_ui_init(&display->ui, d, display->instance, display->physical,
                        display->device, display->queue, display->queue_family,
                        display->width, display->height, display->format,
                        (const void *)(uintptr_t)ctx->atlas,
                        (uint32_t)ctx->atlas_bytes, icon_layers);
        hbx_logf(ctx, "vk: ui init %d", (int)r);
        if (r != VK_SUCCESS) { display->last_result = r; return -9; }
        if (ctx->icons && icon_layers > 0) {
            r = hbx_ui_set_icons(&display->ui, (const void *)(uintptr_t)ctx->icons,
                                 icon_layers);
            hbx_logf(ctx, "vk: icons %d", (int)r);
        }
        display->stage_done = 5;
    }
    if (stage_limit == 5u) return 105;

    /* 6: attach the swapchain to the renderer */
    if (display->stage_done < 6) {
        r = hbx_ui_attach_swapchain(&display->ui, display->swapchain, display->surface,
                                    display->image_count, display->images, display->views,
                                    display->acquire, VK_NULL_HANDLE);
        hbx_logf(ctx, "vk: attach %d", (int)r);
        if (r != VK_SUCCESS) { display->last_result = r; return -10; }
        display->stage_done = 6;
        display->initialized = 1;
    }
    hbx_log(ctx, "vk: display ready");
    if (stage_limit == 6u) return 106;
    return HBX_OK;
}

int launcher_display_release(struct launcher_display *display, const struct hbx_ctx *ctx) {
    const struct hbx_vk_dispatch *d = hbx_vk(ctx);
    uint32_t i;
    if (!d || !display->initialized || display->released) return HBX_OK;
    d->vkQueueWaitIdle(display->queue);
    for (i = 0; i < display->image_count; i++) {
        if (display->views[i] && d->vkDestroyImageView)
            d->vkDestroyImageView(display->device, display->views[i], NULL);
        display->views[i] = VK_NULL_HANDLE;
        display->images[i] = VK_NULL_HANDLE;
    }
    for (i = 0; i < display->ui.image_count && i < HBX_MAX_IMAGES; i++) {
        if (display->ui.render_semaphores[i] && d->vkDestroySemaphore)
            d->vkDestroySemaphore(display->device, display->ui.render_semaphores[i], NULL);
        display->ui.render_semaphores[i] = VK_NULL_HANDLE;
    }
    if (display->acquire && d->vkDestroySemaphore)
        d->vkDestroySemaphore(display->device, display->acquire, NULL);
    display->acquire = VK_NULL_HANDLE;
    if (display->swapchain && d->vkDestroySwapchainKHR)
        d->vkDestroySwapchainKHR(display->device, display->swapchain, NULL);
    display->swapchain = VK_NULL_HANDLE;
    if (display->surface && d->vkDestroySurfaceKHR)
        d->vkDestroySurfaceKHR(display->instance, display->surface, NULL);
    display->surface = VK_NULL_HANDLE;
    display->ui.swapchain = VK_NULL_HANDLE;
    display->ui.surface = VK_NULL_HANDLE;
    display->released = 1;
    hbx_log(ctx, "vk: display released for an exclusive app");
    return HBX_OK;
}

int launcher_display_reacquire(struct launcher_display *display, const struct hbx_ctx *ctx) {
    const struct hbx_vk_dispatch *d = hbx_vk(ctx);
    HbxViSurfaceCreateInfoNN info;
    PFN_vkCreateViSurfaceNN create_surface =
        (PFN_vkCreateViSurfaceNN)(uintptr_t)ctx->gpu.create_vi_surface;
    VkResult r;
    if (!display->released) return HBX_OK;
    if (!d || !create_surface || !ctx->gpu.vi_window) return HBX_ERR_GPU;
    hbx_memset(&info, 0, sizeof info);
    info.sType = (VkStructureType)HBX_VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    info.window = (void *)(uintptr_t)ctx->gpu.vi_window;
    r = create_surface(display->instance, &info, NULL, &display->surface);
    hbx_logf(ctx, "vk: reacquire surface %d", (int)r);
    if (r != VK_SUCCESS) { display->last_result = r; return HBX_ERR_GPU; }
    r = create_swapchain(ctx, d, display);
    hbx_logf(ctx, "vk: reacquire swapchain %d", (int)r);
    if (r != VK_SUCCESS) { display->last_result = r; return HBX_ERR_GPU; }
    r = hbx_ui_attach_swapchain(&display->ui, display->swapchain, display->surface,
                                display->image_count, display->images, display->views,
                                display->acquire, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) { display->last_result = r; return HBX_ERR_GPU; }
    display->released = 0;
    return HBX_OK;
}
