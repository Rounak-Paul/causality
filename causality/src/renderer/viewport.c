// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* viewport.c — offscreen render target for external renderers */
#include "viewport.h"
#include <string.h>
#include <stdio.h>

/* ---- Helpers ---- */

static uint32_t find_memory_type(VkPhysicalDevice gpu, uint32_t type_bits,
                                 VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/* ---- GPU resource lifecycle ---- */

bool ca_viewport_gpu_create(Ca_Instance *inst, Ca_Viewport *vp,
                            uint32_t width, uint32_t height, VkFormat format)
{
    if (width == 0 || height == 0) return false;

    vp->width  = width;
    vp->height = height;
    vp->format = format;
    vp->has_rendered_once = false;

    /* Colour image — used as both colour attachment and sampled texture */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(inst->vk_device, &img_ci, NULL, &vp->color_image) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkCreateImage failed\n");
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(inst->vk_device, vp->color_image, &req);
    uint32_t mem_idx = find_memory_type(inst->vk_gpu, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_idx == UINT32_MAX) {
        fprintf(stderr, "[viewport] no suitable memory type\n");
        vkDestroyImage(inst->vk_device, vp->color_image, NULL);
        vp->color_image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mem_idx,
    };
    if (vkAllocateMemory(inst->vk_device, &mem_ai, NULL, &vp->color_memory) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkAllocateMemory failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }
    if (vkBindImageMemory(inst->vk_device, vp->color_image, vp->color_memory, 0) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkBindImageMemory failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = vp->color_image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(inst->vk_device, &view_ci, NULL, &vp->color_view) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkCreateImageView failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    /* Sampler (linear, clamp — for compositing into UI) */
    VkSamplerCreateInfo samp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = VK_LOD_CLAMP_NONE,
    };
    if (vkCreateSampler(inst->vk_device, &samp_ci, NULL, &vp->sampler) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkCreateSampler failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    /* Descriptor set — reuses the text pipeline's descriptor set layout for
       combined image sampler at binding 0 */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = inst->image_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &inst->text_pipeline.desc_layout,
    };
    if (vkAllocateDescriptorSets(inst->vk_device, &ds_ai, &vp->desc_set) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] descriptor set alloc failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    VkDescriptorImageInfo img_info = {
        .sampler     = vp->sampler,
        .imageView   = vp->color_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wr = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = vp->desc_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &img_info,
    };
    vkUpdateDescriptorSets(inst->vk_device, 1, &wr, 0, NULL);

    /* Per-viewport command buffer */
    VkCommandBufferAllocateInfo cmd_ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = inst->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(inst->vk_device, &cmd_ai, &vp->cmd) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkAllocateCommandBuffers failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    /* Fence for reclaiming vp->cmd once the GPU is done with last frame's
       render — waited on at the START of the next render, not right after
       submitting this one (see render_done below for how the compositor
       knows the texture is ready without the CPU blocking on this fence). */
    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    if (vkCreateFence(inst->vk_device, &fence_ci, NULL, &vp->render_fence) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkCreateFence failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    /* Semaphore for the GPU-side handoff to swapchain compositing — see
       ca_viewport_render_all's doc comment. */
    VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(inst->vk_device, &sem_ci, NULL, &vp->render_done) != VK_SUCCESS) {
        fprintf(stderr, "[viewport] vkCreateSemaphore failed\n");
        ca_viewport_gpu_destroy(inst, vp);
        return false;
    }

    return true;
}

void ca_viewport_gpu_destroy(Ca_Instance *inst, Ca_Viewport *vp)
{
    if (!vp) return;
    vkDeviceWaitIdle(inst->vk_device);

    if (vp->render_fence != VK_NULL_HANDLE) {
        vkDestroyFence(inst->vk_device, vp->render_fence, NULL);
        vp->render_fence = VK_NULL_HANDLE;
    }
    if (vp->render_done != VK_NULL_HANDLE) {
        vkDestroySemaphore(inst->vk_device, vp->render_done, NULL);
        vp->render_done = VK_NULL_HANDLE;
    }
    if (vp->cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &vp->cmd);
        vp->cmd = VK_NULL_HANDLE;
    }
    if (vp->desc_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(inst->vk_device, inst->image_desc_pool,
                             1, &vp->desc_set);
        vp->desc_set = VK_NULL_HANDLE;
    }
    if (vp->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(inst->vk_device, vp->sampler, NULL);
        vp->sampler = VK_NULL_HANDLE;
    }
    if (vp->color_view != VK_NULL_HANDLE) {
        vkDestroyImageView(inst->vk_device, vp->color_view, NULL);
        vp->color_view = VK_NULL_HANDLE;
    }
    if (vp->color_image != VK_NULL_HANDLE) {
        vkDestroyImage(inst->vk_device, vp->color_image, NULL);
        vp->color_image = VK_NULL_HANDLE;
    }
    if (vp->color_memory != VK_NULL_HANDLE) {
        vkFreeMemory(inst->vk_device, vp->color_memory, NULL);
        vp->color_memory = VK_NULL_HANDLE;
    }

    vp->width  = 0;
    vp->height = 0;
}

bool ca_viewport_gpu_resize(Ca_Instance *inst, Ca_Viewport *vp,
                            uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return true;
    if (vp->width == width && vp->height == height) return true;

    VkFormat fmt = vp->format;
    ca_viewport_gpu_destroy(inst, vp);
    return ca_viewport_gpu_create(inst, vp, width, height, fmt);
}

/* ---- Per-frame viewport rendering ---- */

static void transition_viewport_image(VkCommandBuffer cmd, VkImage image,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout,
                                       VkPipelineStageFlags2 src_stage,
                                       VkAccessFlags2 src_access,
                                       VkPipelineStageFlags2 dst_stage,
                                       VkAccessFlags2 dst_access)
{
    VkImageMemoryBarrier2 barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src_stage,
        .srcAccessMask       = src_access,
        .dstStageMask        = dst_stage,
        .dstAccessMask       = dst_access,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

void ca_viewport_render_all(Ca_Instance *inst, Ca_Window *win,
                            VkSemaphore *out_semaphores, uint32_t *out_count)
{
    *out_count = 0;
    if (!win->viewport_pool) return;

    for (uint32_t i = 0; i < CA_MAX_VIEWPORTS_PER_WINDOW; ++i) {
        Ca_Viewport *vp = &win->viewport_pool[i];
        if (!vp->in_use || !vp->on_render) continue;
        if (vp->color_image == VK_NULL_HANDLE) continue;
        if (!vp->needs_redraw) continue;

        /* Check if layout changed the node size — resize if needed */
        if (vp->node) {
            float content_scale = 1.0f;
            if (win->glfw)
                glfwGetWindowContentScale(win->glfw, &content_scale, NULL);

            uint32_t new_w = (uint32_t)(vp->node->w * content_scale);
            uint32_t new_h = (uint32_t)(vp->node->h * content_scale);
            if (new_w < 1) new_w = 1;
            if (new_h < 1) new_h = 1;

            if (new_w != vp->width || new_h != vp->height) {
                ca_viewport_gpu_resize(inst, vp, new_w, new_h);
                vp->needs_redraw = true;
                if (vp->on_resize)
                    vp->on_resize(vp, new_w, new_h, vp->resize_data);
            }
        }

        /* Wait for previous frame's render to complete */
        vkWaitForFences(inst->vk_device, 1, &vp->render_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(inst->vk_device, 1, &vp->render_fence);

        /* Begin command buffer */
        vkResetCommandBuffer(vp->cmd, 0);
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(vp->cmd, &begin);

        /* Transition to COLOR_ATTACHMENT_OPTIMAL for engine rendering */
        transition_viewport_image(vp->cmd, vp->color_image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        /* Let the engine render */
        vp->on_render(vp, vp->render_data);

        vp->needs_redraw = false;
        if (vp->node)
            vp->node->dirty |= CA_DIRTY_CONTENT;

        /* Transition to SHADER_READ_ONLY for compositing */
        transition_viewport_image(vp->cmd, vp->color_image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);

        vkEndCommandBuffer(vp->cmd);

        /* Submit asynchronously: render_fence is waited on at the START of
           this viewport's NEXT render (line ~285 above), reclaiming vp->cmd
           only once the GPU is actually done with it, not here. render_done
           is what tells the swapchain compositing submit — a separate
           command buffer, submitted after this function returns — that the
           texture is safe to sample, entirely at the GPU level, so the CPU
           never blocks waiting for this viewport's render to finish. */
        VkSubmitInfo submit = {
            .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount   = 1,
            .pCommandBuffers      = &vp->cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores    = &vp->render_done,
        };
        vkQueueSubmit(inst->gfx_queue, 1, &submit, vp->render_fence);
        vp->has_rendered_once = true;

        if (*out_count < CA_MAX_VIEWPORTS_PER_WINDOW)
            out_semaphores[(*out_count)++] = vp->render_done;
    }
}
