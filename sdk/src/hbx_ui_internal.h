/*
 * hbx_ui_internal.h - private renderer layout shared by the launcher, the
 * host preview and the SDK self-tests.  Not part of the public SDK ABI.
 */
#ifndef HBX_UI_INTERNAL_H
#define HBX_UI_INTERNAL_H

#include "hbx_ui.h"
#include "hbx_vk.h"
#include "hbx_font.h"

#define HBX_UI_MAX_VERTICES 163840u  /* 163,840 quads-worth at 24 B = 3.75 MiB */
#define HBX_UI_VERTEX_BYTES 24u

struct hbx_ui_vertex {
    float x, y;
    float u, v;
    uint32_t color;   /* 0xAABBGGRR */
    uint32_t tex;     /* bits 0..1 mode, bits 8..15 icon layer */
};

#define HBX_TEX_SOLID 0u
#define HBX_TEX_GLYPH 1u
#define HBX_TEX_ICON  2u
#define HBX_TEX_IMAGE 3u
#define HBX_TEX_MODE_MASK 3u
#define HBX_TEX_LAYER_SHIFT 8u

struct hbx_ui_impl {
    uint32_t abi;
    uint32_t ready;

    const struct hbx_vk_dispatch *d;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;

    uint32_t width, height;
    VkFormat format;
    uint32_t image_count;
    VkImage images[HBX_MAX_IMAGES];
    VkImageView views[HBX_MAX_IMAGES];
    VkSwapchainKHR swapchain;
    VkSurfaceKHR surface;
    VkSemaphore acquire_semaphore;
    VkSemaphore render_semaphores[HBX_MAX_IMAGES];  /* one per swapchain image */

    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    VkSampler atlas_sampler;
    VkSampler icon_sampler;

    VkImage app_image;
    VkDeviceMemory app_memory;
    VkImageView app_view;
    VkSampler app_sampler;
    VkBuffer app_staging;
    VkDeviceMemory app_staging_memory;
    unsigned char *app_staging_mapped;
    VkImageLayout app_layout;
    uint32_t app_width;
    uint32_t app_height;
    uint32_t app_dirty;

    VkImage atlas_image;
    VkDeviceMemory atlas_memory;
    VkImageView atlas_view;
    VkImage icon_image;
    VkDeviceMemory icon_memory;
    VkImageView icon_view;
    uint32_t icon_layers;

    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    struct hbx_ui_vertex *vertex_mapped;
    uint32_t vertex_count;
    uint32_t vertex_capacity;

    VkCommandPool command_pool;
    /* One per swapchain image plus the shared upload buffer. */
    VkCommandBuffer commands[HBX_MAX_IMAGES + 1];
    VkCommandBuffer upload_command;

    uint32_t background;
    VkImageLayout atlas_layout;
    VkImageLayout icon_layout;
    uint32_t frame;
    uint32_t overflow;
    uint32_t last_error;
    uint32_t out_of_date;   /* frames skipped because the swapchain was stale */
};

/* Create the device-side objects (pipeline, atlas, vertex buffer).  The
 * display handles may be zero for an offscreen target. */
VkResult hbx_ui_init(struct hbx_ui_impl *impl, const struct hbx_vk_dispatch *d,
                     VkInstance instance, VkPhysicalDevice physical,
                     VkDevice device, VkQueue queue, uint32_t queue_family,
                     uint32_t width, uint32_t height, VkFormat format,
                     const void *atlas_pixels, uint32_t atlas_bytes,
                     uint32_t icon_layers);

/* Attach a swapchain (console display path). */
VkResult hbx_ui_attach_swapchain(struct hbx_ui_impl *impl, VkSwapchainKHR swapchain,
                                 VkSurfaceKHR surface, uint32_t image_count,
                                 const VkImage *images, const VkImageView *views,
                                 VkSemaphore acquire, VkSemaphore render);

/* Upload icon layers (RGBA8, 96x96 each).  Safe to call once after init. */
VkResult hbx_ui_set_icons(struct hbx_ui_impl *impl, const void *pixels, uint32_t layers);

/* Geometry. */
void hbx_ui_engine_begin(struct hbx_ui_impl *impl, uint32_t background);
void hbx_ui_engine_emit(struct hbx_ui_impl *impl, const struct hbx_ui_vertex *vertices,
                 uint32_t count);

/* Render the accumulated frame into an explicit image (offscreen preview). */
VkResult hbx_ui_render_offscreen(struct hbx_ui_impl *impl, VkImage image,
                                 VkImageView view, VkFormat format);

/* Publish the call-through table. */
void hbx_ui_publish(struct hbx_ui_impl *impl, struct hbx_render *out);

/* Engine entry points (same implementations the ops table uses). */
void hbx_ui_engine_triangle(struct hbx_ui_impl *impl, float x0, float y0, float x1,
                            float y1, float x2, float y2, uint32_t c0, uint32_t c1,
                            uint32_t c2);
void hbx_ui_engine_quad_gradient(struct hbx_ui_impl *impl, float x0, float y0,
                                 float x1, float y1, uint32_t top_left,
                                 uint32_t top_right, uint32_t bottom_right,
                                 uint32_t bottom_left);
/* A band whose bottom edge slopes from bottom_left to bottom_right at x1. */
void hbx_ui_engine_band(struct hbx_ui_impl *impl, float x0, float x1, float top,
                        float bottom_left, float bottom_right, uint32_t top_left,
                        uint32_t top_right, uint32_t bottom_left_color,
                        uint32_t bottom_right_color);
void hbx_ui_engine_rect(struct hbx_ui_impl *impl, float x, float y, float w, float h,
                        float radius, uint32_t color);
void hbx_ui_engine_stroke(struct hbx_ui_impl *impl, float x, float y, float w, float h,
                          float radius, float thickness, uint32_t color);
float hbx_ui_engine_text(struct hbx_ui_impl *impl, float x, float y, uint32_t font,
                         const char *text, uint32_t color);
float hbx_ui_engine_text_width(uint32_t font, const char *text);
void hbx_ui_engine_text_center(struct hbx_ui_impl *impl, float cx, float y, uint32_t font,
                               const char *text, uint32_t color);
void hbx_ui_engine_line(struct hbx_ui_impl *impl, float x0, float y0, float x1, float y1,
                        float width, uint32_t color, uint32_t flags);
void hbx_ui_engine_circle(struct hbx_ui_impl *impl, float cx, float cy, float radius,
                          float thickness, uint32_t color);
void hbx_ui_engine_icon(struct hbx_ui_impl *impl, float x, float y, float size,
                        uint32_t layer, uint32_t tint);
uint32_t hbx_ui_engine_vertex_count(const struct hbx_ui_impl *impl);
uint32_t hbx_ui_engine_overflow(const struct hbx_ui_impl *impl);

#endif /* HBX_UI_INTERNAL_H */
