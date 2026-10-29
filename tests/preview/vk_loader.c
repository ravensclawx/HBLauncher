/*
 * vk_loader.c - fill struct hbx_vk_dispatch from the host Vulkan loader.
 *
 * Only the host preview and SDK tests use this file; the console path fills the
 * table from the system module's symbol table instead.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "hbx_vk.h"

static void *(*get_instance_proc)(VkInstance, const char *);
static void *(*get_device_proc)(VkDevice, const char *);

void *hbx_host_vulkan_open(void) {
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "vk_loader: cannot open libvulkan: %s\n", dlerror());
        return NULL;
    }
    get_instance_proc = (void *(*)(VkInstance, const char *))dlsym(library, "vkGetInstanceProcAddr");
    if (!get_instance_proc) {
        fprintf(stderr, "vk_loader: vkGetInstanceProcAddr missing\n");
        return NULL;
    }
    return library;
}

void hbx_host_vulkan_fill(struct hbx_vk_dispatch *d, VkInstance instance, VkDevice device) {
    memset(d, 0, sizeof *d);
    get_device_proc = NULL;
    if (instance) {
        PFN_vkGetDeviceProcAddr gdpa =
            (PFN_vkGetDeviceProcAddr)get_instance_proc(instance, "vkGetDeviceProcAddr");
        get_device_proc = (void *(*)(VkDevice, const char *))gdpa;
    }
#define INSTANCE_FN(field, name) \
    d->field = (PFN_##field)get_instance_proc(instance, name)
#define DEVICE_FN(field, name) \
    d->field = device && get_device_proc ? (PFN_##field)get_device_proc(device, name) : NULL

    INSTANCE_FN(vkCreateInstance, "vkCreateInstance");
    INSTANCE_FN(vkEnumerateInstanceExtensionProperties, "vkEnumerateInstanceExtensionProperties");
    d->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)get_instance_proc;
    d->vkCreateViSurfaceNN = NULL;
    INSTANCE_FN(vkDestroySurfaceKHR, "vkDestroySurfaceKHR");
    INSTANCE_FN(vkDestroyInstance, "vkDestroyInstance");
    INSTANCE_FN(vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    INSTANCE_FN(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    INSTANCE_FN(vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
    INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR");
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    INSTANCE_FN(vkGetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    INSTANCE_FN(vkCreateDevice, "vkCreateDevice");

    DEVICE_FN(vkDestroyDevice, "vkDestroyDevice");
    DEVICE_FN(vkGetDeviceQueue, "vkGetDeviceQueue");
    DEVICE_FN(vkDeviceWaitIdle, "vkDeviceWaitIdle");
    DEVICE_FN(vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    DEVICE_FN(vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    DEVICE_FN(vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    DEVICE_FN(vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
    DEVICE_FN(vkQueuePresentKHR, "vkQueuePresentKHR");
    DEVICE_FN(vkCreateImageView, "vkCreateImageView");
    DEVICE_FN(vkDestroyImageView, "vkDestroyImageView");
    DEVICE_FN(vkCreateSemaphore, "vkCreateSemaphore");
    DEVICE_FN(vkDestroySemaphore, "vkDestroySemaphore");
    DEVICE_FN(vkCreateImage, "vkCreateImage");
    DEVICE_FN(vkDestroyImage, "vkDestroyImage");
    DEVICE_FN(vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    DEVICE_FN(vkBindImageMemory, "vkBindImageMemory");
    DEVICE_FN(vkCreateBuffer, "vkCreateBuffer");
    DEVICE_FN(vkDestroyBuffer, "vkDestroyBuffer");
    DEVICE_FN(vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    DEVICE_FN(vkAllocateMemory, "vkAllocateMemory");
    DEVICE_FN(vkFreeMemory, "vkFreeMemory");
    DEVICE_FN(vkBindBufferMemory, "vkBindBufferMemory");
    DEVICE_FN(vkMapMemory, "vkMapMemory");
    DEVICE_FN(vkUnmapMemory, "vkUnmapMemory");
    DEVICE_FN(vkCreateSampler, "vkCreateSampler");
    DEVICE_FN(vkDestroySampler, "vkDestroySampler");
    DEVICE_FN(vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    DEVICE_FN(vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    DEVICE_FN(vkCreateDescriptorPool, "vkCreateDescriptorPool");
    DEVICE_FN(vkDestroyDescriptorPool, "vkDestroyDescriptorPool");
    DEVICE_FN(vkAllocateDescriptorSets, "vkAllocateDescriptorSets");
    DEVICE_FN(vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
    DEVICE_FN(vkCreatePipelineLayout, "vkCreatePipelineLayout");
    DEVICE_FN(vkDestroyPipelineLayout, "vkDestroyPipelineLayout");
    DEVICE_FN(vkCreateShaderModule, "vkCreateShaderModule");
    DEVICE_FN(vkDestroyShaderModule, "vkDestroyShaderModule");
    DEVICE_FN(vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    DEVICE_FN(vkDestroyPipeline, "vkDestroyPipeline");
    DEVICE_FN(vkCreateCommandPool, "vkCreateCommandPool");
    DEVICE_FN(vkDestroyCommandPool, "vkDestroyCommandPool");
    DEVICE_FN(vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    DEVICE_FN(vkResetCommandBuffer, "vkResetCommandBuffer");
    DEVICE_FN(vkBeginCommandBuffer, "vkBeginCommandBuffer");
    DEVICE_FN(vkEndCommandBuffer, "vkEndCommandBuffer");
    DEVICE_FN(vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    DEVICE_FN(vkCmdBeginRendering, "vkCmdBeginRendering");
    DEVICE_FN(vkCmdEndRendering, "vkCmdEndRendering");
    DEVICE_FN(vkCmdBindPipeline, "vkCmdBindPipeline");
    DEVICE_FN(vkCmdBindVertexBuffers, "vkCmdBindVertexBuffers");
    DEVICE_FN(vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    DEVICE_FN(vkCmdDraw, "vkCmdDraw");
    DEVICE_FN(vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    DEVICE_FN(vkCmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
    DEVICE_FN(vkQueueSubmit, "vkQueueSubmit");
    DEVICE_FN(vkQueueWaitIdle, "vkQueueWaitIdle");
    DEVICE_FN(vkCmdPushConstants, "vkCmdPushConstants");
#undef INSTANCE_FN
#undef DEVICE_FN
}
