/*
 * hbx_vk.h - Vulkan 1.4 entrypoints for HBLauncher Native apps.
 *
 * The console process contains a Vulkan loader and driver, but a freestanding
 * app cannot link against them.  The host resolves every entrypoint from the
 * system module for the current process and writes absolute addresses into
 * `struct hbx_vk_dispatch`, whose address arrives in ctx->gpu.dispatch.
 *
 * Apps call through the table: `HBX_VK(ctx)->vkCmdDraw(...)`.  The table is
 * read-only for apps; the host rebuilds it for every process start because the
 * module base is ASLR-dependent.
 *
 * Only Vulkan 1.3+ core dynamic rendering is used (no render passes).  The
 * swapchain is owned by the launcher (see hbx_sdk.h).
 *
 * Copyright (c) 2026 HBLauncher Native contributors.  MIT licensed.
 */
#ifndef HBX_VK_H
#define HBX_VK_H

/* We never link a loader; every call goes through the injected table. */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stddef.h>

#include "hbx_sdk.h"

/* --- NN vi surface extension (not in the public Vulkan headers) ---------- */
#define HBX_VK_NN_VI_SURFACE_EXTENSION_NAME "VK_NN_vi_surface"
#define HBX_VK_KHR_SURFACE_EXTENSION_NAME   "VK_KHR_surface"
#define HBX_VK_KHR_SWAPCHAIN_EXTENSION_NAME "VK_KHR_swapchain"

#ifndef HBX_VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN
#define HBX_VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN 1000062000u
#endif

typedef struct HbxViSurfaceCreateInfoNN {
    VkStructureType sType;
    const void     *pNext;
    VkFlags         flags;
    void           *window;
} HbxViSurfaceCreateInfoNN;

typedef VkResult (VKAPI_PTR *PFN_vkCreateViSurfaceNN)(
    VkInstance instance, const HbxViSurfaceCreateInfoNN *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);

/* --- Dispatch table ------------------------------------------------------ */
/*
 * Field order is the ABI shared with host/vk/dispatch.cjs.  The host parses
 * this header to emit the table, so keep one field per declaration and do not
 * reorder without regenerating (the loader re-derives offsets every run).
 */
struct hbx_vk_dispatch {
    /* Instance-level */
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkCreateViSurfaceNN vkCreateViSurfaceNN;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;

    /* Device-level */
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle;

    /* Swapchain */
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkAcquireNextImage2KHR vkAcquireNextImage2KHR;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkCreateSemaphore vkCreateSemaphore;
    PFN_vkDestroySemaphore vkDestroySemaphore;
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory vkBindImageMemory;

    /* Buffers and memory */
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;

    /* Descriptors, samplers, pipelines */
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;

    /* Commands */
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdBeginRendering vkCmdBeginRendering;
    PFN_vkCmdEndRendering vkCmdEndRendering;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdDraw vkCmdDraw;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
    PFN_vkQueueSubmit vkQueueSubmit;
    PFN_vkQueueWaitIdle vkQueueWaitIdle;
    PFN_vkCmdPushConstants vkCmdPushConstants;
};

/* Number of entries; the host checks its own count against this at load. */
#define HBX_VK_DISPATCH_FIELDS (sizeof(struct hbx_vk_dispatch) / sizeof(void *))

static inline const struct hbx_vk_dispatch *hbx_vk(const struct hbx_ctx *ctx) {
    return (const struct hbx_vk_dispatch *)(uintptr_t)ctx->gpu.dispatch;
}

/* --- Small creation helpers --------------------------------------------- */

/* Find the first memory type satisfying `required` flags in `bits`. */
static inline int32_t hbx_vk_memory_type(const struct hbx_ctx *ctx,
                                         uint32_t required, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties;
    const struct hbx_vk_dispatch *d = hbx_vk(ctx);
    uint32_t i;
    if (!d || !d->vkGetPhysicalDeviceMemoryProperties) return -1;
    hbx_memset(&properties, 0, sizeof properties);
    d->vkGetPhysicalDeviceMemoryProperties(
        (VkPhysicalDevice)(uintptr_t)ctx->gpu.physical, &properties);
    for (i = 0; i < properties.memoryTypeCount && i < 32u; i++)
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) == required)
            return (int32_t)i;
    return -1;
}

/* Create a host-visible, host-coherent buffer and map it.  Used for vertex
 * data, staging and readback.  Returns VK_SUCCESS and fills `buffer`/`memory`
 * /`mapped`. */
static inline VkResult hbx_vk_host_buffer(const struct hbx_ctx *ctx,
                                          VkDeviceSize bytes, VkBufferUsageFlags usage,
                                          VkBuffer *buffer, VkDeviceMemory *memory,
                                          void **mapped) {
    const struct hbx_vk_dispatch *d = hbx_vk(ctx);
    VkDevice device = (VkDevice)(uintptr_t)ctx->gpu.device;
    VkBufferCreateInfo info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation;
    int32_t type;
    VkResult r;

    if (!d || !buffer || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    if (mapped) *mapped = NULL;

    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = d->vkCreateBuffer(device, &info, NULL, buffer);
    if (r != VK_SUCCESS) return r;

    hbx_memset(&requirements, 0, sizeof requirements);
    d->vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    type = hbx_vk_memory_type(ctx, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              requirements.memoryTypeBits);
    if (type < 0) return VK_ERROR_FEATURE_NOT_PRESENT;

    hbx_memset(&allocation, 0, sizeof allocation);
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = (uint32_t)type;
    r = d->vkAllocateMemory(device, &allocation, NULL, memory);
    if (r != VK_SUCCESS) return r;
    r = d->vkBindBufferMemory(device, *buffer, *memory, 0);
    if (r != VK_SUCCESS) return r;
    if (mapped) {
        r = d->vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
        if (r != VK_SUCCESS) { *mapped = NULL; return r; }
    }
    return VK_SUCCESS;
}

#endif /* HBX_VK_H */
