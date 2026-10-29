/*
 * launcher_vk.h - console Vulkan display bring-up for the launcher.
 *
 * The launcher owns the display so apps can borrow it: instance, physical
 * device, queue, vi surface and the 1920x1080 swapchain are created once and
 * retained for the life of the process (the proven SM64/NES policy is to never
 * tear the vi swapchain down).
 */
#ifndef LAUNCHER_VK_H
#define LAUNCHER_VK_H

#include "hbx_sdk.h"
#include "../../sdk/src/hbx_ui_internal.h"

struct launcher_display {
    struct hbx_ui_impl ui;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    uint32_t image_count;
    VkImage images[HBX_MAX_IMAGES];
    VkImageView views[HBX_MAX_IMAGES];
    VkSemaphore acquire;
    uint32_t width;
    uint32_t height;
    VkResult last_result;
    uint32_t initialized;
    uint32_t stage_done;   /* highest bring-up stage completed (persists) */
    uint32_t released;     /* surface + swapchain handed to an exclusive app */
};

/* Exclusive apps (their own Vulkan/Dawn swapchain on the same vi window):
 * release the surface and swapchain, keeping the instance, device and
 * renderer; reacquire rebuilds them on the next entry. */
int launcher_display_release(struct launcher_display *display, const struct hbx_ctx *ctx);
int launcher_display_reacquire(struct launcher_display *display, const struct hbx_ctx *ctx);

/* Create everything.  Returns HBX_OK or a negative status; `display->last_result`
 * carries the failing VkResult.
 *
 * `stage_limit` is a diagnostic: when non-zero, creation stops after that stage
 * (1 instance, 2 physical+surface, 3 device, 4 swapchain, 5 renderer objects,
 * 6 attach) and returns 100 + stage.  The launcher then exits cleanly, which
 * tells the host exactly how far the display stack got. */
int launcher_display_create(struct launcher_display *display, const struct hbx_ctx *ctx,
                            uint32_t stage_limit);

#endif
