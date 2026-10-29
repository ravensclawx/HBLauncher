/*
 * Raw Vulkan example - an app that owns its own pipeline.
 *
 * The launcher owns the display; an app borrows it.  This example creates a
 * pipeline, a command pool and semaphores through the injected dispatch table,
 * draws a rotating triangle into the launcher's swapchain image views, and
 * presents.  It never touches the instance, device, swapchain or views.
 *
 * Build:  python3 sdk/tools/hbx-build.py sdk/examples/raw-vulkan
 * Then copy the directory to apps/ and run ./hbl-start.
 */
#include "hbx_sdk.h"
#include "hbx_app.h"
#include "hbx_vk.h"
#include "hbx_ui_shaders.h"
#include "hbx_math.h"

struct demo {
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[HBX_MAX_IMAGES];
    VkImageView views[HBX_MAX_IMAGES];
    uint32_t image_count;
    uint32_t width, height;
    VkFormat format;
    VkShaderModule vert, frag;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkCommandPool pool;
    VkCommandBuffer commands[HBX_MAX_IMAGES];
    VkSemaphore acquire, draw_done[HBX_MAX_IMAGES];
    uint32_t frame;
};

static VkShaderModule make_module(const struct hbx_vk_dispatch *d, VkDevice device,
                                  const uint32_t *words, uint32_t count) {
    VkShaderModuleCreateInfo info;
    VkShaderModule module = VK_NULL_HANDLE;
    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = (size_t)count * 4u;
    info.pCode = words;
    if (d->vkCreateShaderModule(device, &info, NULL, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

static int create_pipeline(struct demo *demo, const struct hbx_vk_dispatch *d) {
    VkPushConstantRange push;
    VkPipelineLayoutCreateInfo layout_info;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineRenderingCreateInfo rendering;
    VkGraphicsPipelineCreateInfo info;

    demo->vert = make_module(d, demo->device, hbx_shader_tri_vert, hbx_shader_tri_vert_words);
    demo->frag = make_module(d, demo->device, hbx_shader_tri_frag, hbx_shader_tri_frag_words);
    if (!demo->vert || !demo->frag) return HBX_ERR_GPU;

    hbx_memset(&push, 0, sizeof push);
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = 12;
    hbx_memset(&layout_info, 0, sizeof layout_info);
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (d->vkCreatePipelineLayout(demo->device, &layout_info, NULL, &demo->layout) != VK_SUCCESS)
        return HBX_ERR_GPU;

    hbx_memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = demo->vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = demo->frag;
    stages[1].pName = "main";

    hbx_memset(&vertex_input, 0, sizeof vertex_input);
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    hbx_memset(&assembly, 0, sizeof assembly);
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    hbx_memset(&viewport, 0, sizeof viewport);
    viewport.width = (float)demo->width;
    viewport.height = (float)demo->height;
    viewport.maxDepth = 1.0f;
    hbx_memset(&scissor, 0, sizeof scissor);
    scissor.extent.width = demo->width;
    scissor.extent.height = demo->height;
    hbx_memset(&viewport_state, 0, sizeof viewport_state);
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    hbx_memset(&raster, 0, sizeof raster);
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    hbx_memset(&multisample, 0, sizeof multisample);
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    hbx_memset(&blend_attachment, 0, sizeof blend_attachment);
    blend_attachment.blendEnable = VK_FALSE;
    blend_attachment.colorWriteMask = 0xf;
    hbx_memset(&blend, 0, sizeof blend);
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    hbx_memset(&rendering, 0, sizeof rendering);
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &demo->format;

    hbx_memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.layout = demo->layout;
    if (d->vkCreateGraphicsPipelines(demo->device, VK_NULL_HANDLE, 1, &info, NULL,
                                     &demo->pipeline) != VK_SUCCESS)
        return HBX_ERR_GPU;
    return HBX_OK;
}

static VkResult draw_frame(struct demo *demo, const struct hbx_vk_dispatch *d, float angle) {
    VkCommandBufferBeginInfo begin;
    VkImageMemoryBarrier barrier;
    VkRenderingAttachmentInfo attachment;
    VkRenderingInfo rendering;
    VkSubmitInfo submit;
    VkPresentInfoKHR present_info;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    float push[3];
    uint32_t index = 0;
    VkResult r;

    r = d->vkAcquireNextImageKHR(demo->device, demo->swapchain, UINT64_MAX,
                                 demo->acquire, VK_NULL_HANDLE, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return r;
    if (index >= demo->image_count) index = 0;

    hbx_memset(&begin, 0, sizeof begin);
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vkBeginCommandBuffer(demo->commands[index], &begin);

    hbx_memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = demo->images[index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* The image was acquired through a semaphore waited on at
       COLOR_ATTACHMENT_OUTPUT, so the transition belongs to that stage. */
    d->vkCmdPipelineBarrier(demo->commands[index],
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                            0, NULL, 0, NULL, 1, &barrier);

    hbx_memset(&attachment, 0, sizeof attachment);
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = demo->views[index];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color.float32[0] = 0.05f;
    attachment.clearValue.color.float32[1] = 0.10f;
    attachment.clearValue.color.float32[2] = 0.16f;
    attachment.clearValue.color.float32[3] = 1.0f;

    hbx_memset(&rendering, 0, sizeof rendering);
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent.width = demo->width;
    rendering.renderArea.extent.height = demo->height;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    d->vkCmdBeginRendering(demo->commands[index], &rendering);
    d->vkCmdBindPipeline(demo->commands[index], VK_PIPELINE_BIND_POINT_GRAPHICS, demo->pipeline);
    push[0] = (float)demo->width;
    push[1] = (float)demo->height;
    push[2] = angle;
    d->vkCmdPushConstants(demo->commands[index], demo->layout, VK_SHADER_STAGE_VERTEX_BIT,
                          0, sizeof push, push);
    d->vkCmdDraw(demo->commands[index], 3, 1, 0, 0);
    d->vkCmdEndRendering(demo->commands[index]);

    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;
    d->vkCmdPipelineBarrier(demo->commands[index],
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                            0, NULL, 0, NULL, 1, &barrier);
    d->vkEndCommandBuffer(demo->commands[index]);

    hbx_memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &demo->acquire;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &demo->commands[index];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &demo->draw_done[index];
    r = d->vkQueueSubmit(demo->queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) return r;

    hbx_memset(&present_info, 0, sizeof present_info);
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &demo->draw_done[index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &demo->swapchain;
    present_info.pImageIndices = &index;
    r = d->vkQueuePresentKHR(demo->queue, &present_info);
    d->vkQueueWaitIdle(demo->queue);
    return r;
}

static void destroy(struct demo *demo, const struct hbx_vk_dispatch *d) {
    if (demo->device) d->vkDeviceWaitIdle(demo->device);
    if (demo->pipeline) d->vkDestroyPipeline(demo->device, demo->pipeline, NULL);
    if (demo->layout) d->vkDestroyPipelineLayout(demo->device, demo->layout, NULL);
    if (demo->vert) d->vkDestroyShaderModule(demo->device, demo->vert, NULL);
    if (demo->frag) d->vkDestroyShaderModule(demo->device, demo->frag, NULL);
    if (demo->pool) d->vkDestroyCommandPool(demo->device, demo->pool, NULL);
    if (demo->acquire) d->vkDestroySemaphore(demo->device, demo->acquire, NULL);
    for (uint32_t i = 0; i < demo->image_count && i < HBX_MAX_IMAGES; i++)
        if (demo->draw_done[i]) d->vkDestroySemaphore(demo->device, demo->draw_done[i], NULL);
}

int hbx_app_main(const struct hbx_ctx *ctx) {
    static struct demo demo;
    const struct hbx_vk_dispatch *d = hbx_vk(ctx);
    struct hbx_input_state input;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo allocate_info;
    VkSemaphoreCreateInfo semaphore;
    uint64_t deadline = 0;
    float angle = 0.0f;
    uint32_t i;
    int status;

    if (hbx_ctx_check(ctx) != HBX_OK) return HBX_ERR_ABI;
    if (!d || !ctx->gpu.device || !ctx->gpu.swapchain) {
        hbx_log(ctx, "raw-vulkan: no borrowed display");
        return HBX_ERR_GPU;
    }
    hbx_memset(&demo, 0, sizeof demo);
    demo.device = (VkDevice)(uintptr_t)ctx->gpu.device;
    demo.queue = (VkQueue)(uintptr_t)ctx->gpu.queue;
    demo.swapchain = (VkSwapchainKHR)(uintptr_t)ctx->gpu.swapchain;
    demo.image_count = ctx->gpu.image_count;
    demo.width = ctx->gpu.width;
    demo.height = ctx->gpu.height;
    demo.format = (VkFormat)ctx->gpu.color_format;
    for (i = 0; i < demo.image_count && i < HBX_MAX_IMAGES; i++) {
        demo.images[i] = (VkImage)(uintptr_t)ctx->gpu.images[i];
        demo.views[i] = (VkImageView)(uintptr_t)ctx->gpu.views[i];
    }

    status = create_pipeline(&demo, d);
    if (status != HBX_OK) { destroy(&demo, d); return status; }

    hbx_memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = ctx->gpu.queue_family;
    if (d->vkCreateCommandPool(demo.device, &pool_info, NULL, &demo.pool) != VK_SUCCESS)
        { destroy(&demo, d); return HBX_ERR_GPU; }

    hbx_memset(&allocate_info, 0, sizeof allocate_info);
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = demo.pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = demo.image_count;
    if (d->vkAllocateCommandBuffers(demo.device, &allocate_info, demo.commands) != VK_SUCCESS)
        { destroy(&demo, d); return HBX_ERR_GPU; }

    hbx_memset(&semaphore, 0, sizeof semaphore);
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    d->vkCreateSemaphore(demo.device, &semaphore, NULL, &demo.acquire);
    /* One render-finished semaphore per image: presentation may still own the
       previous one when the next frame is submitted. */
    for (i = 0; i < demo.image_count && i < HBX_MAX_IMAGES; i++)
        d->vkCreateSemaphore(demo.device, &semaphore, NULL, &demo.draw_done[i]);

    hbx_log(ctx, "raw-vulkan: pipeline ready");
    hbx_memset(&input, 0, sizeof input);
    while (!hbx_stop_requested(ctx) && ctx->exit == 0) {
        hbx_input_poll(ctx, &input);
        if (input.pressed & (HBX_B | HBX_PLUS)) break;
        angle += 0.02f;
        if (draw_frame(&demo, d, angle) != VK_SUCCESS) {
            hbx_log(ctx, "raw-vulkan: present failed");
            status = HBX_ERR_GPU;
            goto done;
        }
        demo.frame++;
        hbx_frame_end(ctx);
        hbx_pace(ctx, &deadline, 60);
    }
    status = HBX_OK;
done:
    destroy(&demo, d);
    hbx_logf(ctx, "raw-vulkan: exit after %u frames", demo.frame);
    return status;
}
