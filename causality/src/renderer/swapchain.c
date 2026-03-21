#include "swapchain.h"
#include "renderer.h"
#include "pipeline.h"
#include "viewport.h"
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */

/* Compare draw commands by z_index for stable sort (preserves original order
   for commands with equal z_index by using index offset). */
static Ca_DrawCmd *s_sort_base; /* set before qsort */
static int cmp_z_cmd(const void *a, const void *b)
{
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;
    int za = s_sort_base[ia].z_index;
    int zb = s_sort_base[ib].z_index;
    if (za != zb) return (za < zb) ? -1 : 1;
    return (ia < ib) ? -1 : (ia > ib) ? 1 : 0; /* stable: by original index */
}

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
        if (formats[i].format     == VK_FORMAT_B8G8R8A8_UNORM &&
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

    inst->present_mode = mode;

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

    /* Per-frame sync + command buffers + instance buffers */
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

        /* Instance buffer for instanced rendering (created if SSBO layout exists) */
        if (inst->ssbo_desc_layout != VK_NULL_HANDLE) {
            if (!ca_instance_buf_create(inst, f)) {
                fprintf(stderr, "[vk] instance buffer creation failed for frame %u\n", i);
                return false;
            }
        }
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
        ca_instance_buf_destroy(inst, f);
        vkDestroySemaphore(inst->vk_device, f->image_available, NULL);
        vkDestroySemaphore(inst->vk_device, f->render_finished, NULL);
        vkDestroyFence(inst->vk_device, f->in_flight, NULL);
        f->image_available = VK_NULL_HANDLE;
        f->render_finished = VK_NULL_HANDLE;
        f->in_flight       = VK_NULL_HANDLE;
        if (f->cmd != VK_NULL_HANDLE)
            vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &f->cmd);
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

    /* Render all active viewports before compositing into the swapchain.
       This submits its own command buffers and waits synchronously. */
    ca_viewport_render_all(inst, win);

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

    /* --- Z-index: partition into z<0 | z==0 | z>0, sort only z!=0 --- */
    uint32_t *sorted_idx = NULL;
    {
        uint32_t count = win->draw_cmd_count;
        uint32_t n_neg = 0, n_pos = 0;
        for (uint32_t d = 1; d < count; ++d) {
            int z = win->draw_cmds[d].z_index;
            if (z < 0) n_neg++;
            else if (z > 0) n_pos++;
        }
        if ((n_neg | n_pos) && count > 1) {
            sorted_idx = win->sorted_idx;
            sorted_idx[0] = 0;
            uint32_t n_zero  = count - 1 - n_neg - n_pos;
            uint32_t ni = 1, zi = 1 + n_neg, pi = 1 + n_neg + n_zero;
            for (uint32_t d = 1; d < count; ++d) {
                int z = win->draw_cmds[d].z_index;
                if (z < 0)       sorted_idx[ni++] = d;
                else if (z == 0) sorted_idx[zi++] = d;
                else             sorted_idx[pi++] = d;
            }
            s_sort_base = win->draw_cmds;
            if (n_neg > 1)
                qsort(sorted_idx + 1, n_neg, sizeof(uint32_t), cmp_z_cmd);
            if (n_pos > 1)
                qsort(sorted_idx + 1 + n_neg + n_zero, n_pos,
                      sizeof(uint32_t), cmp_z_cmd);
        }
    }

    /* Helper: convert logical clip rect to physical scissor rect */
    int log_w, log_h;
    glfwGetWindowSize(win->glfw, &log_w, &log_h);
    float scale_x = (log_w > 0) ? (float)sc->extent.width  / (float)log_w : 1.0f;
    float scale_y = (log_h > 0) ? (float)sc->extent.height / (float)log_h : 1.0f;
    VkRect2D full_scissor = { .offset = {0, 0}, .extent = sc->extent };

    /* ================================================================
       Instanced rendering with scissor-aware batching.
       Instance data is packed into the per-frame SSBO:
         Region 0 (offset 0)           : Ca_RectPushConst instances
         Region 1 (aligned after rects): Ca_TextInstance (text + images)
       Within each section, draws are batched and flushed on scissor change.
       ================================================================ */

#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((uint32_t)(align) - 1))

    /* Text/image region starts after the maximum possible rect region.
       Use draw_cmd_count as upper bound (all cmds could be rects). */
    uint32_t text_byte_off = ALIGN_UP(
        win->draw_cmd_count * (uint32_t)sizeof(Ca_RectPushConst),
        inst->min_ssbo_align);

    Ca_RectPushConst *rect_base = (Ca_RectPushConst *)f->instance_mapped;
    Ca_TextInstance  *ti_base   =
        (Ca_TextInstance *)((char *)f->instance_mapped + text_byte_off);
    uint32_t          rect_n   = 0;   /* total rect instances written */
    uint32_t          ti_n     = 0;   /* total text+image instances written */
    uint32_t          batch_n  = 0;   /* total vkCmdDraw calls (batches) */

    VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width  = (float)sc->extent.width,
        .height = (float)sc->extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };

    for (int phase = 0; phase < 2; ++phase) {
        const bool want_overlay = (phase == 1);

        /* ---- Rects ---- */
        if (inst->rect_pipeline.pipeline != VK_NULL_HANDLE && win->draw_cmd_count > 1) {
            vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              inst->rect_pipeline.pipeline);
            vkCmdSetViewport(f->cmd, 0, 1, &viewport);

            uint32_t dyn_off = 0;
            vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    inst->rect_pipeline.layout,
                                    0, 1, &f->ssbo_set, 1, &dyn_off);

            uint32_t batch_start = rect_n;
            VkRect2D cur_sc      = full_scissor;
            bool     first       = true;

            for (uint32_t d = 1; d < win->draw_cmd_count; ++d) {
                uint32_t idx = sorted_idx ? sorted_idx[d] : d;
                const Ca_DrawCmd *cmd = &win->draw_cmds[idx];
                if (!cmd->in_use || cmd->type != CA_DRAW_RECT || cmd->a < 0.004f)
                    continue;
                if (cmd->overlay != want_overlay) continue;

                /* Compute scissor for this command */
                VkRect2D sc_new = full_scissor;
                if (cmd->has_clip) {
                    int32_t cx = (int32_t)(cmd->clip_x * scale_x);
                    int32_t cy = (int32_t)(cmd->clip_y * scale_y);
                    int32_t cw = (int32_t)(cmd->clip_w * scale_x);
                    int32_t ch = (int32_t)(cmd->clip_h * scale_y);
                    if (cx < 0) { cw += cx; cx = 0; }
                    if (cy < 0) { ch += cy; cy = 0; }
                    if (cw < 0) cw = 0;
                    if (ch < 0) ch = 0;
                    sc_new = (VkRect2D){ .offset = {cx, cy},
                                         .extent = {(uint32_t)cw, (uint32_t)ch} };
                }

                /* Flush batch on scissor change */
                if (!first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0) {
                    if (rect_n > batch_start) {
                        vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                        vkCmdDraw(f->cmd, 6, rect_n - batch_start, 0, batch_start);
                        batch_n++;
                    }
                    batch_start = rect_n;
                }
                cur_sc = sc_new;
                first  = false;

                /* Pack instance data */
                Ca_RectPushConst *dst = &rect_base[rect_n++];
                dst->pos[0] = cmd->x;            dst->pos[1] = cmd->y;
                dst->size[0] = cmd->w;            dst->size[1] = cmd->h;
                dst->color[0] = cmd->r;           dst->color[1] = cmd->g;
                dst->color[2] = cmd->b;           dst->color[3] = cmd->a;
                dst->viewport[0] = (float)log_w;  dst->viewport[1] = (float)log_h;
                dst->corner_radius = cmd->corner_radius;
                dst->border_width  = cmd->border_width;
                dst->border_color[0] = cmd->border_r;
                dst->border_color[1] = cmd->border_g;
                dst->border_color[2] = cmd->border_b;
                dst->border_color[3] = cmd->border_a;
            }
            /* Flush final batch */
            if (rect_n > batch_start) {
                vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                vkCmdDraw(f->cmd, 6, rect_n - batch_start, 0, batch_start);
                batch_n++;
            }
        }

        /* ---- Text glyphs ---- */
        if (inst->text_pipeline.pipeline != VK_NULL_HANDLE && inst->font != NULL) {

            bool has_glyphs = false;
            for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                if (win->draw_cmds[d].in_use &&
                    win->draw_cmds[d].type == CA_DRAW_GLYPH &&
                    win->draw_cmds[d].overlay == want_overlay) {
                    has_glyphs = true;
                    break;
                }
            }

            if (has_glyphs) {
                vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  inst->text_pipeline.pipeline);
                vkCmdSetViewport(f->cmd, 0, 1, &viewport);

                /* Bind SSBO at set 0 with text region offset */
                vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        inst->text_pipeline.layout,
                                        0, 1, &f->ssbo_set, 1, &text_byte_off);
                /* Bind font atlas at set 1 */
                vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        inst->text_pipeline.layout,
                                        1, 1, &inst->text_pipeline.desc_set,
                                        0, NULL);

                uint32_t batch_start = ti_n;
                VkRect2D cur_sc      = full_scissor;
                bool     first       = true;

                for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                    uint32_t idx = sorted_idx ? sorted_idx[d] : d;
                    const Ca_DrawCmd *cmd = &win->draw_cmds[idx];
                    if (!cmd->in_use || cmd->type != CA_DRAW_GLYPH || cmd->a < 0.004f)
                        continue;
                    if (cmd->overlay != want_overlay) continue;

                    VkRect2D sc_new = full_scissor;
                    if (cmd->has_clip) {
                        int32_t cx = (int32_t)(cmd->clip_x * scale_x);
                        int32_t cy = (int32_t)(cmd->clip_y * scale_y);
                        int32_t cw = (int32_t)(cmd->clip_w * scale_x);
                        int32_t ch = (int32_t)(cmd->clip_h * scale_y);
                        if (cx < 0) { cw += cx; cx = 0; }
                        if (cy < 0) { ch += cy; cy = 0; }
                        if (cw < 0) cw = 0;
                        if (ch < 0) ch = 0;
                        sc_new = (VkRect2D){ .offset = {cx, cy},
                                             .extent = {(uint32_t)cw, (uint32_t)ch} };
                    }

                    if (!first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0) {
                        if (ti_n > batch_start) {
                            vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                            vkCmdDraw(f->cmd, 6, ti_n - batch_start, 0, batch_start);
                            batch_n++;
                        }
                        batch_start = ti_n;
                    }
                    cur_sc = sc_new;
                    first  = false;

                    Ca_TextInstance *dst = &ti_base[ti_n++];
                    dst->pos[0] = cmd->x;            dst->pos[1] = cmd->y;
                    dst->size[0] = cmd->w;            dst->size[1] = cmd->h;
                    dst->uv[0] = cmd->u0;             dst->uv[1] = cmd->v0;
                    dst->uv[2] = cmd->u1;             dst->uv[3] = cmd->v1;
                    dst->color[0] = cmd->r;            dst->color[1] = cmd->g;
                    dst->color[2] = cmd->b;            dst->color[3] = cmd->a;
                    dst->viewport[0] = (float)log_w;   dst->viewport[1] = (float)log_h;
                    dst->_pad[0] = 0.0f;               dst->_pad[1] = 0.0f;
                }
                if (ti_n > batch_start) {
                    vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                    vkCmdDraw(f->cmd, 6, ti_n - batch_start, 0, batch_start);
                    batch_n++;
                }
            }
        }

        /* ---- Images (RGBA textured quads) ---- */
        if (inst->image_pipeline != VK_NULL_HANDLE) {

            bool has_images = false;
            for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                if (win->draw_cmds[d].in_use &&
                    win->draw_cmds[d].type == CA_DRAW_IMAGE &&
                    win->draw_cmds[d].overlay == want_overlay) {
                    has_images = true;
                    break;
                }
            }
            if (has_images) {
                vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  inst->image_pipeline);
                vkCmdSetViewport(f->cmd, 0, 1, &viewport);

                /* Bind SSBO at set 0 with text/image region offset */
                vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        inst->text_pipeline.layout,
                                        0, 1, &f->ssbo_set, 1, &text_byte_off);

                uint32_t batch_start = ti_n;
                VkRect2D cur_sc      = full_scissor;
                int16_t  cur_img     = -1;
                bool     first       = true;

                for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                    uint32_t idx = sorted_idx ? sorted_idx[d] : d;
                    const Ca_DrawCmd *cmd = &win->draw_cmds[idx];
                    if (!cmd->in_use || cmd->type != CA_DRAW_IMAGE || cmd->a < 0.004f)
                        continue;
                    if (cmd->overlay != want_overlay) continue;

                    int16_t ii = cmd->image_index;
                    if (ii < 0 || ii >= CA_MAX_IMAGES || !inst->images[ii].in_use)
                        continue;

                    VkRect2D sc_new = full_scissor;
                    if (cmd->has_clip) {
                        int32_t cx = (int32_t)(cmd->clip_x * scale_x);
                        int32_t cy = (int32_t)(cmd->clip_y * scale_y);
                        int32_t cw = (int32_t)(cmd->clip_w * scale_x);
                        int32_t ch = (int32_t)(cmd->clip_h * scale_y);
                        if (cx < 0) { cw += cx; cx = 0; }
                        if (cy < 0) { ch += cy; cy = 0; }
                        if (cw < 0) cw = 0;
                        if (ch < 0) ch = 0;
                        sc_new = (VkRect2D){ .offset = {cx, cy},
                                             .extent = {(uint32_t)cw, (uint32_t)ch} };
                    }

                    bool sc_change  = !first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0;
                    bool img_change = (ii != cur_img);

                    if (sc_change || img_change) {
                        if (ti_n > batch_start) {
                            vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                            vkCmdDraw(f->cmd, 6, ti_n - batch_start, 0, batch_start);
                            batch_n++;
                        }
                        batch_start = ti_n;
                    }
                    if (img_change) {
                        /* Bind per-image sampler at set 1 */
                        vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                inst->text_pipeline.layout,
                                                1, 1, &inst->images[ii].desc_set,
                                                0, NULL);
                        cur_img = ii;
                    }
                    cur_sc = sc_new;
                    first  = false;

                    Ca_TextInstance *dst = &ti_base[ti_n++];
                    dst->pos[0] = cmd->x;            dst->pos[1] = cmd->y;
                    dst->size[0] = cmd->w;            dst->size[1] = cmd->h;
                    dst->uv[0] = cmd->u0;             dst->uv[1] = cmd->v0;
                    dst->uv[2] = cmd->u1;             dst->uv[3] = cmd->v1;
                    dst->color[0] = cmd->r;            dst->color[1] = cmd->g;
                    dst->color[2] = cmd->b;            dst->color[3] = cmd->a;
                    dst->viewport[0] = (float)log_w;   dst->viewport[1] = (float)log_h;
                    dst->_pad[0] = 0.0f;               dst->_pad[1] = 0.0f;
                }
                if (ti_n > batch_start) {
                    vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                    vkCmdDraw(f->cmd, 6, ti_n - batch_start, 0, batch_start);
                    batch_n++;
                }
            }
        }

        /* ---- Viewports (offscreen render targets composited as textured quads) ---- */
        if (inst->image_pipeline != VK_NULL_HANDLE && win->viewport_pool) {

            bool has_viewports = false;
            for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                if (win->draw_cmds[d].in_use &&
                    win->draw_cmds[d].type == CA_DRAW_VIEWPORT &&
                    win->draw_cmds[d].overlay == want_overlay) {
                    has_viewports = true;
                    break;
                }
            }
            if (has_viewports) {
                vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  inst->image_pipeline);
                vkCmdSetViewport(f->cmd, 0, 1, &viewport);

                /* Bind SSBO at set 0 with text/image region offset */
                vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        inst->text_pipeline.layout,
                                        0, 1, &f->ssbo_set, 1, &text_byte_off);

                uint32_t batch_start = ti_n;
                VkRect2D cur_sc      = full_scissor;
                int16_t  cur_vp_idx  = -1;
                bool     first       = true;

                for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
                    uint32_t idx = sorted_idx ? sorted_idx[d] : d;
                    const Ca_DrawCmd *cmd = &win->draw_cmds[idx];
                    if (!cmd->in_use || cmd->type != CA_DRAW_VIEWPORT || cmd->a < 0.004f)
                        continue;
                    if (cmd->overlay != want_overlay) continue;

                    int16_t vi = cmd->viewport_index;
                    if (vi < 0 || vi >= CA_MAX_VIEWPORTS_PER_WINDOW ||
                        !win->viewport_pool[vi].in_use ||
                        win->viewport_pool[vi].desc_set == VK_NULL_HANDLE)
                        continue;

                    VkRect2D sc_new = full_scissor;
                    if (cmd->has_clip) {
                        int32_t cx = (int32_t)(cmd->clip_x * scale_x);
                        int32_t cy = (int32_t)(cmd->clip_y * scale_y);
                        int32_t cw = (int32_t)(cmd->clip_w * scale_x);
                        int32_t ch = (int32_t)(cmd->clip_h * scale_y);
                        if (cx < 0) { cw += cx; cx = 0; }
                        if (cy < 0) { ch += cy; cy = 0; }
                        if (cw < 0) cw = 0;
                        if (ch < 0) ch = 0;
                        sc_new = (VkRect2D){ .offset = {cx, cy},
                                             .extent = {(uint32_t)cw, (uint32_t)ch} };
                    }

                    bool sc_change = !first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0;
                    bool vp_change = (vi != cur_vp_idx);

                    if (sc_change || vp_change) {
                        if (ti_n > batch_start) {
                            vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                            vkCmdDraw(f->cmd, 6, ti_n - batch_start, 0, batch_start);
                            batch_n++;
                        }
                        batch_start = ti_n;
                    }
                    if (vp_change) {
                        vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                inst->text_pipeline.layout,
                                                1, 1, &win->viewport_pool[vi].desc_set,
                                                0, NULL);
                        cur_vp_idx = vi;
                    }
                    cur_sc = sc_new;
                    first  = false;

                    Ca_TextInstance *dst = &ti_base[ti_n++];
                    dst->pos[0] = cmd->x;            dst->pos[1] = cmd->y;
                    dst->size[0] = cmd->w;            dst->size[1] = cmd->h;
                    dst->uv[0] = cmd->u0;             dst->uv[1] = cmd->v0;
                    dst->uv[2] = cmd->u1;             dst->uv[3] = cmd->v1;
                    dst->color[0] = cmd->r;            dst->color[1] = cmd->g;
                    dst->color[2] = cmd->b;            dst->color[3] = cmd->a;
                    dst->viewport[0] = (float)log_w;   dst->viewport[1] = (float)log_h;
                    dst->_pad[0] = 0.0f;               dst->_pad[1] = 0.0f;
                }
                if (ti_n > batch_start) {
                    vkCmdSetScissor(f->cmd, 0, 1, &cur_sc);
                    vkCmdDraw(f->cmd, 6, ti_n - batch_start, 0, batch_start);
                    batch_n++;
                }
            }
        }
    } /* end phase loop */

#undef ALIGN_UP

    /* Store debug stats for the overlay */
    win->dbg_frames_rendered++;
    win->dbg_draw_cmds       = win->draw_cmd_count;
    win->dbg_rect_instances  = rect_n;
    win->dbg_ti_instances    = ti_n;
    win->dbg_batches         = batch_n;

    /* Frame timing for FPS / frame-time display */
    {
        double now = glfwGetTime();
        win->dbg_fps_frames++;
        double elapsed = now - win->dbg_fps_last_time;
        if (elapsed >= 1.0) {
            win->dbg_fps = (double)win->dbg_fps_frames / elapsed;
            win->dbg_fps_frames = 0;
            win->dbg_fps_last_time = now;
        }
        /* Per-frame time: measure from previous frame end */
        static double prev_time = 0;
        if (prev_time > 0)
            win->dbg_frame_time_ms = (now - prev_time) * 1000.0;
        prev_time = now;
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
