#include "swapchain.h"
#include "renderer.h"
#include "pipeline.h"

/* ---- Helpers ---- */

static VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice gpu,
                                                 VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, NULL);
    VkSurfaceFormatKHR *formats =
        (VkSurfaceFormatKHR *)malloc(count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats);

    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format     == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            break;
        }
    }
    free(formats);
    return chosen;
}

static VkPresentModeKHR choose_present_mode(VkPhysicalDevice gpu,
                                             VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, NULL);
    VkPresentModeKHR *modes =
        (VkPresentModeKHR *)malloc(count * sizeof(VkPresentModeKHR));
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, modes);

    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR; /* guaranteed */
    for (uint32_t i = 0; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) { chosen = modes[i]; break; }
    }
    free(modes);
    return chosen;
}

static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR *caps,
                                 uint32_t w, uint32_t h)
{
    if (caps->currentExtent.width != UINT32_MAX)
        return caps->currentExtent;

    VkExtent2D e = { w, h };
    e.width  = e.width  < caps->minImageExtent.width  ? caps->minImageExtent.width  :
               e.width  > caps->maxImageExtent.width  ? caps->maxImageExtent.width  : e.width;
    e.height = e.height < caps->minImageExtent.height ? caps->minImageExtent.height :
               e.height > caps->maxImageExtent.height ? caps->maxImageExtent.height : e.height;
    return e;
}

/* ---- Public ---- */

bool ca_swapchain_create(Ca_Instance *inst, Ca_Window *win,
                         uint32_t width, uint32_t height)
{
    Ca_Swapchain *sc = &win->sc;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(inst->vk_gpu, win->surface, &caps);

    VkSurfaceFormatKHR fmt  = choose_surface_format(inst->vk_gpu, win->surface);
    VkPresentModeKHR   mode = choose_present_mode(inst->vk_gpu, win->surface);
    VkExtent2D         ext  = choose_extent(&caps, width, height);

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;
    if (img_count > CA_MAX_SWAPCHAIN_IMAGES)
        img_count = CA_MAX_SWAPCHAIN_IMAGES;

    uint32_t queue_families[2] = { inst->gfx_family, inst->present_family };
    bool     same              = inst->gfx_family == inst->present_family;

    VkSwapchainCreateInfoKHR ci = {
        .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface               = win->surface,
        .minImageCount         = img_count,
        .imageFormat           = fmt.format,
        .imageColorSpace       = fmt.colorSpace,
        .imageExtent           = ext,
        .imageArrayLayers      = 1,
        .imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode      = same ? VK_SHARING_MODE_EXCLUSIVE
                                      : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = same ? 0 : 2,
        .pQueueFamilyIndices   = same ? NULL : queue_families,
        .preTransform          = caps.currentTransform,
        .compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode           = mode,
        .clipped               = VK_TRUE,
    };

    if (vkCreateSwapchainKHR(inst->vk_device, &ci, NULL, &sc->swapchain) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateSwapchainKHR failed\n");
        return false;
    }

    sc->format = fmt.format;
    sc->extent = ext;

    /* Retrieve images */
    vkGetSwapchainImagesKHR(inst->vk_device, sc->swapchain, &sc->image_count, NULL);
    vkGetSwapchainImagesKHR(inst->vk_device, sc->swapchain, &sc->image_count, sc->images);

    /* Image views */
    for (uint32_t i = 0; i < sc->image_count; ++i) {
        VkImageViewCreateInfo vci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = sc->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = sc->format,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        vkCreateImageView(inst->vk_device, &vci, NULL, &sc->image_views[i]);
    }

    /* Per-frame sync + command buffers */
    for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i) {
        Ca_Frame *f = &sc->frames[i];

        VkSemaphoreCreateInfo sem_ci = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
        };
        VkFenceCreateInfo fence_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        vkCreateSemaphore(inst->vk_device, &sem_ci, NULL, &f->image_available);
        vkCreateSemaphore(inst->vk_device, &sem_ci, NULL, &f->render_finished);
        vkCreateFence(inst->vk_device, &fence_ci, NULL, &f->in_flight);

        VkCommandBufferAllocateInfo alloc = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = inst->cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        vkAllocateCommandBuffers(inst->vk_device, &alloc, &f->cmd);
    }

    sc->current_frame = 0;
    printf("[vk] swapchain created (%ux%u, %u images)\n",
           ext.width, ext.height, sc->image_count);
    return true;
}

void ca_swapchain_destroy(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Swapchain *sc = &win->sc;
    if (sc->swapchain == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(inst->vk_device);

    for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i) {
        Ca_Frame *f = &sc->frames[i];
        vkDestroySemaphore(inst->vk_device, f->image_available, NULL);
        vkDestroySemaphore(inst->vk_device, f->render_finished, NULL);
        vkDestroyFence(inst->vk_device, f->in_flight, NULL);
        f->image_available = VK_NULL_HANDLE;
        f->render_finished = VK_NULL_HANDLE;
        f->in_flight       = VK_NULL_HANDLE;
        f->cmd             = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < sc->image_count; ++i) {
        vkDestroyImageView(inst->vk_device, sc->image_views[i], NULL);
        sc->image_views[i] = VK_NULL_HANDLE;
    }

    vkDestroySwapchainKHR(inst->vk_device, sc->swapchain, NULL);
    sc->swapchain   = VK_NULL_HANDLE;
    sc->image_count = 0;
}

/* ---- Image layout transition helper ---- */

static void transition_image(VkCommandBuffer cmd, VkImage image,
                              VkImageLayout old_layout, VkImageLayout new_layout,
                              VkPipelineStageFlags2 src_stage,
                              VkAccessFlags2        src_access,
                              VkPipelineStageFlags2 dst_stage,
                              VkAccessFlags2        dst_access)
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

/* ---- Frame ---- */

void ca_swapchain_frame(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Swapchain *sc = &win->sc;
    Ca_Frame     *f  = &sc->frames[sc->current_frame];

    vkWaitForFences(inst->vk_device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(
        inst->vk_device, sc->swapchain, UINT64_MAX,
        f->image_available, VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        int w, h;
        glfwGetFramebufferSize(win->glfw, &w, &h);
        ca_renderer_window_resize(inst, win, w, h);
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "[vk] vkAcquireNextImageKHR failed: %d\n", result);
        return;
    }

    vkResetFences(inst->vk_device, 1, &f->in_flight);
    vkResetCommandBuffer(f->cmd, 0);

    /* Record */
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(f->cmd, &begin);

    /* Transition to COLOR_ATTACHMENT_OPTIMAL */
    transition_image(f->cmd, sc->images[image_index],
        VK_IMAGE_LAYOUT_UNDEFINED,               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,     VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    /* Background: use the root node's color if available */
    float bg_r = 0.15f, bg_g = 0.15f, bg_b = 0.17f, bg_a = 1.0f;
    if (win->draw_cmd_count > 0 && win->draw_cmds[0].in_use) {
        bg_r = win->draw_cmds[0].r;
        bg_g = win->draw_cmds[0].g;
        bg_b = win->draw_cmds[0].b;
        bg_a = win->draw_cmds[0].a;
    }

    /* Dynamic rendering — background is the dynamic clear color */
    VkRenderingAttachmentInfo color_attach = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = sc->image_views[image_index],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = { bg_r, bg_g, bg_b, bg_a } } },
    };
    VkRenderingInfo render_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { .offset = {0, 0}, .extent = sc->extent },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_attach,
    };
    vkCmdBeginRendering(f->cmd, &render_info);

    /* Helper: convert logical clip rect to physical scissor rect */
    int log_w, log_h;
    glfwGetWindowSize(win->glfw, &log_w, &log_h);
    float scale_x = (log_w > 0) ? (float)sc->extent.width  / (float)log_w : 1.0f;
    float scale_y = (log_h > 0) ? (float)sc->extent.height / (float)log_h : 1.0f;
    VkRect2D full_scissor = { .offset = {0, 0}, .extent = sc->extent };

    /* Draw each widget rect using the proper graphics pipeline.
       Index 0 is the root background (already handled as the clear color).
       Nodes with alpha == 0 (e.g. transparent label placeholder) are skipped. */
    if (inst->rect_pipeline.pipeline != VK_NULL_HANDLE && win->draw_cmd_count > 1) {
        vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inst->rect_pipeline.pipeline);

        VkViewport viewport = {
            .x        = 0.0f, .y        = 0.0f,
            .width    = (float)sc->extent.width,
            .height   = (float)sc->extent.height,
            .minDepth = 0.0f, .maxDepth = 1.0f,
        };
        vkCmdSetViewport(f->cmd, 0, 1, &viewport);
        vkCmdSetScissor(f->cmd,  0, 1, &full_scissor);

        for (uint32_t d = 1; d < win->draw_cmd_count; ++d) {
            const Ca_DrawCmd *cmd = &win->draw_cmds[d];
            if (!cmd->in_use || cmd->type != CA_DRAW_RECT || cmd->a < 0.004f)
                continue;

            /* Set scissor rect for overflow clipping */
            if (cmd->has_clip) {
                int32_t cx = (int32_t)(cmd->clip_x * scale_x);
                int32_t cy = (int32_t)(cmd->clip_y * scale_y);
                int32_t cw = (int32_t)(cmd->clip_w * scale_x);
                int32_t ch = (int32_t)(cmd->clip_h * scale_y);
                if (cx < 0) { cw += cx; cx = 0; }
                if (cy < 0) { ch += cy; cy = 0; }
                if (cw < 0) cw = 0;
                if (ch < 0) ch = 0;
                VkRect2D clip = { .offset = {cx, cy},
                                  .extent = {(uint32_t)cw, (uint32_t)ch} };
                vkCmdSetScissor(f->cmd, 0, 1, &clip);
            } else {
                vkCmdSetScissor(f->cmd, 0, 1, &full_scissor);
            }

            /* Viewport in logical pixels so NDC matches layout coordinates */
            Ca_RectPushConst pc = {
                .pos      = { cmd->x, cmd->y },
                .size     = { cmd->w, cmd->h },
                .color    = { cmd->r, cmd->g, cmd->b, cmd->a },
                .viewport = { (float)log_w, (float)log_h },
            };
            vkCmdPushConstants(f->cmd, inst->rect_pipeline.layout,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(Ca_RectPushConst), &pc);
            vkCmdDraw(f->cmd, 6, 1, 0, 0);
        }
    }

    /* ---- Text glyphs ---- */
    if (inst->text_pipeline.pipeline != VK_NULL_HANDLE &&
        inst->font != NULL) {

        /* Check whether any glyph commands are queued */
        bool has_glyphs = false;
        for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
            if (win->draw_cmds[d].in_use &&
                win->draw_cmds[d].type == CA_DRAW_GLYPH) {
                has_glyphs = true;
                break;
            }
        }

        if (has_glyphs) {
            vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              inst->text_pipeline.pipeline);

            VkViewport vp = {
                .x = 0.0f, .y = 0.0f,
                .width  = (float)sc->extent.width,
                .height = (float)sc->extent.height,
                .minDepth = 0.0f, .maxDepth = 1.0f,
            };
            vkCmdSetViewport(f->cmd, 0, 1, &vp);
            vkCmdSetScissor(f->cmd,  0, 1, &full_scissor);

            vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    inst->text_pipeline.layout,
                                    0, 1, &inst->text_pipeline.desc_set,
                                    0, NULL);

            for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                const Ca_DrawCmd *cmd = &win->draw_cmds[d];
                if (!cmd->in_use || cmd->type != CA_DRAW_GLYPH ||
                    cmd->a < 0.004f)
                    continue;

                /* Set scissor rect for overflow clipping */
                if (cmd->has_clip) {
                    int32_t cx = (int32_t)(cmd->clip_x * scale_x);
                    int32_t cy = (int32_t)(cmd->clip_y * scale_y);
                    int32_t cw = (int32_t)(cmd->clip_w * scale_x);
                    int32_t ch = (int32_t)(cmd->clip_h * scale_y);
                    if (cx < 0) { cw += cx; cx = 0; }
                    if (cy < 0) { ch += cy; cy = 0; }
                    if (cw < 0) cw = 0;
                    if (ch < 0) ch = 0;
                    VkRect2D clip = { .offset = {cx, cy},
                                      .extent = {(uint32_t)cw, (uint32_t)ch} };
                    vkCmdSetScissor(f->cmd, 0, 1, &clip);
                } else {
                    vkCmdSetScissor(f->cmd, 0, 1, &full_scissor);
                }

                Ca_TextPushConst pc = {
                    .pos      = { cmd->x, cmd->y },
                    .size     = { cmd->w, cmd->h },
                    .uv       = { cmd->u0, cmd->v0, cmd->u1, cmd->v1 },
                    .color    = { cmd->r, cmd->g, cmd->b, cmd->a },
                    .viewport = { (float)log_w, (float)log_h },
                };
                vkCmdPushConstants(f->cmd, inst->text_pipeline.layout,
                                   VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(Ca_TextPushConst), &pc);
                vkCmdDraw(f->cmd, 6, 1, 0, 0);
            }
        }
    }

    vkCmdEndRendering(f->cmd);

    /* Transition to PRESENT_SRC */
    transition_image(f->cmd, sc->images[image_index],
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,  VK_ACCESS_2_NONE);

    vkEndCommandBuffer(f->cmd);

    /* Submit */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1,
        .pWaitSemaphores      = &f->image_available,
        .pWaitDstStageMask    = &wait_stage,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &f->cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &f->render_finished,
    };
    vkQueueSubmit(inst->gfx_queue, 1, &submit, f->in_flight);

    /* Present */
    VkPresentInfoKHR present = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &f->render_finished,
        .swapchainCount     = 1,
        .pSwapchains        = &sc->swapchain,
        .pImageIndices      = &image_index,
    };
    result = vkQueuePresentKHR(inst->present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        int w, h;
        glfwGetFramebufferSize(win->glfw, &w, &h);
        ca_renderer_window_resize(inst, win, w, h);
    }

    sc->current_frame = (sc->current_frame + 1) % CA_FRAMES_IN_FLIGHT;
}
