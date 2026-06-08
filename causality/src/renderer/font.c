// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* font.c — high-quality dynamic glyph atlas backed by FreeType.

   Text and Nerd Font icons are rasterised on demand at their final
   device-pixel size, packed into 1024x1024 pages inside one 4096x4096
   Vulkan atlas, and evicted with an LRU policy.  UI layout stays in logical
   pixels; the glyph cache key stores the scaled visual size so runtime UI
   scale changes produce native-resolution glyphs instead of stretched
   bitmaps.                                                             */

#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_MODULE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Codepoint ranges supported by the on-demand cache: ASCII/Latin-1 plus
   the Nerd-Font private-use ranges used by the editor.                 */
static const struct { int first; int count; } g_range_defs[CA_FONT_RANGE_COUNT] = {
    { 32,     224 },   /* ASCII + Latin-1 Supplement  (32-255)     */
    { 0xE0A0,  56 },   /* Powerline + extras          (E0A0-E0D7) */
    { 0xE5FA, 188 },   /* Seti-UI + Custom            (E5FA-E6B5) */
    { 0xE700, 198 },   /* Devicons                    (E700-E7C5) */
    { 0xEA60, 447 },   /* Codicons                    (EA60-EC1E) */
    { 0xF000, 737 },   /* Font Awesome                (F000-F2E0) */
};

static int chars_for_ranges(int num_ranges)
{
    int n = 0;
    for (int i = 0; i < num_ranges; i++)
        n += g_range_defs[i].count;
    return n;
}

/* ============================================================
   Vulkan helpers
   ============================================================ */

static uint32_t find_memory_type(VkPhysicalDevice gpu, uint32_t type_bits,
                                  VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}

static VkCommandBuffer begin_once(Ca_Instance *inst)
{
    VkCommandBufferAllocateInfo alloc = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = inst->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(inst->vk_device, &alloc, &cmd);

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

static void end_once(Ca_Instance *inst, VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    vkQueueSubmit(inst->gfx_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst->gfx_queue);
    vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &cmd);
}

/* Upload an RGBA8 atlas bitmap to the font image, create view + sampler. */
static bool upload_atlas(Ca_Instance *inst, Ca_Font *font,
                          const unsigned char *bitmap_rgba)
{
    VkDeviceSize atlas_sz = (VkDeviceSize)font->atlas_w * font->atlas_h * 4;
    VkResult vr;

    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = { (uint32_t)font->atlas_w, (uint32_t)font->atlas_h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vr = vkCreateImage(inst->vk_device, &img_ci, NULL, &font->image);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkCreateImage failed for atlas (%d)\n", vr);
        return false;
    }

    VkMemoryRequirements img_req;
    vkGetImageMemoryRequirements(inst->vk_device, font->image, &img_req);
    uint32_t img_mem_type = find_memory_type(inst->vk_gpu, img_req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mem_type == UINT32_MAX) {
        fprintf(stderr, "[font] no device-local memory for atlas image\n");
        return false;
    }
    VkMemoryAllocateInfo img_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = img_req.size,
        .memoryTypeIndex = img_mem_type,
    };
    vr = vkAllocateMemory(inst->vk_device, &img_mem_ai, NULL, &font->memory);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkAllocateMemory failed for atlas image (%d)\n", vr);
        return false;
    }
    vr = vkBindImageMemory(inst->vk_device, font->image, font->memory, 0);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkBindImageMemory failed for atlas image (%d)\n", vr);
        return false;
    }

    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = atlas_sz,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    vr = vkCreateBuffer(inst->vk_device, &buf_ci, NULL, &staging_buf);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkCreateBuffer failed for initial atlas upload (%d)\n", vr);
        return false;
    }

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(inst->vk_device, staging_buf, &buf_req);
    uint32_t buf_mem_type = find_memory_type(inst->vk_gpu, buf_req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_mem_type == UINT32_MAX) {
        fprintf(stderr, "[font] no host-visible memory for initial atlas upload\n");
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        return false;
    }
    VkMemoryAllocateInfo buf_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = buf_req.size,
        .memoryTypeIndex = buf_mem_type,
    };
    vr = vkAllocateMemory(inst->vk_device, &buf_mem_ai, NULL, &staging_mem);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkAllocateMemory failed for initial atlas upload (%d)\n", vr);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        return false;
    }
    vr = vkBindBufferMemory(inst->vk_device, staging_buf, staging_mem, 0);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkBindBufferMemory failed for initial atlas upload (%d)\n", vr);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        vkFreeMemory(inst->vk_device, staging_mem, NULL);
        return false;
    }

    void *mapped = NULL;
    vr = vkMapMemory(inst->vk_device, staging_mem, 0, atlas_sz, 0, &mapped);
    if (vr != VK_SUCCESS || !mapped) {
        fprintf(stderr, "[font] vkMapMemory failed for initial atlas upload (%d)\n", vr);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        vkFreeMemory(inst->vk_device, staging_mem, NULL);
        return false;
    }
    memcpy(mapped, bitmap_rgba, (size_t)atlas_sz);
    vkUnmapMemory(inst->vk_device, staging_mem);

    VkCommandBuffer cmd = begin_once(inst);
    VkImageMemoryBarrier bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = font->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent      = { (uint32_t)font->atlas_w,
                              (uint32_t)font->atlas_h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging_buf, font->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    end_once(inst, cmd);

    vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
    vkFreeMemory(inst->vk_device, staging_mem, NULL);

    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = font->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vr = vkCreateImageView(inst->vk_device, &view_ci, NULL, &font->view);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkCreateImageView failed for atlas (%d)\n", vr);
        return false;
    }

    /* Glyph bitmaps already contain final grayscale/LCD antialiasing.
       Sample exact atlas texels to avoid softening 1x Windows text. */
    VkSamplerCreateInfo samp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_NEAREST,
        .minFilter    = VK_FILTER_NEAREST,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = VK_LOD_CLAMP_NONE,
    };
    vr = vkCreateSampler(inst->vk_device, &samp_ci, NULL, &font->sampler);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkCreateSampler failed for atlas (%d)\n", vr);
        return false;
    }
    return true;
}

/* Copy a FreeType LCD bitmap into the atlas at (dst_x, dst_y).  This is kept
   for compatibility if a platform path ever requests LCD bitmaps; the default
   Causality path below uses grayscale for predictable cross-display output. */
static void blit_lcd(unsigned char *atlas, int atlas_w,
                      int dst_x, int dst_y,
                      const FT_Bitmap *src)
{
    int pixel_w = (int)src->width / 3;
    int pixel_h = (int)src->rows;
    int src_pitch = src->pitch;  /* may be negative if top-down */
    const unsigned char *row = src->buffer;
    if (src_pitch < 0) {
        row = src->buffer + (pixel_h - 1) * (-src_pitch);
    }
    for (int y = 0; y < pixel_h; y++) {
        const unsigned char *s = row;
        unsigned char *d = atlas + ((dst_y + y) * atlas_w + dst_x) * 4;
        for (int x = 0; x < pixel_w; x++) {
            unsigned r = s[0], g = s[1], b = s[2];
            /* ITU-R BT.601 luma weights for the alpha fallback.       */
            unsigned a = (r * 76u + g * 150u + b * 29u + 128u) >> 8;
            if (a > 255) a = 255;
            d[0] = (unsigned char)r;
            d[1] = (unsigned char)g;
            d[2] = (unsigned char)b;
            d[3] = (unsigned char)a;
            s += 3;
            d += 4;
        }
        row += src_pitch;
    }
}

/* Strengthen regular text coverage after high-resolution rasterisation so
   small embedded-font glyphs remain solid after low-DPI downsampling. */
static unsigned char strengthen_text_coverage(unsigned char value)
{
    if (value == 0 || value == 255) return value;
    unsigned v = value;
    unsigned boosted = v + (((255u - v) * v + 510u) / 1020u);
    return (unsigned char)(boosted > 255u ? 255u : boosted);
}

/* Blit a normal grayscale FreeType bitmap into all RGB channels of the atlas
   so the existing text shader receives identical per-channel coverage. */
static void blit_gray(unsigned char *atlas, int atlas_w,
                       int dst_x, int dst_y,
                       const FT_Bitmap *src,
                       bool strengthen)
{
    int w = (int)src->width;
    int h = (int)src->rows;
    int src_pitch = src->pitch;
    const unsigned char *row = src->buffer;
    if (src_pitch < 0) row = src->buffer + (h - 1) * (-src_pitch);
    for (int y = 0; y < h; y++) {
        const unsigned char *s = row;
        unsigned char *d = atlas + ((dst_y + y) * atlas_w + dst_x) * 4;
        for (int x = 0; x < w; x++) {
            unsigned char v = *s++;
            if (strengthen) v = strengthen_text_coverage(v);
            d[0] = v; d[1] = v; d[2] = v; d[3] = v;
            d += 4;
        }
        row += src_pitch;
    }
}

static unsigned char bitmap_gray_at(const unsigned char *base_row,
                                    int src_pitch,
                                    int x, int y)
{
    const unsigned char *row = base_row + y * src_pitch;
    return row[x];
}

/* Reconstructs a supersampled FreeType grayscale bitmap into atlas coverage.
   Parameters: atlas destination, source bitmap, supersample ratio, and whether
   small low-DPI text coverage should be strengthened. */
static void blit_gray_reconstructed(unsigned char *atlas, int atlas_w,
                                    int dst_x, int dst_y,
                                    const FT_Bitmap *src,
                                    int supersample,
                                    bool strengthen)
{
    int src_w = (int)src->width;
    int src_h = (int)src->rows;
    if (src_w <= 0 || src_h <= 0) return;
    if (supersample <= 1) {
        blit_gray(atlas, atlas_w, dst_x, dst_y, src, strengthen);
        return;
    }

    int dst_w = (src_w + supersample - 1) / supersample;
    int dst_h = (src_h + supersample - 1) / supersample;
    int src_pitch = src->pitch;
    const unsigned char *base_row = src->buffer;
    if (src_pitch < 0) {
        base_row = src->buffer + (src_h - 1) * (-src_pitch);
    }

    for (int y = 0; y < dst_h; y++) {
        unsigned char *d = atlas + ((dst_y + y) * atlas_w + dst_x) * 4;
        int sy0 = y * supersample;
        int sy1 = sy0 + supersample;
        if (sy1 > src_h) sy1 = src_h;
        for (int x = 0; x < dst_w; x++) {
            int sx0 = x * supersample;
            int sx1 = sx0 + supersample;
            if (sx1 > src_w) sx1 = src_w;

            unsigned sum = 0;
            unsigned count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    sum += bitmap_gray_at(base_row, src_pitch, sx, sy);
                    count++;
                }
            }

            unsigned char v = count > 0
                ? (unsigned char)((sum + count / 2u) / count)
                : 0;
            if (strengthen) v = strengthen_text_coverage(v);
            d[0] = v; d[1] = v; d[2] = v; d[3] = v;
            d += 4;
        }
    }
}

/* ============================================================
   Dynamic page cache
   ============================================================ */

static uint32_t font_size_key(float desired_px)
{
    if (!(desired_px > 0.0f)) desired_px = CA_FONT_DEFAULT_SIZE_PX;
    if (desired_px < 1.0f) desired_px = 1.0f;
    if (desired_px > 512.0f) desired_px = 512.0f;
    return (uint32_t)(desired_px * 64.0f + 0.5f);
}

static int font_range_index_for_cp(uint32_t cp)
{
    for (int i = 0; i < CA_FONT_RANGE_COUNT; i++) {
        uint32_t first = (uint32_t)g_range_defs[i].first;
        uint32_t last  = first + (uint32_t)g_range_defs[i].count;
        if (cp >= first && cp < last) return i;
    }
    return -1;
}

static bool font_codepoint_is_icon(uint32_t cp, int range_index)
{
    if (range_index >= CA_FONT_TEXT_RANGES)
        return true;
    /* Nerd Fonts also use supplementary private-use planes beyond the legacy
       BMP ranges above.  Treat all Unicode private-use blocks as icons so
       they resolve through the regular Nerd-Font face. */
    return (cp >= 0xE000u  && cp <= 0xF8FFu)  ||
           (cp >= 0xF0000u && cp <= 0xFFFFDu) ||
           (cp >= 0x100000u && cp <= 0x10FFFDu);
}

static void font_mark_dirty(Ca_Font *font, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h)
{
    if (!font || w == 0 || h == 0) return;
    if (font->dirty_full) return;
    if (font->dirty_rect_count >= CA_FONT_MAX_DIRTY_RECTS) {
        font->dirty_full = true;
        font->dirty_rect_count = 0;
        return;
    }
    font->dirty_rects[font->dirty_rect_count++] =
        (Ca_FontDirtyRect){ x, y, w, h };
}

static void font_invalidate_paint_caches(Ca_Font *font)
{
    Ca_Instance *inst = font ? font->owner : NULL;
    if (!inst) return;
    for (int wi = 0; wi < CA_MAX_WINDOWS_TOTAL; wi++) {
        Ca_Window *win = &inst->windows[wi];
        if (!win->in_use || !win->node_pool) continue;
        win->paint_cache_used = 0;
        for (uint32_t ni = 0; ni < CA_MAX_NODES_PER_WINDOW; ni++) {
            Ca_Node *node = &win->node_pool[ni];
            if (!node->in_use) continue;
            node->dirty |= CA_DIRTY_CONTENT;
            node->cache_count = 0;
            node->cache_post_count = 0;
        }
        win->needs_render = true;
    }
}

static void font_init_ranges(Ca_FontTier *tier)
{
    int cpt = chars_for_ranges(CA_FONT_RANGE_COUNT);
    tier->chardata_block = (Ca_Glyph *)CA_CALLOC((size_t)cpt, sizeof(Ca_Glyph));
    if (!tier->chardata_block) return;
    int offset = 0;
    for (int r = 0; r < CA_FONT_RANGE_COUNT; r++) {
        tier->ranges[r].first_codepoint = g_range_defs[r].first;
        tier->ranges[r].num_chars       = g_range_defs[r].count;
        tier->ranges[r].chardata        = tier->chardata_block + offset;
        offset += g_range_defs[r].count;
    }
    tier->extra_capacity = CA_FONT_EXTRA_GLYPHS_PER_PAGE;
    tier->extra_lookup_capacity = CA_FONT_EXTRA_LOOKUP_CAPACITY;
    tier->extra_glyphs = (Ca_FontExtraGlyph *)CA_CALLOC(
        tier->extra_capacity, sizeof(Ca_FontExtraGlyph));
    tier->extra_lookup = (uint16_t *)CA_CALLOC(
        tier->extra_lookup_capacity, sizeof(uint16_t));
}

static void font_clear_page(Ca_Font *font, Ca_FontTier *tier, bool clear_pixels)
{
    if (!font || !tier || !tier->dynamic_page) return;
    if (clear_pixels && font->atlas_rgba) {
        for (uint16_t y = 0; y < CA_FONT_PAGE_SIZE; y++) {
            unsigned char *row = font->atlas_rgba +
                (((uint32_t)tier->origin_y + y) * (uint32_t)font->atlas_w +
                 (uint32_t)tier->origin_x) * 4u;
            memset(row, 0, (size_t)CA_FONT_PAGE_SIZE * 4u);
        }
        font_mark_dirty(font, tier->origin_x, tier->origin_y,
                        CA_FONT_PAGE_SIZE, CA_FONT_PAGE_SIZE);
    }
    if (tier->chardata_block) {
        int cpt = chars_for_ranges(CA_FONT_RANGE_COUNT);
        memset(tier->chardata_block, 0, (size_t)cpt * sizeof(Ca_Glyph));
    }
    if (tier->extra_glyphs)
        memset(tier->extra_glyphs, 0,
               (size_t)tier->extra_capacity * sizeof(Ca_FontExtraGlyph));
    if (tier->extra_lookup)
        memset(tier->extra_lookup, 0,
               (size_t)tier->extra_lookup_capacity * sizeof(uint16_t));
    tier->extra_count = 0;
    tier->shelf_x = tier->shelf_y = tier->shelf_h = 0;
    tier->packed = false;
}

static bool font_set_page_metrics(Ca_Font *font, Ca_FontTier *tier)
{
    FT_Face face = (tier->style == CA_FONT_STYLE_BOLD && font->bold_face)
        ? (FT_Face)font->bold_face
        : (FT_Face)font->regular_face;
    if (!face) return false;
    FT_Error err = FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(tier->baked_px + 0.5f));
    if (err) return false;

    float to_logical = tier->logical_px / tier->baked_px;
    float ascent  = (float)face->size->metrics.ascender  / 64.0f;
    float descent = (float)face->size->metrics.descender / 64.0f;
    float height  = (float)face->size->metrics.height    / 64.0f;
    tier->ascent   = ascent * to_logical;
    tier->descent  = descent * to_logical;
    tier->line_gap = (height - (ascent - descent)) * to_logical;
    return true;
}

static bool font_init_page(Ca_Font *font, Ca_FontTier *tier,
                           uint32_t size_key, uint8_t style,
                           uint16_t page_index, bool clear_pixels)
{
    uint16_t cols = (uint16_t)(CA_FONT_ATLAS_W / CA_FONT_PAGE_SIZE);
    memset(tier, 0, sizeof(*tier));
    tier->owner        = font;
    tier->dynamic_page = true;
    tier->style        = style;
    tier->page_index   = page_index;
    tier->origin_x     = (uint16_t)((page_index % cols) * CA_FONT_PAGE_SIZE);
    tier->origin_y     = (uint16_t)((page_index / cols) * CA_FONT_PAGE_SIZE);
    tier->size_key     = size_key;
    tier->logical_px   = (float)size_key / 64.0f;
    tier->baked_px     = tier->logical_px * font->content_scale;
    if (tier->baked_px < 1.0f) tier->baked_px = 1.0f;
    tier->baked_px     = (float)(int)(tier->baked_px + 0.5f);
    tier->last_used_frame = font->frame_counter;
    font_init_ranges(tier);
    if (!tier->chardata_block || !tier->extra_glyphs || !tier->extra_lookup) {
        CA_FREE(tier->chardata_block);
        CA_FREE(tier->extra_glyphs);
        CA_FREE(tier->extra_lookup);
        memset(tier, 0, sizeof(*tier));
        return false;
    }
    if (!font_set_page_metrics(font, tier)) {
        CA_FREE(tier->chardata_block);
        CA_FREE(tier->extra_glyphs);
        CA_FREE(tier->extra_lookup);
        memset(tier, 0, sizeof(*tier));
        return false;
    }
    font_clear_page(font, tier, clear_pixels);
    tier->packed = true;
    return true;
}

static Ca_FontTier *font_find_page(Ca_Font *font, uint32_t key, uint8_t style)
{
    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        Ca_FontTier *tier = &font->pages[i];
        if (tier->dynamic_page && tier->packed &&
            tier->size_key == key && tier->style == style) {
            ca_font_touch_page(font, i);
            return tier;
        }
    }
    return NULL;
}

static Ca_FontTier *font_alloc_page(Ca_Font *font, uint32_t key, uint8_t style)
{
    int victim = -1;
    uint64_t oldest = UINT64_MAX;

    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        if (!font->pages[i].dynamic_page) {
            victim = i;
            break;
        }
        if (font->pages[i].last_used_frame == font->frame_counter)
            continue;
        if (font->pages[i].last_used_frame < oldest) {
            oldest = font->pages[i].last_used_frame;
            victim = i;
        }
    }

    if (victim < 0) {
        /* All pages are referenced this frame.  Keep rendering stable rather
           than evicting live UVs; the missing glyph will retry next frame. */
        return NULL;
    }

    Ca_FontTier *tier = &font->pages[victim];
    bool was_dynamic = tier->dynamic_page;
    if (was_dynamic)
        font_invalidate_paint_caches(font);
    CA_FREE(tier->chardata_block);
    CA_FREE(tier->extra_glyphs);
    CA_FREE(tier->extra_lookup);
    return font_init_page(font, tier, key, style, (uint16_t)victim, was_dynamic)
        ? tier : NULL;
}

static Ca_FontTier *font_any_live_page(Ca_Font *font)
{
    if (!font) return NULL;
    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        Ca_FontTier *tier = &font->pages[i];
        if (tier->dynamic_page && tier->packed &&
            tier->style == CA_FONT_STYLE_REGULAR) {
            ca_font_touch_page(font, i);
            return tier;
        }
    }
    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        Ca_FontTier *tier = &font->pages[i];
        if (tier->dynamic_page && tier->packed) {
            ca_font_touch_page(font, i);
            return tier;
        }
    }
    return NULL;
}

static FT_Face font_primary_face_for_range(Ca_Font *font,
                                           uint8_t style,
                                           bool is_icon_range)
{
    if (!font) return NULL;
    /* Icon glyphs live in the bundled Symbols Nerd Font layer. */
    if (is_icon_range)
        return font->icon_face ? (FT_Face)font->icon_face
                               : (FT_Face)font->regular_face;
    if (style == CA_FONT_STYLE_BOLD && font->bold_face)
        return (FT_Face)font->bold_face;
    return (FT_Face)font->regular_face;
}

static bool font_set_face_size(FT_Face face, float baked_px)
{
    return face &&
           FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(baked_px + 0.5f)) == 0;
}

/* Returns the Causality-owned text supersampling ratio for a glyph.
   Parameters: font display state, tier size metrics, and icon-range flag. */
static int font_supersample_for_glyph(const Ca_Font *font,
                                      const Ca_FontTier *tier,
                                      bool is_icon_range)
{
    if (!font || !tier || is_icon_range) return 1;
    if (font->display_scale >= 1.5f)
        return CA_FONT_TEXT_SUPERSAMPLE_HIDPI;
    return tier->logical_px <= 18.0f
        ? CA_FONT_TEXT_SUPERSAMPLE_LOW_DPI_SMALL
        : CA_FONT_TEXT_SUPERSAMPLE_LOW_DPI_LARGE;
}

/* Returns whether to use RGB subpixel coverage for Windows-like 1x text.
   Parameters: font display state, tier size metrics, and icon-range flag. */
static bool font_should_use_lcd_glyph(const Ca_Font *font,
                                      const Ca_FontTier *tier,
                                      bool is_icon_range)
{
    if (!font || !tier || is_icon_range) return false;
    return font->display_scale < 1.5f && tier->logical_px <= 18.0f;
}

/* Returns whether post-reconstruction coverage should be boosted.
   Parameters: font display state, tier size metrics, and icon-range flag. */
static bool font_should_strengthen_glyph(const Ca_Font *font,
                                         const Ca_FontTier *tier,
                                         bool is_icon_range)
{
    if (!font || !tier || is_icon_range) return false;
    return font->display_scale < 1.5f && tier->logical_px <= 12.0f;
}

static bool font_page_alloc(Ca_FontTier *tier, int w, int h, int *out_x, int *out_y)
{
    if (w <= 0 || h <= 0) return false;
    if (w > CA_FONT_PAGE_SIZE || h > CA_FONT_PAGE_SIZE) return false;
    if (tier->shelf_x + w > CA_FONT_PAGE_SIZE || h > tier->shelf_h) {
        uint16_t new_top = (uint16_t)(tier->shelf_y + tier->shelf_h);
        if ((int)new_top + h > CA_FONT_PAGE_SIZE) return false;
        tier->shelf_y = new_top;
        tier->shelf_h = (uint16_t)h;
        tier->shelf_x = 0;
    }
    *out_x = (int)tier->origin_x + tier->shelf_x;
    *out_y = (int)tier->origin_y + tier->shelf_y;
    tier->shelf_x = (uint16_t)(tier->shelf_x + w);
    return true;
}

static bool font_render_glyph(Ca_FontTier *tier, uint32_t cp, Ca_Glyph *g);

static Ca_Glyph *font_extra_glyph(Ca_FontTier *tier, uint32_t cp, bool create)
{
    if (!tier || !tier->extra_glyphs || !tier->extra_lookup ||
        tier->extra_lookup_capacity == 0u)
        return NULL;

    uint32_t pos = (cp * 2654435761u) % tier->extra_lookup_capacity;
    for (uint16_t probe = 0; probe < tier->extra_lookup_capacity; probe++) {
        uint16_t entry = tier->extra_lookup[pos];
        if (entry != 0u) {
            Ca_FontExtraGlyph *slot = &tier->extra_glyphs[entry - 1u];
            if (slot->codepoint == cp)
                return &slot->glyph;
        } else {
            if (!create || tier->extra_count >= tier->extra_capacity)
                return NULL;
            uint16_t new_entry = (uint16_t)(tier->extra_count + 1u);
            Ca_FontExtraGlyph *slot = &tier->extra_glyphs[tier->extra_count++];
            memset(slot, 0, sizeof(*slot));
            slot->codepoint = cp;
            tier->extra_lookup[pos] = new_entry;
            return &slot->glyph;
        }
        pos++;
        if (pos >= tier->extra_lookup_capacity) pos = 0u;
    }
    return NULL;
}

static Ca_Glyph *font_glyph_slot(Ca_FontTier *tier, uint32_t cp, bool create)
{
    if (!tier) return NULL;
    int range_index = font_range_index_for_cp(cp);
    if (range_index >= 0) {
        Ca_GlyphRange *r = &tier->ranges[range_index];
        if (!r->chardata) return NULL;
        return &r->chardata[cp - (uint32_t)r->first_codepoint];
    }
    return font_extra_glyph(tier, cp, create);
}

static bool font_page_matches(const Ca_FontTier *tier,
                              uint32_t key, uint8_t style)
{
    return tier && tier->dynamic_page && tier->packed &&
           tier->size_key == key && tier->style == style;
}

static Ca_Glyph *font_find_rendered_glyph(Ca_Font *font,
                                          uint32_t key, uint8_t style,
                                          uint32_t cp,
                                          Ca_FontTier **out_tier)
{
    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        Ca_FontTier *page = &font->pages[i];
        if (!font_page_matches(page, key, style)) continue;
        Ca_Glyph *g = font_glyph_slot(page, cp, false);
        if (g && g->valid) {
            ca_font_touch_page(font, i);
            if (out_tier) *out_tier = page;
            return g;
        }
    }
    return NULL;
}

static Ca_Glyph *font_render_glyph_on_page(Ca_FontTier *page,
                                           uint32_t cp,
                                           Ca_FontTier **out_tier)
{
    Ca_Glyph *g = font_glyph_slot(page, cp, true);
    if (!g) return NULL;
    if (g->valid || font_render_glyph(page, cp, g)) {
        if (out_tier) *out_tier = page;
        return g;
    }
    return NULL;
}

static bool font_render_glyph(Ca_FontTier *tier, uint32_t cp, Ca_Glyph *g)
{
    Ca_Font *font = tier ? tier->owner : NULL;
    if (!font || !g) return false;
    memset(g, 0, sizeof(*g));

    int range_index = font_range_index_for_cp(cp);
    bool is_icon_range = font_codepoint_is_icon(cp, range_index);
    FT_Face face = font_primary_face_for_range(font, tier->style, is_icon_range);
    if (!font_set_face_size(face, tier->baked_px))
        return false;
    const int supersample = font_supersample_for_glyph(font, tier,
                                                       is_icon_range);
    const bool use_lcd = font_should_use_lcd_glyph(font, tier, is_icon_range);

    if (cp < 32u || cp == 0x7Fu) {
        if (cp == '\t') {
            FT_UInt space_gi = FT_Get_Char_Index(face, ' ');
            if (space_gi != 0 && FT_Load_Glyph(face, space_gi, FT_LOAD_DEFAULT) == 0)
                g->xadvance = ((float)face->glyph->advance.x / 64.0f) * 4.0f;
            else
                g->xadvance = tier->baked_px * 2.0f;
        }
        g->valid = 1;
        return true;
    }

    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (gi == 0 && face != (FT_Face)font->regular_face) {
        FT_Face regular = (FT_Face)font->regular_face;
        if (font_set_face_size(regular, tier->baked_px)) {
            FT_UInt regular_gi = FT_Get_Char_Index(regular, cp);
            if (regular_gi != 0) {
                face = regular;
                gi = regular_gi;
            }
        }
    }
    if (gi == 0 && cp != '?')
        gi = FT_Get_Char_Index(face, '?');
    if (gi == 0) {
        g->xadvance = tier->logical_px * 0.5f * (tier->baked_px / tier->logical_px);
        g->valid = 1;
        return true;
    }

    int32_t load_flags = is_icon_range
        ? (FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)
        : (use_lcd
            ? (FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD | FT_LOAD_NO_BITMAP)
            : (FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP));
    if (!font_set_face_size(face, tier->baked_px * (float)supersample))
        return false;
    if (FT_Load_Glyph(face, gi, load_flags) != 0) return false;
    FT_GlyphSlot slot = face->glyph;
    FT_Render_Mode mode = use_lcd ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL;
    if (FT_Render_Glyph(slot, mode) != 0) return false;

    const FT_Bitmap *bmp = &slot->bitmap;
    int src_pixel_w = (bmp->pixel_mode == FT_PIXEL_MODE_LCD)
        ? (int)bmp->width / 3
        : (int)bmp->width;
    int src_pixel_h = (int)bmp->rows;
    int pixel_w = (src_pixel_w + supersample - 1) / supersample;
    int pixel_h = (src_pixel_h + supersample - 1) / supersample;

    if (pixel_w <= 0 || pixel_h <= 0) {
        g->xadvance = ((float)slot->advance.x / 64.0f) / (float)supersample;
        g->valid = 1;
        return true;
    }

    int rx, ry;
    if (!font_page_alloc(tier, pixel_w + 2, pixel_h + 2, &rx, &ry)) {
        return false;
    }
    rx += 1;
    ry += 1;

    if (bmp->pixel_mode == FT_PIXEL_MODE_LCD)
        blit_lcd(font->atlas_rgba, font->atlas_w, rx, ry, bmp);
    else
        blit_gray_reconstructed(font->atlas_rgba, font->atlas_w, rx, ry, bmp,
                                supersample,
                                font_should_strengthen_glyph(font, tier,
                                                             is_icon_range));

    g->x0 = (uint16_t)rx;
    g->y0 = (uint16_t)ry;
    g->x1 = (uint16_t)(rx + pixel_w);
    g->y1 = (uint16_t)(ry + pixel_h);
    g->xoff  = (float)slot->bitmap_left / (float)supersample;
    g->yoff  = (float)(-slot->bitmap_top) / (float)supersample;
    g->xoff2 = g->xoff + (float)pixel_w;
    g->yoff2 = g->yoff + (float)pixel_h;
    g->xadvance = ((float)slot->advance.x / 64.0f) / (float)supersample;
    g->valid = 1;

    font_mark_dirty(font, (uint16_t)(rx - 1), (uint16_t)(ry - 1),
                    (uint16_t)(pixel_w + 2), (uint16_t)(pixel_h + 2));
    return true;
}

Ca_FontTier *ca_font_select_tier_for_size(Ca_Font *font,
                                          float desired_px,
                                          bool bold)
{
    if (!font) return NULL;
    uint8_t style = (bold && font->bold_face) ? CA_FONT_STYLE_BOLD : CA_FONT_STYLE_REGULAR;
    uint32_t key = font_size_key(desired_px);
    Ca_FontTier *tier = font_find_page(font, key, style);
    if (tier) return tier;
    tier = font_alloc_page(font, key, style);
    if (tier) return tier;
    tier = font_find_page(font, font_size_key(font->default_size), CA_FONT_STYLE_REGULAR);
    return tier ? tier : font_any_live_page(font);
}

Ca_Glyph *ca_font_glyph_from_tier(Ca_FontTier *tier, uint32_t cp,
                                  Ca_FontTier **out_tier)
{
    if (out_tier) *out_tier = tier;
    if (!tier) return NULL;
    Ca_Font *font = tier->owner;
    if (!font) return NULL;

    const uint32_t key = tier->size_key;
    const uint8_t style = tier->style;
    Ca_Glyph *g = font_find_rendered_glyph(font, key, style, cp, out_tier);
    if (g) return g;

    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        Ca_FontTier *page = &font->pages[i];
        if (!font_page_matches(page, key, style)) continue;
        ca_font_touch_page(font, i);
        g = font_render_glyph_on_page(page, cp, out_tier);
        if (g) return g;
    }

    Ca_FontTier *page = font_alloc_page(font, key, style);
    if (page) {
        g = font_render_glyph_on_page(page, cp, out_tier);
        if (g) return g;
    }

    fprintf(stderr, "[font] could not render/pack glyph %.1fpx style=%u cp=U+%04X\n",
            tier->logical_px, (unsigned)style, cp);
    return NULL;
}

void ca_font_begin_frame(Ca_Font *font)
{
    if (font) font->frame_counter++;
}

void ca_font_touch_page(Ca_Font *font, int page_index)
{
    if (!font || page_index < 0 || page_index >= CA_FONT_MAX_PAGES) return;
    if (font->pages[page_index].dynamic_page)
        font->pages[page_index].last_used_frame = font->frame_counter;
}

void ca_font_flush_uploads(Ca_Instance *inst, Ca_Font *font)
{
    if (!inst || !font || !font->atlas_rgba || font->image == VK_NULL_HANDLE)
        return;
    uint32_t rect_count = font->dirty_full ? 1u : font->dirty_rect_count;
    if (rect_count == 0) return;

    Ca_FontDirtyRect full = { 0, 0, (uint16_t)font->atlas_w, (uint16_t)font->atlas_h };
    Ca_FontDirtyRect *rects = font->dirty_full ? &full : font->dirty_rects;

    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < rect_count; i++)
        total += (VkDeviceSize)rects[i].w * rects[i].h * 4u;
    if (total == 0) return;

    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = total,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkResult vr = vkCreateBuffer(inst->vk_device, &buf_ci, NULL, &staging_buf);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkCreateBuffer failed for atlas upload (%d)\n", vr);
        return;
    }

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(inst->vk_device, staging_buf, &buf_req);
    uint32_t mem_type = find_memory_type(inst->vk_gpu, buf_req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "[font] no host-visible memory for atlas upload\n");
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        return;
    }
    VkMemoryAllocateInfo buf_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = buf_req.size,
        .memoryTypeIndex = mem_type,
    };
    vr = vkAllocateMemory(inst->vk_device, &buf_mem_ai, NULL, &staging_mem);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkAllocateMemory failed for atlas upload (%d)\n", vr);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        return;
    }
    vr = vkBindBufferMemory(inst->vk_device, staging_buf, staging_mem, 0);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[font] vkBindBufferMemory failed for atlas upload (%d)\n", vr);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        vkFreeMemory(inst->vk_device, staging_mem, NULL);
        return;
    }

    void *mapped = NULL;
    vr = vkMapMemory(inst->vk_device, staging_mem, 0, total, 0, &mapped);
    if (vr != VK_SUCCESS || !mapped) {
        fprintf(stderr, "[font] vkMapMemory failed for atlas upload (%d)\n", vr);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        vkFreeMemory(inst->vk_device, staging_mem, NULL);
        return;
    }
    unsigned char *dst = (unsigned char *)mapped;
    VkBufferImageCopy *copies =
        (VkBufferImageCopy *)CA_CALLOC(rect_count, sizeof(VkBufferImageCopy));
    if (!copies) {
        fprintf(stderr, "[font] copy list allocation failed for atlas upload\n");
        vkUnmapMemory(inst->vk_device, staging_mem);
        vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
        vkFreeMemory(inst->vk_device, staging_mem, NULL);
        return;
    }
    VkDeviceSize off = 0;
    for (uint32_t i = 0; i < rect_count; i++) {
        Ca_FontDirtyRect r = rects[i];
        size_t row_bytes = (size_t)r.w * 4u;
        const unsigned char *src = font->atlas_rgba +
            ((uint32_t)r.y * (uint32_t)font->atlas_w + (uint32_t)r.x) * 4u;
        for (uint16_t y = 0; y < r.h; y++)
            memcpy(dst + off + (VkDeviceSize)y * row_bytes,
                   src + (size_t)y * (size_t)font->atlas_w * 4u,
                   row_bytes);
        copies[i] = (VkBufferImageCopy){
            .bufferOffset = off,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { r.x, r.y, 0 },
            .imageExtent = { r.w, r.h, 1 },
        };
        off += (VkDeviceSize)r.w * r.h * 4u;
    }
    vkUnmapMemory(inst->vk_device, staging_mem);

    VkCommandBuffer cmd = begin_once(inst);
    VkImageMemoryBarrier bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = font->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    vkCmdCopyBufferToImage(cmd, staging_buf, font->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           rect_count, copies);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);
    end_once(inst, cmd);

    CA_FREE(copies);
    vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
    vkFreeMemory(inst->vk_device, staging_mem, NULL);
    font->dirty_rect_count = 0;
    font->dirty_full = false;
    font->atlas_generation++;
}

/* ============================================================
   Top-level creation
   ============================================================ */

static bool font_create_internal(Ca_Instance *inst, GLFWwindow *glfw_win,
                                 Ca_Font *out_font,
                                 const unsigned char *regular_data,
                                 size_t               regular_size,
                                 const unsigned char *bold_data,
                                 size_t               bold_size)
{
    memset(out_font, 0, sizeof(*out_font));

    float cx = 1.0f;
    glfwGetWindowContentScale(glfw_win, &cx, NULL);
    if (!(cx > 0.0f)) cx = 1.0f;
    out_font->display_scale = cx;
    out_font->content_scale = cx;
    out_font->default_size  = CA_FONT_DEFAULT_SIZE_PX;
    out_font->owner         = inst;
    out_font->atlas_w       = CA_FONT_ATLAS_W;
    out_font->atlas_h       = CA_FONT_ATLAS_H;

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib) != 0) {
        fprintf(stderr, "[font] FT_Init_FreeType failed\n");
        return false;
    }
    FT_Library_SetLcdFilter(lib, FT_LCD_FILTER_DEFAULT);

    out_font->regular_data = (unsigned char *)CA_MALLOC(regular_size);
    if (!out_font->regular_data) {
        FT_Done_FreeType(lib);
        return false;
    }
    memcpy(out_font->regular_data, regular_data, regular_size);
    out_font->regular_size = regular_size;

    FT_Face regular_face = NULL;
    if (FT_New_Memory_Face(lib, out_font->regular_data,
                           (FT_Long)out_font->regular_size,
                           0, &regular_face) != 0) {
        fprintf(stderr, "[font] FT_New_Memory_Face regular failed\n");
        CA_FREE(out_font->regular_data);
        FT_Done_FreeType(lib);
        memset(out_font, 0, sizeof(*out_font));
        return false;
    }
    out_font->regular_face = regular_face;

    if (bold_data && bold_size > 0) {
        out_font->bold_data = (unsigned char *)CA_MALLOC(bold_size);
        if (out_font->bold_data) {
            memcpy(out_font->bold_data, bold_data, bold_size);
            out_font->bold_size = bold_size;
            FT_Face bold_face = NULL;
            if (FT_New_Memory_Face(lib, out_font->bold_data,
                                   (FT_Long)out_font->bold_size,
                                   0, &bold_face) == 0) {
                out_font->bold_face = bold_face;
            } else {
                fprintf(stderr, "[font] FT_New_Memory_Face bold failed; using regular\n");
                CA_FREE(out_font->bold_data);
                out_font->bold_data = NULL;
                out_font->bold_size = 0;
            }
        }
    }

    extern const unsigned char ca_embedded_symbols_font_data[];
    extern const unsigned int  ca_embedded_symbols_font_size;
    if (ca_embedded_symbols_font_size > 0u) {
        out_font->icon_data =
            (unsigned char *)CA_MALLOC(ca_embedded_symbols_font_size);
        if (out_font->icon_data) {
            memcpy(out_font->icon_data, ca_embedded_symbols_font_data,
                   ca_embedded_symbols_font_size);
            out_font->icon_size = ca_embedded_symbols_font_size;
            FT_Face icon_face = NULL;
            if (FT_New_Memory_Face(lib, out_font->icon_data,
                                   (FT_Long)out_font->icon_size,
                                   0, &icon_face) == 0) {
                out_font->icon_face = icon_face;
            } else {
                fprintf(stderr, "[font] FT_New_Memory_Face icons failed; using regular\n");
                CA_FREE(out_font->icon_data);
                out_font->icon_data = NULL;
                out_font->icon_size = 0;
            }
        }
    }

    out_font->ft_library = lib;

    size_t atlas_bytes = (size_t)out_font->atlas_w * out_font->atlas_h * 4u;
    out_font->atlas_rgba = (unsigned char *)CA_CALLOC(1, atlas_bytes);
    if (!out_font->atlas_rgba) {
        ca_font_destroy(inst, out_font);
        return false;
    }

    if (!upload_atlas(inst, out_font, out_font->atlas_rgba)) {
        ca_font_destroy(inst, out_font);
        return false;
    }

    /* Prime the default regular page so callers kept for compatibility
       (ca_font_select_tier/ca_font_tier) always have a valid fallback. */
    if (!ca_font_select_tier_for_size(out_font, out_font->default_size, false)) {
        fprintf(stderr, "[font] failed to initialise default font page\n");
        ca_font_destroy(inst, out_font);
        memset(out_font, 0, sizeof(*out_font));
        return false;
    }

    printf("[font] FreeType dynamic atlas %dx%d, %d LRU pages of %dpx, "
           "display_scale=%.1fx, atlas_scale=%.1fx\n",
           out_font->atlas_w, out_font->atlas_h,
           CA_FONT_MAX_PAGES, CA_FONT_PAGE_SIZE,
           out_font->display_scale, out_font->content_scale);
    return true;
}

bool ca_font_create(Ca_Instance *inst, GLFWwindow *glfw_win,
                    Ca_Font *out_font,
                    const char *regular_path, const char *bold_path)
{
    unsigned char *regular_buf = NULL;
    unsigned char *bold_buf    = NULL;
    long regular_sz = 0;
    long bold_sz    = 0;

    FILE *f = fopen(regular_path, "rb");
    if (!f) {
        fprintf(stderr, "[font] cannot open regular font: %s\n", regular_path);
        return false;
    }
    fseek(f, 0, SEEK_END); regular_sz = ftell(f); rewind(f);
    regular_buf = (unsigned char *)CA_MALLOC((size_t)regular_sz);
    if (!regular_buf) { fclose(f); return false; }
    fread(regular_buf, 1, (size_t)regular_sz, f);
    fclose(f);

    if (bold_path) {
        f = fopen(bold_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); bold_sz = ftell(f); rewind(f);
            bold_buf = (unsigned char *)CA_MALLOC((size_t)bold_sz);
            if (bold_buf) fread(bold_buf, 1, (size_t)bold_sz, f);
            fclose(f);
        }
    }

    bool ok = font_create_internal(inst, glfw_win, out_font,
                                   regular_buf, (size_t)regular_sz,
                                   bold_buf,    (size_t)bold_sz);
    CA_FREE(regular_buf);
    CA_FREE(bold_buf);
    return ok;
}

bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *regular_data, unsigned int regular_size,
                                const unsigned char *bold_data,    unsigned int bold_size)
{
    return font_create_internal(inst, glfw_win, out_font,
                                regular_data, (size_t)regular_size,
                                bold_data,    (size_t)bold_size);
}

void ca_font_destroy(Ca_Instance *inst, Ca_Font *font)
{
    if (!font) return;
    if (inst && font->image != VK_NULL_HANDLE)
        vkDeviceWaitIdle(inst->vk_device);
    if (inst && font->sampler != VK_NULL_HANDLE)
        vkDestroySampler(inst->vk_device, font->sampler, NULL);
    if (inst && font->view != VK_NULL_HANDLE)
        vkDestroyImageView(inst->vk_device, font->view, NULL);
    if (inst && font->image != VK_NULL_HANDLE)
        vkDestroyImage(inst->vk_device, font->image, NULL);
    if (inst && font->memory != VK_NULL_HANDLE)
        vkFreeMemory(inst->vk_device, font->memory, NULL);
    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        CA_FREE(font->pages[i].chardata_block);
        CA_FREE(font->pages[i].extra_glyphs);
        CA_FREE(font->pages[i].extra_lookup);
    }
    if (font->bold_face)
        FT_Done_Face((FT_Face)font->bold_face);
    if (font->icon_face)
        FT_Done_Face((FT_Face)font->icon_face);
    if (font->regular_face)
        FT_Done_Face((FT_Face)font->regular_face);
    if (font->ft_library)
        FT_Done_FreeType((FT_Library)font->ft_library);
    CA_FREE(font->regular_data);
    CA_FREE(font->bold_data);
    CA_FREE(font->icon_data);
    CA_FREE(font->atlas_rgba);
    memset(font, 0, sizeof(*font));
}
