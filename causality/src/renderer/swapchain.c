// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "swapchain.h"
#include "renderer.h"
#include "pipeline.h"
#include "viewport.h"
#include "blur.h"
#include "node.h"
#include <math.h>
#include <stdio.h>
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

/* Paint bands, drawn strictly in order 0..3. Draw commands are submitted
   batched by TYPE within each band (rects, then glyphs, then images, then
   viewports — see record_range; backdrop-filter elements split a band into
   separately recorded ranges, see record_band), so two
   commands of different types with the same z_index would otherwise not
   respect z_index = "absolute paint order" (documented in causality.h) if
   they were left in a single band together. Splitting into z<0 / z==0 / z>0
   bands — matching the sorted_idx partition already computed above — makes
   type-batching safe: within a band all commands share the same z bracket
   relative to every other band, so cross-type mis-ordering can only happen
   between commands of equal z_index, which paint_node_content() emits in
   tree order already (stable qsort). Band 3 is reserved for true overlay
   content — context menus, tooltips, dropdowns, modals — explicitly flagged
   by paint_overlays() in paint.c, which must always sit above every ordinary
   z-banded node regardless of that node's z_index. */
static int cmd_paint_band(const Ca_DrawCmd *cmd)
{
    if (cmd->overlay) return 3;
    if (cmd->z_index < 0) return 0;
    if (cmd->z_index > 0) return 2;
    return 1;
}

/**
 * Packs per-corner radii and the edge anti-aliasing scale into the image
 * instance's reserved SSBO payload.
 *
 * _pad1[0..3] carry the per-corner radii (GLSL corner_01/corner_23).
 * _pad1[4] carries edge_aa_scale (GLSL edge_aa_scale_pad.x) — physical
 * px per logical px, so IMAGE_FRAG_GLSL can size its rounded-corner AA
 * ramp to exactly one physical pixel at any content_scale (see the
 * matching FRAG_GLSL comment in pipeline.c for why a derivative-based
 * ramp is wrong here above 1x DPI).
 *
 * @param dst      Image instance receiving the radii and scale.
 * @param cmd      Draw command supplying per-corner or uniform fallback radii.
 * @param aa_scale Physical-px-per-logical-px for this window (content_scale).
 */
static void image_instance_pack_corner_radii(Ca_TextInstance *dst,
                                             const Ca_DrawCmd *cmd,
                                             float aa_scale)
{
    memset(dst->_pad1, 0, sizeof(dst->_pad1));
    bool has_per_corner = cmd->corner_tl != 0.0f || cmd->corner_tr != 0.0f ||
                          cmd->corner_br != 0.0f || cmd->corner_bl != 0.0f;
    dst->_pad1[0] = has_per_corner ? cmd->corner_tl : cmd->corner_radius;
    dst->_pad1[1] = has_per_corner ? cmd->corner_tr : cmd->corner_radius;
    dst->_pad1[2] = has_per_corner ? cmd->corner_br : cmd->corner_radius;
    dst->_pad1[3] = has_per_corner ? cmd->corner_bl : cmd->corner_radius;
    dst->_pad1[4] = aa_scale;
}

/* Locate the root node's own background-rect draw command for this frame.
   Used both as the swapchain clear color and to exclude it from the regular
   rect batch (it's implicitly drawn via VK_ATTACHMENT_LOAD_OP_CLEAR instead).

   This is NOT always win->draw_cmds[0]: paint_node_content() emits a
   CA_DRAW_BACKDROP_BLUR and/or a CA_DRAW_MODE_SHADOW rect BEFORE the plain
   background rect whenever the root node has backdrop_blur or shadow_color
   set, which would otherwise shift the background rect off index 0. The
   background rect is always emitted (unconditionally, right after those),
   so a short bounded scan from root->draw_cmd_idx finds it reliably instead
   of assuming a fixed position. Returns UINT32_MAX if not found (root
   hidden, or nothing painted yet). */
static uint32_t find_root_bg_cmd_index(const Ca_Window *win)
{
    if (!win->root || win->root->draw_cmd_idx < 0) return UINT32_MAX;
    uint32_t start = (uint32_t)win->root->draw_cmd_idx;
    for (uint32_t d = start; d < win->draw_cmd_count; ++d) {
        const Ca_DrawCmd *cmd = &win->draw_cmds[d];
        if (!cmd->in_use) continue;
        if (cmd->type == CA_DRAW_RECT && cmd->draw_mode != CA_DRAW_MODE_SHADOW &&
            cmd->draw_mode != CA_DRAW_MODE_GLOW)
            return d;
        if (cmd->type != CA_DRAW_BACKDROP_BLUR &&
            !(cmd->type == CA_DRAW_RECT && (cmd->draw_mode == CA_DRAW_MODE_SHADOW || cmd->draw_mode == CA_DRAW_MODE_GLOW)))
            break; /* not one of the root's own leading commands — give up */
    }
    return UINT32_MAX;
}

static VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice gpu,
                                                 VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, NULL);
    VkSurfaceFormatKHR *formats =
        (VkSurfaceFormatKHR *)CA_MALLOC(count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats);

    VkSurfaceFormatKHR chosen = formats[0];
    /* Prefer sRGB framebuffer: hardware linearises destination samples
       before blending and re-encodes on store, which gives gamma-correct
       blending — required by the LCD text path and the rect/image
       shaders that output linear colours.                              */
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format     == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            found = true;
            break;
        }
    }
    if (!found) {
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i].format     == VK_FORMAT_R8G8B8A8_SRGB &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = formats[i];
                break;
            }
        }
    }
    CA_FREE(formats);
    return chosen;
}

/**
 * Selects the presentation mode for a window surface.
 *
 * @param gpu Physical device associated with the surface.
 * @param surface Window surface whose swapchain is being configured.
 * @param disable_vsync When false (default), always FIFO — compositor-paced,
 *        no tearing, guaranteed supported by every Vulkan implementation.
 *        When true, prefers MAILBOX (triple-buffered, no tearing, submits at
 *        the GPU's max rate) and falls back to IMMEDIATE (may tear) or FIFO
 *        if neither is supported by this surface.
 * @return The selected presentation mode.
 */
static VkPresentModeKHR choose_present_mode(VkPhysicalDevice gpu,
                                            VkSurfaceKHR surface,
                                            bool disable_vsync)
{
    if (!disable_vsync)
        return VK_PRESENT_MODE_FIFO_KHR;

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, NULL);
    if (count == 0) return VK_PRESENT_MODE_FIFO_KHR;

    Ca_DynArray mode_storage = CA_DYN_ARRAY_INIT(VkPresentModeKHR);
    if (!ca_dyn_array_resize(&mode_storage, count))
        return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR *modes = mode_storage.data;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count,
                                                   modes) != VK_SUCCESS) {
        ca_dyn_array_destroy(&mode_storage);
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    bool has_mailbox = false, has_immediate = false;
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)   has_mailbox   = true;
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) has_immediate = true;
    }
    ca_dyn_array_destroy(&mode_storage);
    if (has_mailbox) return VK_PRESENT_MODE_MAILBOX_KHR;
    if (has_immediate) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
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
    VkPresentModeKHR   mode = choose_present_mode(inst->vk_gpu, win->surface, inst->disable_vsync);
    VkExtent2D         ext  = choose_extent(&caps, width, height);

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;

    uint32_t queue_families[2] = { inst->gfx_family, inst->present_family };
    bool     same              = inst->gfx_family == inst->present_family;

    VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT)
        image_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    VkSwapchainCreateInfoKHR ci = {
        .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface               = win->surface,
        .minImageCount         = img_count,
        .imageFormat           = fmt.format,
        .imageColorSpace       = fmt.colorSpace,
        .imageExtent           = ext,
        .imageArrayLayers      = 1,
        .imageUsage            = image_usage,
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
    sc->image_usage = image_usage;
    sc->extent = ext;

    if (sc->image_storage.element_size == 0) {
        sc->image_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(VkImage);
        sc->image_view_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(VkImageView);
        sc->image_render_finished_storage =
            (Ca_DynArray)CA_DYN_ARRAY_INIT(VkSemaphore);
        sc->viewport_wait_storage =
            (Ca_DynArray)CA_DYN_ARRAY_INIT(VkSemaphore);
        sc->submit_wait_storage =
            (Ca_DynArray)CA_DYN_ARRAY_INIT(VkSemaphore);
        sc->submit_stage_storage =
            (Ca_DynArray)CA_DYN_ARRAY_INIT(VkPipelineStageFlags);
        sc->retired_viewport_storage =
            (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_ViewportRetired);
    }

    uint32_t actual_image_count = 0;
    if (vkGetSwapchainImagesKHR(inst->vk_device, sc->swapchain,
                                &actual_image_count, NULL) != VK_SUCCESS ||
        actual_image_count == 0 ||
        !ca_dyn_array_resize(&sc->image_storage, actual_image_count) ||
        !ca_dyn_array_resize(&sc->image_view_storage, actual_image_count) ||
        !ca_dyn_array_resize(&sc->image_render_finished_storage,
                             actual_image_count)) {
        fprintf(stderr, "[vk] unable to allocate swapchain image storage\n");
        ca_swapchain_destroy(inst, win);
        return false;
    }
    sc->images = sc->image_storage.data;
    sc->image_views = sc->image_view_storage.data;
    sc->image_render_finished = sc->image_render_finished_storage.data;
    sc->image_count = actual_image_count;
    if (vkGetSwapchainImagesKHR(inst->vk_device, sc->swapchain,
                                &sc->image_count, sc->images) != VK_SUCCESS) {
        fprintf(stderr, "[vk] unable to retrieve swapchain images\n");
        ca_swapchain_destroy(inst, win);
        return false;
    }

    /* Image views + presentation semaphores. Render-finished semaphores are
       indexed by acquired swapchain image because presentation may retain them
       until that specific image is acquired again. */
    VkSemaphoreCreateInfo sem_ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
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
        if (vkCreateImageView(inst->vk_device, &vci, NULL,
                              &sc->image_views[i]) != VK_SUCCESS ||
            vkCreateSemaphore(inst->vk_device, &sem_ci, NULL,
                              &sc->image_render_finished[i]) != VK_SUCCESS) {
            fprintf(stderr, "[vk] swapchain image resource creation failed\n");
            ca_swapchain_destroy(inst, win);
            return false;
        }
    }

    /* Per-frame sync + command buffers + instance buffers */
    for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i) {
        Ca_Frame *f = &sc->frames[i];

        VkFenceCreateInfo fence_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        if (vkCreateSemaphore(inst->vk_device, &sem_ci, NULL,
                              &f->image_available) != VK_SUCCESS ||
            vkCreateFence(inst->vk_device, &fence_ci, NULL,
                          &f->in_flight) != VK_SUCCESS) {
            ca_swapchain_destroy(inst, win);
            return false;
        }

        VkCommandBufferAllocateInfo alloc = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = inst->cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(inst->vk_device, &alloc, &f->cmd) !=
            VK_SUCCESS) {
            ca_swapchain_destroy(inst, win);
            return false;
        }

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
    ca_viewport_collect_retired(inst, win, true);

    for (uint32_t i = 0; i < CA_FRAMES_IN_FLIGHT; ++i) {
        Ca_Frame *f = &sc->frames[i];
        ca_instance_buf_destroy(inst, f);
        vkDestroySemaphore(inst->vk_device, f->image_available, NULL);
        vkDestroyFence(inst->vk_device, f->in_flight, NULL);
        f->image_available = VK_NULL_HANDLE;
        f->in_flight       = VK_NULL_HANDLE;
        if (f->cmd != VK_NULL_HANDLE)
            vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &f->cmd);
        f->cmd             = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < sc->image_count; ++i) {
        vkDestroySemaphore(inst->vk_device, sc->image_render_finished[i], NULL);
        vkDestroyImageView(inst->vk_device, sc->image_views[i], NULL);
        sc->image_render_finished[i] = VK_NULL_HANDLE;
        sc->image_views[i] = VK_NULL_HANDLE;
    }

    vkDestroySwapchainKHR(inst->vk_device, sc->swapchain, NULL);
    sc->swapchain   = VK_NULL_HANDLE;
    sc->image_count = 0;
    sc->images = NULL;
    sc->image_views = NULL;
    sc->image_render_finished = NULL;
    ca_dyn_array_destroy(&sc->image_storage);
    ca_dyn_array_destroy(&sc->image_view_storage);
    ca_dyn_array_destroy(&sc->image_render_finished_storage);
    ca_dyn_array_destroy(&sc->viewport_wait_storage);
    ca_dyn_array_destroy(&sc->submit_wait_storage);
    ca_dyn_array_destroy(&sc->submit_stage_storage);
    ca_dyn_array_destroy(&sc->retired_viewport_storage);
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

/* Per-frame recording state shared by the paint-order range recorders
   below. Positions are indices into paint order: sorted_idx when z-sorting
   is active, raw draw_cmds order otherwise. */
typedef struct {
    Ca_Instance               *inst;
    Ca_Window                 *win;
    Ca_Frame                  *f;
    const uint32_t            *sorted_idx;
    uint32_t                   root_bg_idx;
    float                      scale_x, scale_y;
    int                        log_w, log_h;
    VkExtent2D                 extent;
    VkRect2D                   full_scissor;
    VkViewport                 viewport;
    VkImage                    swapchain_image;
    VkRenderingAttachmentInfo  reload_attach;
    bool                       can_blur;
    Ca_RectPushConst          *rect_base;
    Ca_TextInstance           *ti_base;
    uint32_t                   rect_n;   /* rect instances written        */
    uint32_t                   ti_n;     /* text/image instances written  */
    uint32_t                   batch_n;  /* vkCmdDraw calls recorded      */
} PaintCtx;

enum {
    RANGE_HAS_RECT     = 1u << 0,
    RANGE_HAS_GLYPH    = 1u << 1,
    RANGE_HAS_IMAGE    = 1u << 2,
    RANGE_HAS_VIEWPORT = 1u << 3,
};

/**
 * Maps a paint-order position to its draw command index.
 *
 * @param ctx Frame recording state.
 * @param pos Paint-order position.
 */
static uint32_t paint_index(const PaintCtx *ctx, uint32_t pos)
{
    return ctx->sorted_idx ? ctx->sorted_idx[pos] : pos;
}

/**
 * Converts a command's logical clip rect to a physical scissor.
 *
 * @param ctx       Frame recording state.
 * @param cmd       Draw command.
 * @param round_out Floor the origin and ceil the far edge (rect path: a
 *                  truncated scissor can shave the outermost column/row off
 *                  thin content hugging the clip edge at fractional scales);
 *                  false truncates, as the glyph/image paths always have.
 * @return The scissor, or the full framebuffer when the command is unclipped.
 */
static VkRect2D cmd_scissor(const PaintCtx *ctx, const Ca_DrawCmd *cmd,
                            bool round_out)
{
    if (!cmd->has_clip) return ctx->full_scissor;
    int32_t cx, cy, cw, ch;
    if (round_out) {
        cx = (int32_t)floorf(cmd->clip_x * ctx->scale_x);
        cy = (int32_t)floorf(cmd->clip_y * ctx->scale_y);
        cw = (int32_t)ceilf((cmd->clip_x + cmd->clip_w) * ctx->scale_x) - cx;
        ch = (int32_t)ceilf((cmd->clip_y + cmd->clip_h) * ctx->scale_y) - cy;
    } else {
        cx = (int32_t)(cmd->clip_x * ctx->scale_x);
        cy = (int32_t)(cmd->clip_y * ctx->scale_y);
        cw = (int32_t)(cmd->clip_w * ctx->scale_x);
        ch = (int32_t)(cmd->clip_h * ctx->scale_y);
    }
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    return (VkRect2D){ .offset = { cx, cy },
                       .extent = { (uint32_t)cw, (uint32_t)ch } };
}

/**
 * Fills the fields every text/image-pipeline instance shares.
 *
 * @param ctx Frame recording state.
 * @param cmd Source draw command.
 * @param dst Instance record to fill.
 */
static void pack_textured_instance(const PaintCtx *ctx, const Ca_DrawCmd *cmd,
                                   Ca_TextInstance *dst)
{
    ca_instance_pack_transform(cmd, cmd->x, cmd->y, dst->pos, dst->xf_ab, dst->xf_cd);
    dst->size[0] = cmd->w;               dst->size[1] = cmd->h;
    dst->uv[0] = cmd->u0;                dst->uv[1] = cmd->v0;
    dst->uv[2] = cmd->u1;                dst->uv[3] = cmd->v1;
    dst->color[0] = cmd->r;              dst->color[1] = cmd->g;
    dst->color[2] = cmd->b;              dst->color[3] = cmd->a;
    dst->viewport[0] = (float)ctx->log_w; dst->viewport[1] = (float)ctx->log_h;
}

/**
 * Returns which command types occur in `band` within paint positions [lo, hi).
 *
 * @param ctx  Frame recording state.
 * @param band Paint band (see cmd_paint_band).
 * @param lo   First paint-order position.
 * @param hi   One past the last paint-order position.
 * @return Bitmask of RANGE_HAS_* flags.
 */
static uint32_t range_types(const PaintCtx *ctx, int band, uint32_t lo, uint32_t hi)
{
    uint32_t mask = 0;
    for (uint32_t pos = lo; pos < hi; ++pos) {
        uint32_t idx = paint_index(ctx, pos);
        const Ca_DrawCmd *cmd = &ctx->win->draw_cmds[idx];
        if (!cmd->in_use || cmd_paint_band(cmd) != band) continue;
        switch (cmd->type) {
        case CA_DRAW_RECT:     if (idx != ctx->root_bg_idx) mask |= RANGE_HAS_RECT; break;
        case CA_DRAW_GLYPH:    mask |= RANGE_HAS_GLYPH;    break;
        case CA_DRAW_IMAGE:    mask |= RANGE_HAS_IMAGE;    break;
        case CA_DRAW_VIEWPORT: mask |= RANGE_HAS_VIEWPORT; break;
        case CA_DRAW_BACKDROP_BLUR: break;
        }
    }
    return mask;
}

/**
 * Records every rect of `band` within paint positions [lo, hi), batched by
 * scissor and rounded-clip push constant.
 *
 * @param ctx  Frame recording state.
 * @param band Paint band.
 * @param lo   First paint-order position.
 * @param hi   One past the last paint-order position.
 */
static void record_rects(PaintCtx *ctx, int band, uint32_t lo, uint32_t hi)
{
    Ca_Instance *inst = ctx->inst;
    VkCommandBuffer cb = ctx->f->cmd;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->rect_pipeline.pipeline);
    vkCmdSetViewport(cb, 0, 1, &ctx->viewport);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->rect_pipeline.layout, 0, 1, &ctx->f->ssbo_set, 0, NULL);

    uint32_t batch_start = ctx->rect_n;
    VkRect2D cur_sc = ctx->full_scissor;
    Ca_ClipPushConst cur_clip = { 0 };
    cur_clip.edge_aa_scale = ctx->scale_x;
    bool first = true;

    for (uint32_t pos = lo; pos < hi; ++pos) {
        uint32_t idx = paint_index(ctx, pos);
        if (idx == ctx->root_bg_idx) continue;
        const Ca_DrawCmd *cmd = &ctx->win->draw_cmds[idx];
        if (!cmd->in_use || cmd->type != CA_DRAW_RECT || cmd->a < 0.004f) continue;
        if (cmd_paint_band(cmd) != band) continue;

        /* Rounded-clip push constant is in logical/node space, matching
           v_node_pos. */
        VkRect2D sc_new = cmd_scissor(ctx, cmd, true);
        Ca_ClipPushConst clip_new = { 0 };
        clip_new.edge_aa_scale = ctx->scale_x;
        if (cmd->has_clip) {
            clip_new.pos[0]  = cmd->clip_x;
            clip_new.pos[1]  = cmd->clip_y;
            clip_new.size[0] = cmd->clip_w;
            clip_new.size[1] = cmd->clip_h;
            clip_new.radius  = cmd->clip_radius;
        }

        /* Flush on scissor OR clip-shape change: a plain rectangular clip
           and a rounded one can share identical scissor bounds while still
           needing separate push constants. */
        bool clip_changed = memcmp(&clip_new, &cur_clip, sizeof(clip_new)) != 0;
        if (!first && (memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0 || clip_changed)) {
            if (ctx->rect_n > batch_start) {
                vkCmdSetScissor(cb, 0, 1, &cur_sc);
                vkCmdPushConstants(cb, inst->rect_pipeline.layout,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(cur_clip), &cur_clip);
                vkCmdDraw(cb, 6, ctx->rect_n - batch_start, 0, batch_start);
                ctx->batch_n++;
            }
            batch_start = ctx->rect_n;
        }
        cur_sc   = sc_new;
        cur_clip = clip_new;
        first    = false;

        Ca_RectPushConst *dst = &ctx->rect_base[ctx->rect_n++];
        ca_instance_pack_transform(cmd, cmd->x, cmd->y, dst->pos, dst->xf_ab, dst->xf_cd);
        dst->size[0]     = cmd->w;              dst->size[1]     = cmd->h;
        dst->color[0]    = cmd->r;              dst->color[1]    = cmd->g;
        dst->color[2]    = cmd->b;              dst->color[3]    = cmd->a;
        dst->viewport[0] = (float)ctx->log_w;   dst->viewport[1] = (float)ctx->log_h;
        /* Per-corner values are authoritative as a set so an explicit zero
           can keep one edge square while another is rounded. */
        bool has_per_corner = cmd->corner_tl != 0.0f || cmd->corner_tr != 0.0f ||
                              cmd->corner_br != 0.0f || cmd->corner_bl != 0.0f;
        dst->corner_radii[0] = has_per_corner ? cmd->corner_tl : cmd->corner_radius;
        dst->corner_radii[1] = has_per_corner ? cmd->corner_tr : cmd->corner_radius;
        dst->corner_radii[2] = has_per_corner ? cmd->corner_br : cmd->corner_radius;
        dst->corner_radii[3] = has_per_corner ? cmd->corner_bl : cmd->corner_radius;
        dst->border_color[0] = cmd->border_r;   dst->border_color[1] = cmd->border_g;
        dst->border_color[2] = cmd->border_b;   dst->border_color[3] = cmd->border_a;
        dst->color2[0]       = cmd->color2_r;   dst->color2[1]       = cmd->color2_g;
        dst->color2[2]       = cmd->color2_b;   dst->color2[3]       = cmd->color2_a;
        dst->border_width    = cmd->border_width;
        dst->blur_radius     = cmd->blur_radius;
        dst->draw_mode       = (uint32_t)cmd->draw_mode;
        dst->gradient_angle  = cmd->gradient_angle;
        dst->gradient_cx     = cmd->gradient_cx;
        dst->gradient_cy     = cmd->gradient_cy;
    }
    if (ctx->rect_n > batch_start) {
        vkCmdSetScissor(cb, 0, 1, &cur_sc);
        vkCmdPushConstants(cb, inst->rect_pipeline.layout,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cur_clip), &cur_clip);
        vkCmdDraw(cb, 6, ctx->rect_n - batch_start, 0, batch_start);
        ctx->batch_n++;
    }
}

/**
 * Records every glyph of `band` within paint positions [lo, hi), batched by
 * scissor against the shared font atlas.
 *
 * @param ctx  Frame recording state.
 * @param band Paint band.
 * @param lo   First paint-order position.
 * @param hi   One past the last paint-order position.
 */
static void record_glyphs(PaintCtx *ctx, int band, uint32_t lo, uint32_t hi)
{
    Ca_Instance *inst = ctx->inst;
    VkCommandBuffer cb = ctx->f->cmd;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->text_pipeline.pipeline);
    vkCmdSetViewport(cb, 0, 1, &ctx->viewport);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->text_pipeline.layout, 0, 1, &ctx->f->ssbo_set, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->text_pipeline.layout, 1, 1,
                            &inst->text_pipeline.desc_set, 0, NULL);

    uint32_t batch_start = ctx->ti_n;
    VkRect2D cur_sc = ctx->full_scissor;
    bool first = true;

    for (uint32_t pos = lo; pos < hi; ++pos) {
        const Ca_DrawCmd *cmd = &ctx->win->draw_cmds[paint_index(ctx, pos)];
        if (!cmd->in_use || cmd->type != CA_DRAW_GLYPH || cmd->a < 0.004f) continue;
        if (cmd_paint_band(cmd) != band) continue;

        VkRect2D sc_new = cmd_scissor(ctx, cmd, false);
        if (!first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0) {
            if (ctx->ti_n > batch_start) {
                vkCmdSetScissor(cb, 0, 1, &cur_sc);
                vkCmdDraw(cb, 6, ctx->ti_n - batch_start, 0, batch_start);
                ctx->batch_n++;
            }
            batch_start = ctx->ti_n;
        }
        cur_sc = sc_new;
        first  = false;
        pack_textured_instance(ctx, cmd, &ctx->ti_base[ctx->ti_n++]);
    }
    if (ctx->ti_n > batch_start) {
        vkCmdSetScissor(cb, 0, 1, &cur_sc);
        vkCmdDraw(cb, 6, ctx->ti_n - batch_start, 0, batch_start);
        ctx->batch_n++;
    }
}

/**
 * Records every user image of `band` within paint positions [lo, hi),
 * batched by scissor and bound texture.
 *
 * @param ctx  Frame recording state.
 * @param band Paint band.
 * @param lo   First paint-order position.
 * @param hi   One past the last paint-order position.
 */
static void record_images(PaintCtx *ctx, int band, uint32_t lo, uint32_t hi)
{
    Ca_Instance *inst = ctx->inst;
    VkCommandBuffer cb = ctx->f->cmd;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->image_pipeline);
    vkCmdSetViewport(cb, 0, 1, &ctx->viewport);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->text_pipeline.layout, 0, 1, &ctx->f->ssbo_set, 0, NULL);

    uint32_t batch_start = ctx->ti_n;
    VkRect2D cur_sc = ctx->full_scissor;
    uint32_t cur_img = UINT32_MAX;
    bool first = true;

    for (uint32_t pos = lo; pos < hi; ++pos) {
        const Ca_DrawCmd *cmd = &ctx->win->draw_cmds[paint_index(ctx, pos)];
        if (!cmd->in_use || cmd->type != CA_DRAW_IMAGE || cmd->a < 0.004f) continue;
        if (cmd_paint_band(cmd) != band) continue;

        uint32_t ii = cmd->image_index;
        if ((size_t)ii >= ca_pool_slot_count(&inst->images)) continue;
        Ca_Image *image = CA_POOL_AT(inst->images, Ca_Image, ii);
        if (!image->in_use) continue;

        VkRect2D sc_new = cmd_scissor(ctx, cmd, false);
        bool sc_change  = !first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0;
        bool img_change = ii != cur_img;
        if (sc_change || img_change) {
            if (ctx->ti_n > batch_start) {
                vkCmdSetScissor(cb, 0, 1, &cur_sc);
                vkCmdDraw(cb, 6, ctx->ti_n - batch_start, 0, batch_start);
                ctx->batch_n++;
            }
            batch_start = ctx->ti_n;
        }
        if (img_change) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    inst->text_pipeline.layout, 1, 1,
                                    &image->desc_set, 0, NULL);
            cur_img = ii;
        }
        cur_sc = sc_new;
        first  = false;

        Ca_TextInstance *dst = &ctx->ti_base[ctx->ti_n++];
        pack_textured_instance(ctx, cmd, dst);
        image_instance_pack_corner_radii(dst, cmd, ctx->scale_x);
    }
    if (ctx->ti_n > batch_start) {
        vkCmdSetScissor(cb, 0, 1, &cur_sc);
        vkCmdDraw(cb, 6, ctx->ti_n - batch_start, 0, batch_start);
        ctx->batch_n++;
    }
}

/**
 * Records every offscreen viewport of `band` within paint positions
 * [lo, hi), composited as textured quads.
 *
 * @param ctx  Frame recording state.
 * @param band Paint band.
 * @param lo   First paint-order position.
 * @param hi   One past the last paint-order position.
 */
static void record_viewports(PaintCtx *ctx, int band, uint32_t lo, uint32_t hi)
{
    Ca_Instance *inst = ctx->inst;
    Ca_Window *win = ctx->win;
    VkCommandBuffer cb = ctx->f->cmd;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->image_pipeline);
    vkCmdSetViewport(cb, 0, 1, &ctx->viewport);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->text_pipeline.layout, 0, 1, &ctx->f->ssbo_set, 0, NULL);

    uint32_t batch_start = ctx->ti_n;
    VkRect2D cur_sc = ctx->full_scissor;
    uint32_t cur_vp = UINT32_MAX;
    bool first = true;

    for (uint32_t pos = lo; pos < hi; ++pos) {
        const Ca_DrawCmd *cmd = &win->draw_cmds[paint_index(ctx, pos)];
        if (!cmd->in_use || cmd->type != CA_DRAW_VIEWPORT || cmd->a < 0.004f) continue;
        if (cmd_paint_band(cmd) != band) continue;

        uint32_t vi = cmd->viewport_index;
        if (vi >= ca_pool_slot_count(&win->viewport_pool) ||
            !CA_POOL_AT(win->viewport_pool, Ca_Viewport, vi)->in_use)
            continue;
        /* Composite the slot that was actually just rendered
           (last_rendered_frame), not whatever frame_index currently points
           at — frame_index already names the NEXT slot
           ca_viewport_render_all will use by the time this submit runs. */
        Ca_Viewport *vp = CA_POOL_AT(win->viewport_pool, Ca_Viewport, vi);
        Ca_ViewportFrame *vpf = &vp->frame[vp->last_rendered_frame];
        if (vpf->desc_set == VK_NULL_HANDLE || !vpf->has_rendered_once) continue;

        VkRect2D sc_new = cmd_scissor(ctx, cmd, false);
        bool sc_change = !first && memcmp(&sc_new, &cur_sc, sizeof(VkRect2D)) != 0;
        bool vp_change = vi != cur_vp;
        if (sc_change || vp_change) {
            if (ctx->ti_n > batch_start) {
                vkCmdSetScissor(cb, 0, 1, &cur_sc);
                vkCmdDraw(cb, 6, ctx->ti_n - batch_start, 0, batch_start);
                ctx->batch_n++;
            }
            batch_start = ctx->ti_n;
        }
        if (vp_change) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    inst->text_pipeline.layout, 1, 1,
                                    &vpf->desc_set, 0, NULL);
            cur_vp = vi;
        }
        cur_sc = sc_new;
        first  = false;

        Ca_TextInstance *dst = &ctx->ti_base[ctx->ti_n++];
        pack_textured_instance(ctx, cmd, dst);
        image_instance_pack_corner_radii(dst, cmd, ctx->scale_x);
    }
    if (ctx->ti_n > batch_start) {
        vkCmdSetScissor(cb, 0, 1, &cur_sc);
        vkCmdDraw(cb, 6, ctx->ti_n - batch_start, 0, batch_start);
        ctx->batch_n++;
    }
}

/**
 * Records all non-backdrop content of `band` within paint positions
 * [lo, hi). Within a range, commands are batched by type (rects, glyphs,
 * images, viewports); ranges themselves are recorded strictly in order.
 *
 * @param ctx  Frame recording state.
 * @param band Paint band.
 * @param lo   First paint-order position.
 * @param hi   One past the last paint-order position.
 */
static void record_range(PaintCtx *ctx, int band, uint32_t lo, uint32_t hi)
{
    if (lo >= hi) return;
    Ca_Instance *inst = ctx->inst;
    uint32_t types = range_types(ctx, band, lo, hi);
    if ((types & RANGE_HAS_RECT) && inst->rect_pipeline.pipeline != VK_NULL_HANDLE)
        record_rects(ctx, band, lo, hi);
    if ((types & RANGE_HAS_GLYPH) && inst->text_pipeline.pipeline != VK_NULL_HANDLE &&
        inst->font != NULL)
        record_glyphs(ctx, band, lo, hi);
    if ((types & RANGE_HAS_IMAGE) && inst->image_pipeline != VK_NULL_HANDLE)
        record_images(ctx, band, lo, hi);
    if ((types & RANGE_HAS_VIEWPORT) && inst->image_pipeline != VK_NULL_HANDLE &&
        ca_pool_slot_count(&ctx->win->viewport_pool) > 0)
        record_viewports(ctx, band, lo, hi);
}

/**
 * Computes the physical-pixel region a backdrop command covers on screen:
 * its (possibly transformed) border box, intersected with its scissor.
 *
 * @param ctx     Frame recording state.
 * @param cmd     CA_DRAW_BACKDROP_BLUR command.
 * @param scissor The command's clip scissor.
 * @param out     Receives the region.
 * @return false when the region is empty.
 */
static bool backdrop_region(const PaintCtx *ctx, const Ca_DrawCmd *cmd,
                            VkRect2D scissor, VkRect2D *out)
{
    float xs[4] = { cmd->x, cmd->x + cmd->w, cmd->x,          cmd->x + cmd->w };
    float ys[4] = { cmd->y, cmd->y,          cmd->y + cmd->h, cmd->y + cmd->h };
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (int i = 0; i < 4; ++i) {
        float px = xs[i], py = ys[i];
        if (cmd->xf_active) {
            px = cmd->xf_a * xs[i] + cmd->xf_c * ys[i] + cmd->xf_tx;
            py = cmd->xf_b * xs[i] + cmd->xf_d * ys[i] + cmd->xf_ty;
        }
        min_x = fminf(min_x, px); max_x = fmaxf(max_x, px);
        min_y = fminf(min_y, py); max_y = fmaxf(max_y, py);
    }
    if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) || !isfinite(max_y))
        return false;

    float x0 = fmaxf(floorf(min_x * ctx->scale_x), (float)scissor.offset.x);
    float y0 = fmaxf(floorf(min_y * ctx->scale_y), (float)scissor.offset.y);
    float x1 = fminf(ceilf(max_x * ctx->scale_x),
                     (float)scissor.offset.x + (float)scissor.extent.width);
    float y1 = fminf(ceilf(max_y * ctx->scale_y),
                     (float)scissor.offset.y + (float)scissor.extent.height);
    x0 = fmaxf(x0, 0.0f);
    y0 = fmaxf(y0, 0.0f);
    x1 = fminf(x1, (float)ctx->extent.width);
    y1 = fminf(y1, (float)ctx->extent.height);
    if (x1 <= x0 || y1 <= y0) return false;
    *out = (VkRect2D){ .offset = { (int32_t)x0, (int32_t)y0 },
                       .extent = { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
    return true;
}

/**
 * Paints one element's CSS backdrop-filter: blur() at its exact position in
 * paint order. Everything recorded so far is its backdrop; dynamic
 * rendering is suspended for the region capture/blur and resumed with
 * LOAD_OP_LOAD, then the blurred region is composited inside the element's
 * rounded border box before the element's own shadow/background/children.
 *
 * @param ctx Frame recording state.
 * @param cmd CA_DRAW_BACKDROP_BLUR command.
 */
static void record_backdrop(PaintCtx *ctx, const Ca_DrawCmd *cmd)
{
    if (!ctx->can_blur || cmd->backdrop_blur_radius <= 0.0f) return;
    Ca_Instance *inst = ctx->inst;
    Ca_Window *win = ctx->win;
    VkCommandBuffer cb = ctx->f->cmd;

    VkRect2D scissor = cmd_scissor(ctx, cmd, true);
    VkRect2D region;
    if (!backdrop_region(ctx, cmd, scissor, &region)) return;

    float uv_scale[2];
    vkCmdEndRendering(cb);
    bool captured = ca_blur_capture_region(inst, win, cb, ctx->swapchain_image,
                                           ctx->extent, region,
                                           cmd->backdrop_blur_radius * ctx->scale_x,
                                           uv_scale);
    VkRenderingInfo reload_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { .offset = { 0, 0 }, .extent = ctx->extent },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &ctx->reload_attach,
    };
    vkCmdBeginRendering(cb, &reload_info);
    if (!captured) return;

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->backdrop_pipeline);
    vkCmdSetViewport(cb, 0, 1, &ctx->viewport);
    vkCmdSetScissor(cb, 0, 1, &region);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->text_pipeline.layout, 0, 1, &ctx->f->ssbo_set, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->text_pipeline.layout, 1, 1, &win->blur_desc_set, 0, NULL);

    Ca_TextInstance *dst = &ctx->ti_base[ctx->ti_n];
    pack_textured_instance(ctx, cmd, dst);
    dst->uv[0] = uv_scale[0];
    dst->uv[1] = uv_scale[1];
    dst->uv[2] = 0.0f;
    dst->uv[3] = 0.0f;
    image_instance_pack_corner_radii(dst, cmd, ctx->scale_x);
    vkCmdDraw(cb, 6, 1, 0, ctx->ti_n);
    ctx->ti_n++;
    ctx->batch_n++;
}

/**
 * Records one paint band in strict paint order: content is recorded in
 * type-batched ranges split at each backdrop-filter element, so every
 * element's backdrop is exactly what painted before it, and at each
 * paint_barrier command, so it paints over every earlier glyph.
 *
 * @param ctx  Frame recording state.
 * @param band Paint band.
 */
static void record_band(PaintCtx *ctx, int band)
{
    const uint32_t count = ctx->win->draw_cmd_count;
    uint32_t lo = 0;
    for (uint32_t pos = 0; pos < count; ++pos) {
        const Ca_DrawCmd *cmd = &ctx->win->draw_cmds[paint_index(ctx, pos)];
        if (!cmd->in_use || cmd_paint_band(cmd) != band)
            continue;
        if (cmd->type == CA_DRAW_BACKDROP_BLUR) {
            record_range(ctx, band, lo, pos);
            record_backdrop(ctx, cmd);
            lo = pos + 1;
        } else if (cmd->paint_barrier) {
            record_range(ctx, band, lo, pos);
            lo = pos;
        }
    }
    record_range(ctx, band, lo, count);
}

/* Recovers from a frame that fails after its in_flight fence was reset and
   a swapchain image was acquired. Command-free submits consume the acquire
   semaphore and every viewport render_done semaphore (a signaled binary
   semaphore must be waited before it is signaled again), the last one
   signaling in_flight — vkQueueSubmit fences cover all earlier submissions
   on the queue. The swapchain is then recreated to release the acquired,
   never-presented image; otherwise repeated failures exhaust the images and
   vkAcquireNextImageKHR blocks forever. Allocation-free, since the failure
   being handled may itself be an allocation failure.

   inst            Owning instance.
   win             Window whose frame is abandoned. Its swapchain frame
                   state is invalid on return.
   f               Frame slot whose fence was reset.
   viewport_sems   render_done semaphores submitted by this frame's viewport
                   pass.
   viewport_count  Number of entries in viewport_sems. */
static void swapchain_abandon_frame(Ca_Instance *inst, Ca_Window *win,
                                    Ca_Frame *f,
                                    const VkSemaphore *viewport_sems,
                                    uint32_t viewport_count)
{
    Ca_Swapchain *sc = &win->sc;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    for (uint32_t i = 0; i <= viewport_count; ++i) {
        bool last = i == viewport_count;
        VkSubmitInfo drain = {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = last ? &f->image_available : &viewport_sems[i],
            .pWaitDstStageMask  = &stage,
        };
        if (last)
            f->submit_serial = ++sc->submit_serial;
        if (vkQueueSubmit(inst->gfx_queue, 1, &drain,
                          last ? f->in_flight : VK_NULL_HANDLE) != VK_SUCCESS) {
            fprintf(stderr, "[vk] unable to drain abandoned frame\n");
            return;
        }
    }

    VkExtent2D extent = sc->extent;
    ca_swapchain_destroy(inst, win);
    if (!ca_swapchain_create(inst, win, extent.width, extent.height))
        fprintf(stderr, "[vk] swapchain recreation after abandoned frame failed\n");
}

void ca_swapchain_frame(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Swapchain *sc = &win->sc;
    Ca_Frame     *f  = &sc->frames[sc->current_frame];

    ca_profile_begin(inst, "Platform Swapchain Fence");
    double timing_start = glfwGetTime();
    vkWaitForFences(inst->vk_device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);
    inst->frame_timing.swapchain_fence_ms += (glfwGetTime() - timing_start) * 1000.0;
    ca_profile_end(inst, "Platform Swapchain Fence");

    ca_profile_begin(inst, "Platform UI Buffer Ensure");
    if (!ca_instance_buf_ensure(inst, f,
                                win->draw_cmd_count > 0
                                    ? win->draw_cmd_count : 1u)) {
        ca_profile_end(inst, "Platform UI Buffer Ensure");
        fprintf(stderr, "[vk] unable to grow frame instance storage\n");
        return;
    }
    ca_profile_end(inst, "Platform UI Buffer Ensure");

    uint32_t image_index;
    ca_profile_begin(inst, "Platform Swapchain Acquire");
    timing_start = glfwGetTime();
    VkResult result = vkAcquireNextImageKHR(
        inst->vk_device, sc->swapchain, UINT64_MAX,
        f->image_available, VK_NULL_HANDLE, &image_index);
    inst->frame_timing.swapchain_acquire_ms += (glfwGetTime() - timing_start) * 1000.0;
    ca_profile_end(inst, "Platform Swapchain Acquire");

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
       Each redrawn viewport's GPU work is submitted asynchronously — see
       ca_viewport_render_all's doc comment — so its completion semaphore
       must be waited on by this frame's own submit below (added to
       viewport_wait_sems) before compositing samples that viewport's
       texture; nothing else guarantees the render is done by then. */
    ca_profile_begin(inst, "Platform Viewport Pass");
    ca_viewport_render_all(inst, win, &sc->viewport_wait_storage);
    ca_profile_end(inst, "Platform Viewport Pass");
    VkSemaphore *viewport_wait_sems = sc->viewport_wait_storage.data;
    uint32_t viewport_wait_count = (uint32_t)sc->viewport_wait_storage.count;

    vkResetCommandBuffer(f->cmd, 0);

    /* Record */
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    ca_profile_begin(inst, "Platform UI Command Build");
    timing_start = glfwGetTime();
    result = vkBeginCommandBuffer(f->cmd, &begin);
    if (result != VK_SUCCESS) {
        ca_profile_end(inst, "Platform UI Command Build");
        fprintf(stderr, "[vk] vkBeginCommandBuffer failed: %d\n", result);
        swapchain_abandon_frame(inst, win, f, viewport_wait_sems, viewport_wait_count);
        return;
    }

    /* Transition to COLOR_ATTACHMENT_OPTIMAL. srcStage must be
       COLOR_ATTACHMENT_OUTPUT, not TOP_OF_PIPE: this image index was last
       read by the presentation engine's previous present of it, and
       image_available only orders the ACQUIRE (queue submission), not this
       barrier's stages against that prior read. TOP_OF_PIPE establishes no
       execution dependency at all, so the driver had no ordering guarantee
       against the previous present and had to insert its own (variable-
       cost) implicit synchronization every frame — confirmed by the
       synchronization2 validation layer flagging a WRITE_AFTER_READ hazard
       here on every frame (found chasing frame-to-frame GPU-time jitter,
       2026-09-26). */
    transition_image(f->cmd, sc->images[image_index],
        VK_IMAGE_LAYOUT_UNDEFINED,               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    /* If a background render callback is registered, invoke it now.
       It renders directly into the swapchain image (already COLOR_ATTACHMENT_OPTIMAL).
       The UI render pass then uses LOAD_OP_LOAD to preserve this content. */
    bool bg_rendered = false;
    Ca_BgRenderFn bg_render_fn = win->bg_render_fn
                                      ? win->bg_render_fn
                                      : inst->default_bg_render_fn;
    void *bg_render_data = win->bg_render_fn
                               ? win->bg_render_data
                               : inst->default_bg_render_data;
    if (bg_render_fn)
        bg_rendered = bg_render_fn(f->cmd,
                                        win,
                                        sc->images[image_index],
                                        sc->image_views[image_index],
                                        sc->format,
                                        sc->image_usage,
                                        sc->current_frame,
                                        sc->extent.width, sc->extent.height,
                                        bg_render_data);

    /* Background: use the root node's color if available */
    float bg_r = 0.15f, bg_g = 0.15f, bg_b = 0.17f, bg_a = 1.0f;
    uint32_t root_bg_idx = find_root_bg_cmd_index(win);
    if (root_bg_idx != UINT32_MAX) {
        bg_r = win->draw_cmds[root_bg_idx].r;
        bg_g = win->draw_cmds[root_bg_idx].g;
        bg_b = win->draw_cmds[root_bg_idx].b;
        bg_a = win->draw_cmds[root_bg_idx].a;
    }

    /* Dynamic rendering — load if bg_render wrote content, clear otherwise */
    VkRenderingAttachmentInfo color_attach = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = sc->image_views[image_index],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = bg_rendered ? VK_ATTACHMENT_LOAD_OP_LOAD
                                   : VK_ATTACHMENT_LOAD_OP_CLEAR,
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

    /* --- Z-index: partition into z<0 | z==0 | z>0, sort only z!=0 ---
       root_bg_idx (the root's own background rect, drawn implicitly via
       VK_ATTACHMENT_LOAD_OP_CLEAR — see find_root_bg_cmd_index) is pinned
       at sorted_idx[0] and excluded from partitioning/sorting, same as the
       rest of this scheme previously assumed slot 0 always held it. */
    uint32_t *sorted_idx = NULL;
    {
        uint32_t count  = win->draw_cmd_count;
        uint32_t pinned = (root_bg_idx != UINT32_MAX) ? root_bg_idx : 0;
        uint32_t n_neg = 0, n_pos = 0;
        for (uint32_t d = 0; d < count; ++d) {
            if (d == pinned) continue;
            int z = win->draw_cmds[d].z_index;
            if (z < 0) n_neg++;
            else if (z > 0) n_pos++;
        }
        if ((n_neg | n_pos) && count > 1) {
            if (!ca_window_reserve_sorted_indices(win, count)) {
                n_neg = 0;
                n_pos = 0;
            }
        }
        if ((n_neg | n_pos) && count > 1) {
            sorted_idx = win->sorted_idx;
            sorted_idx[0] = pinned;
            uint32_t n_zero  = count - 1 - n_neg - n_pos;
            uint32_t ni = 1, zi = 1 + n_neg, pi = 1 + n_neg + n_zero;
            for (uint32_t d = 0; d < count; ++d) {
                if (d == pinned) continue;
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

    /* Instance data is packed into fixed-size SSBO slots — region 0: rect
       instances; region 1: text/image/viewport/backdrop instances after the
       rect range. Count actual rects so the text region starts right after
       them instead of over-reserving when most commands are glyphs. */
    uint32_t rect_cmd_count = 0;
    for (uint32_t d = 0; d < win->draw_cmd_count; ++d) {
        if (win->draw_cmds[d].in_use &&
            win->draw_cmds[d].type == CA_DRAW_RECT)
            rect_cmd_count++;
    }

    PaintCtx paint = {
        .inst            = inst,
        .win             = win,
        .f               = f,
        .sorted_idx      = sorted_idx,
        .root_bg_idx     = root_bg_idx,
        .scale_x         = scale_x,
        .scale_y         = scale_y,
        .log_w           = log_w,
        .log_h           = log_h,
        .extent          = sc->extent,
        .full_scissor    = full_scissor,
        .viewport        = {
            .x = 0.0f, .y = 0.0f,
            .width  = (float)sc->extent.width,
            .height = (float)sc->extent.height,
            .minDepth = 0.0f, .maxDepth = 1.0f,
        },
        .swapchain_image = sc->images[image_index],
        .reload_attach   = {
            .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView   = sc->image_views[image_index],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        },
        .can_blur        = inst->backdrop_pipeline != VK_NULL_HANDLE &&
                           win->blur_image != VK_NULL_HANDLE &&
                           (sc->image_usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u,
        .rect_base       = (Ca_RectPushConst *)f->instance_mapped,
        .ti_base         = (Ca_TextInstance *)f->instance_mapped,
        .rect_n          = 0,
        .ti_n            = rect_cmd_count,
        .batch_n         = 0,
    };
    for (int band = 0; band < 4; ++band)
        record_band(&paint, band);

    /* Store debug stats for the overlay */
    win->dbg_frames_rendered++;
    win->dbg_draw_cmds       = win->draw_cmd_count;
    win->dbg_rect_instances  = paint.rect_n;
    win->dbg_ti_instances    = paint.ti_n;
    win->dbg_batches         = paint.batch_n;

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

    result = vkEndCommandBuffer(f->cmd);
    inst->frame_timing.swapchain_record_ms += (glfwGetTime() - timing_start) * 1000.0;
    ca_profile_end(inst, "Platform UI Command Build");
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkEndCommandBuffer failed: %d\n", result);
        swapchain_abandon_frame(inst, win, f, viewport_wait_sems, viewport_wait_count);
        return;
    }

    /* Submit. Waits on image_available (swapchain image ready to draw
       into) plus every viewport's render_done semaphore collected above
       (viewport texture ready to sample during compositing) — the GPU-
       level replacement for the CPU wait ca_viewport_render_all no longer
       does. */
    size_t wait_count = (size_t)viewport_wait_count + 1u;
    if (!ca_dyn_array_resize(&sc->submit_wait_storage, wait_count) ||
        !ca_dyn_array_resize(&sc->submit_stage_storage, wait_count)) {
        fprintf(stderr, "[vk] unable to allocate submit wait storage\n");
        swapchain_abandon_frame(inst, win, f, viewport_wait_sems, viewport_wait_count);
        return;
    }
    VkSemaphore *wait_sems = sc->submit_wait_storage.data;
    VkPipelineStageFlags *wait_stages = sc->submit_stage_storage.data;
    wait_sems[0]   = f->image_available;
    wait_stages[0] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    for (uint32_t i = 0; i < viewport_wait_count; i++) {
        wait_sems[1 + i]   = viewport_wait_sems[i];
        wait_stages[1 + i] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    VkSemaphore render_finished = sc->image_render_finished[image_index];
    VkSubmitInfo submit = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = 1 + viewport_wait_count,
        .pWaitSemaphores      = wait_sems,
        .pWaitDstStageMask    = wait_stages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &f->cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &render_finished,
    };
    ca_profile_begin(inst, "Platform Swapchain Submit");
    timing_start = glfwGetTime();
    /* Tagged before submitting; the abandon path below re-tags on failure. */
    f->submit_serial = ++sc->submit_serial;
    result = vkQueueSubmit(inst->gfx_queue, 1, &submit, f->in_flight);
    inst->frame_timing.swapchain_submit_ms += (glfwGetTime() - timing_start) * 1000.0;
    ca_profile_end(inst, "Platform Swapchain Submit");
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkQueueSubmit failed: %d\n", result);
        /* The real submit did not happen, so its waits were never consumed
           and in_flight (reset above) would never signal; the frame's
           content is dropped. */
        swapchain_abandon_frame(inst, win, f, viewport_wait_sems, viewport_wait_count);
        return;
    }

    /* Present */
    VkPresentInfoKHR present = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &render_finished,
        .swapchainCount     = 1,
        .pSwapchains        = &sc->swapchain,
        .pImageIndices      = &image_index,
    };
    ca_profile_begin(inst, "Platform Present");
    timing_start = glfwGetTime();
    result = vkQueuePresentKHR(inst->present_queue, &present);
    inst->frame_timing.present_ms += (glfwGetTime() - timing_start) * 1000.0;
    ca_profile_end(inst, "Platform Present");
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        int w, h;
        glfwGetFramebufferSize(win->glfw, &w, &h);
        ca_renderer_window_resize(inst, win, w, h);
    }

    sc->current_frame = (sc->current_frame + 1) % CA_FRAMES_IN_FLIGHT;
}
