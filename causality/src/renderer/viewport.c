// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* viewport.c — offscreen render target for external renderers */
#include "viewport.h"
#include "image.h"
#include <string.h>
#include <stdio.h>

/* ---- GPU resource lifecycle ---- */

bool ca_viewport_gpu_create(Ca_Instance *inst, Ca_Viewport *vp,
                            uint32_t width, uint32_t height, VkFormat format)
{
    if (width == 0 || height == 0) return false;

    vp->width  = width;
    vp->height = height;
    vp->format = format;
    vp->frame_index = 0;
    vp->last_rendered_frame = 0;

    /* Sampler (linear, clamp — for compositing into UI). Stateless, shared
       across all frame-in-flight slots — created once, outside the per-slot
       loop below. */
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
        return false;
    }

    for (uint32_t fi = 0; fi < CA_FRAMES_IN_FLIGHT; fi++) {
        Ca_ViewportFrame *f = &vp->frame[fi];
        f->has_rendered_once = false;

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
        if (vkCreateImage(inst->vk_device, &img_ci, NULL, &f->color_image) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkCreateImage failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(inst->vk_device, f->color_image, &req);
        uint32_t mem_idx = ca_gpu_find_memory_type(inst, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_idx == UINT32_MAX) {
            fprintf(stderr, "[viewport] no suitable memory type\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }
        VkMemoryAllocateInfo mem_ai = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = req.size,
            .memoryTypeIndex = mem_idx,
        };
        if (vkAllocateMemory(inst->vk_device, &mem_ai, NULL, &f->color_memory) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkAllocateMemory failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }
        if (vkBindImageMemory(inst->vk_device, f->color_image, f->color_memory, 0) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkBindImageMemory failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }

        /* Image view */
        VkImageViewCreateInfo view_ci = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = f->color_image,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (vkCreateImageView(inst->vk_device, &view_ci, NULL, &f->color_view) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkCreateImageView failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }

        /* Descriptor set — reuses the text pipeline's descriptor set layout
           for combined image sampler at binding 0. One set per frame-in-
           flight slot, written once here (matches the codebase's write-once
           descriptor convention — see Quasar's rg_pbr_node.c for the same
           pattern), since each slot's color_view is a distinct VkImageView
           for the lifetime of these GPU resources. */
        if (!ca_image_descriptor_allocate(inst,
                                          inst->text_pipeline.desc_layout,
                                          &f->desc_set,
                                          &f->desc_pool)) {
            fprintf(stderr, "[viewport] descriptor set alloc failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }

        VkDescriptorImageInfo img_info = {
            .sampler     = vp->sampler,
            .imageView   = f->color_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wr = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = f->desc_set,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &img_info,
        };
        vkUpdateDescriptorSets(inst->vk_device, 1, &wr, 0, NULL);

        /* Per-slot command buffer */
        VkCommandBufferAllocateInfo cmd_ai = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = inst->cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(inst->vk_device, &cmd_ai, &f->cmd) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkAllocateCommandBuffers failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }

        /* Fence for reclaiming this slot's cmd once the GPU is done with its
           last render — waited on at the START of this slot's next render,
           not right after submitting this one (see render_done below for
           how the compositor knows the texture is ready without the CPU
           blocking on this fence). */
        VkFenceCreateInfo fence_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        if (vkCreateFence(inst->vk_device, &fence_ci, NULL, &f->render_fence) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkCreateFence failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }

        /* Semaphore for the GPU-side handoff to swapchain compositing — see
           ca_viewport_render_all's doc comment. */
        VkSemaphoreCreateInfo sem_ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(inst->vk_device, &sem_ci, NULL, &f->render_done) != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkCreateSemaphore failed\n");
            ca_viewport_gpu_destroy(inst, vp);
            return false;
        }
    }

    return true;
}

void ca_viewport_gpu_destroy(Ca_Instance *inst, Ca_Viewport *vp)
{
    if (!vp) return;

    /* Wait only on this viewport's own in-flight submissions rather than
       the whole device (vkDeviceWaitIdle) — ca_viewport_gpu_resize calls
       this from the per-frame render loop, so stalling every other window's
       and viewport's GPU work here on every resize is a real, easily
       user-triggered hitch (e.g. dragging to resize a panel). Each slot's
       fence is CA_FENCE_CREATE_SIGNALED_BIT at creation and reset only
       right before that slot's own submit, so waiting on it here is safe
       even for a slot that never rendered. */
    for (uint32_t fi = 0; fi < CA_FRAMES_IN_FLIGHT; fi++) {
        Ca_ViewportFrame *f = &vp->frame[fi];
        if (f->render_fence != VK_NULL_HANDLE)
            vkWaitForFences(inst->vk_device, 1, &f->render_fence, VK_TRUE, UINT64_MAX);
    }

    for (uint32_t fi = 0; fi < CA_FRAMES_IN_FLIGHT; fi++) {
        Ca_ViewportFrame *f = &vp->frame[fi];
        if (f->render_fence != VK_NULL_HANDLE) {
            vkDestroyFence(inst->vk_device, f->render_fence, NULL);
            f->render_fence = VK_NULL_HANDLE;
        }
        if (f->render_done != VK_NULL_HANDLE) {
            vkDestroySemaphore(inst->vk_device, f->render_done, NULL);
            f->render_done = VK_NULL_HANDLE;
        }
        if (f->cmd != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &f->cmd);
            f->cmd = VK_NULL_HANDLE;
        }
        if (f->desc_set != VK_NULL_HANDLE) {
            ca_image_descriptor_free(inst, f->desc_pool, f->desc_set);
            f->desc_set = VK_NULL_HANDLE;
            f->desc_pool = VK_NULL_HANDLE;
        }
        if (f->color_view != VK_NULL_HANDLE) {
            vkDestroyImageView(inst->vk_device, f->color_view, NULL);
            f->color_view = VK_NULL_HANDLE;
        }
        if (f->color_image != VK_NULL_HANDLE) {
            vkDestroyImage(inst->vk_device, f->color_image, NULL);
            f->color_image = VK_NULL_HANDLE;
        }
        if (f->color_memory != VK_NULL_HANDLE) {
            vkFreeMemory(inst->vk_device, f->color_memory, NULL);
            f->color_memory = VK_NULL_HANDLE;
        }
        f->has_rendered_once = false;
    }

    if (vp->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(inst->vk_device, vp->sampler, NULL);
        vp->sampler = VK_NULL_HANDLE;
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
                            Ca_DynArray *out_semaphores)
{
    if (!out_semaphores) return;
    ca_dyn_array_clear(out_semaphores);
    if (ca_pool_slot_count(&win->viewport_pool) == 0) return;

    for (uint32_t i = 0; i < ca_pool_slot_count(&win->viewport_pool); ++i) {
        Ca_Viewport *vp = CA_POOL_AT(win->viewport_pool, Ca_Viewport, i);
        if (!vp->in_use || !vp->on_render) continue;
        /* fi is captured BEFORE resize/advance below: it names the slot this
           call will render into. ca_viewport_frame_index() (called from
           inside vp->on_render, via qs_gpu.c's trampoline) must observe this
           same value, which holds as long as frame_index is only advanced
           after on_render returns (see the end of this loop body). */
        uint32_t fi = vp->frame_index;
        Ca_ViewportFrame *f = &vp->frame[fi];
        if (f->color_image == VK_NULL_HANDLE) continue;
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
                /* Resize destroys+recreates every slot (ca_viewport_gpu_destroy
                   is a full vkDeviceWaitIdle teardown), so re-fetch f — the
                   old pointer is dangling and fi may now be past a reset
                   frame_index (ca_viewport_gpu_create sets it back to 0). */
                fi = vp->frame_index;
                f  = &vp->frame[fi];
                vp->needs_redraw = true;
                if (vp->on_resize)
                    vp->on_resize(vp, new_w, new_h, vp->resize_data);
            }
        }

        /* Wait for this slot's previous render to complete. In steady state
           this slot was last submitted CA_FRAMES_IN_FLIGHT frames ago rather
           than 1, so the fence is typically already signaled — this is the
           actual latency win double-buffering provides over the old
           single-fence design. */
        ca_profile_begin(inst, "Platform Viewport Fence");
        vkWaitForFences(inst->vk_device, 1, &f->render_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(inst->vk_device, 1, &f->render_fence);
        ca_profile_end(inst, "Platform Viewport Fence");

        /* Begin command buffer */
        vkResetCommandBuffer(f->cmd, 0);
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VkResult vr = vkBeginCommandBuffer(f->cmd, &begin);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkBeginCommandBuffer failed: %d\n", vr);
            /* render_fence was already reset above in anticipation of a
               submit that isn't happening this iteration — re-signal it
               with an empty submit so the next render of this slot doesn't
               hang waiting on a fence nothing will ever signal. */
            VkSubmitInfo empty_submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
            vkQueueSubmit(inst->gfx_queue, 1, &empty_submit, f->render_fence);
            continue;
        }

        /* Transition to COLOR_ATTACHMENT_OPTIMAL for engine rendering */
        transition_viewport_image(f->cmd, f->color_image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        /* Let the engine render */
        ca_profile_begin(inst, "Platform Viewport Render");
        vp->on_render(vp, vp->render_data);
        ca_profile_end(inst, "Platform Viewport Render");

        vp->needs_redraw = false;
        if (vp->node)
            vp->node->dirty |= CA_DIRTY_CONTENT;

        /* Transition to SHADER_READ_ONLY for compositing */
        transition_viewport_image(f->cmd, f->color_image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);

        vr = vkEndCommandBuffer(f->cmd);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkEndCommandBuffer failed: %d\n", vr);
            VkSubmitInfo empty_submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
            vkQueueSubmit(inst->gfx_queue, 1, &empty_submit, f->render_fence);
            /* on_render already ran above and needs_redraw was cleared, but
               this slot's image never actually reached the GPU — re-arm so
               the next call retries instead of leaving whatever partial/
               undefined content is in f->color_image to be sampled forever. */
            vp->needs_redraw = true;
            continue;
        }

        /* Submit asynchronously: render_fence is waited on at the START of
           this slot's NEXT render (line ~340 above), reclaiming f->cmd
           only once the GPU is actually done with it, not here. render_done
           is what tells the swapchain compositing submit — a separate
           command buffer, submitted after this function returns — that the
           texture is safe to sample, entirely at the GPU level, so the CPU
           never blocks waiting for this viewport's render to finish. */
        VkSubmitInfo submit = {
            .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount   = 1,
            .pCommandBuffers      = &f->cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores    = &f->render_done,
        };
        ca_profile_begin(inst, "Platform Viewport Submit");
        vr = vkQueueSubmit(inst->gfx_queue, 1, &submit, f->render_fence);
        ca_profile_end(inst, "Platform Viewport Submit");
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "[viewport] vkQueueSubmit failed: %d\n", vr);
            /* The real submit above didn't happen, so render_fence would
               otherwise never be signaled — same recovery as the other
               failure paths in this loop. render_done was never signaled
               either; skip pushing it below so the compositor never waits
               on a semaphore nothing will signal. */
            VkSubmitInfo empty_submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
            vkQueueSubmit(inst->gfx_queue, 1, &empty_submit, f->render_fence);
            vp->needs_redraw = true;
            vp->frame_index = (fi + 1) % CA_FRAMES_IN_FLIGHT;
            continue;
        }
        f->has_rendered_once = true;

        /* Record which slot was actually just submitted — the compositor
           (swapchain.c) reads this, not frame_index, since frame_index is
           about to advance to point at the NEXT slot below. */
        vp->last_rendered_frame = fi;

        /* Cycle to the next frame-in-flight slot, mirroring
           Ca_Swapchain.current_frame (swapchain.c). Must happen AFTER
           on_render/submit above — see fi's capture comment at the top of
           this loop body. */
        vp->frame_index = (fi + 1) % CA_FRAMES_IN_FLIGHT;

        if (!ca_dyn_array_push(out_semaphores, &f->render_done)) {
            fprintf(stderr, "[viewport] unable to retain render semaphore\n");
            vkQueueWaitIdle(inst->gfx_queue);
            ca_dyn_array_clear(out_semaphores);
            return;
        }
    }
}
