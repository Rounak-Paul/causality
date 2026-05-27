// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* font.c — high-quality glyph atlas baked with FreeType.

   Rendering recipe (per glyph):
     1. FT_Load_Glyph(face, gid, FT_LOAD_TARGET_LCD | FT_LOAD_RENDER)
        - autohinter on (FT_LOAD_FORCE_AUTOHINT) for consistent
          rendering across faces that may lack good native hints.
        - FT_LOAD_TARGET_LCD picks the LCD-tuned hinting algorithm.
     2. FT_Render_Glyph(slot, FT_RENDER_MODE_LCD)
        - rasterises at 3x horizontal resolution.
        - applies the library's lcd_filter (we set FT_LCD_FILTER_DEFAULT)
          which is the [1,4,7,4,1]/17 ClearType-style filter.
        - output bitmap has pixel_mode = FT_PIXEL_MODE_LCD; width is
          glyph_pixels*3, each consecutive 3 bytes are R,G,B coverage.
     3. Pack into a 4096x4096 RGBA8 shelf-packed atlas:
          R = red subpixel coverage
          G = green subpixel coverage
          B = blue subpixel coverage
          A = luma-weighted coverage (used as fallback / dual-source alpha)

   The shader samples this atlas as plain RGBA, treats .rgb as per-channel
   coverage, and uses dual-source blending so the framebuffer receives
   per-subpixel blending in linear light against an sRGB target.         */

#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_MODULE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

/* Codepoint ranges baked at startup.  Layout mirrors the previous
   stb-based packer so the atlas can hold ASCII/Latin-1 plus the
   Nerd-Font icon ranges used by the editor.                       */
static const struct { int first; int count; } g_range_defs[CA_FONT_RANGE_COUNT] = {
    { 32,     224 },   /* ASCII + Latin-1 Supplement  (32-255)     */
    { 0xE0A0,  56 },   /* Powerline + extras          (E0A0-E0D7) */
    { 0xE5FA, 188 },   /* Seti-UI + Custom            (E5FA-E6B5) */
    { 0xE700, 198 },   /* Devicons                    (E700-E7C5) */
    { 0xEA60, 447 },   /* Codicons                    (EA60-EC1E) */
    { 0xF000, 737 },   /* Font Awesome                (F000-F2E0) */
};

static int chars_per_style(void)
{
    int n = 0;
    for (int i = 0; i < CA_FONT_RANGE_COUNT; i++)
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
    vkCreateImage(inst->vk_device, &img_ci, NULL, &font->image);

    VkMemoryRequirements img_req;
    vkGetImageMemoryRequirements(inst->vk_device, font->image, &img_req);
    VkMemoryAllocateInfo img_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = img_req.size,
        .memoryTypeIndex = find_memory_type(inst->vk_gpu, img_req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    vkAllocateMemory(inst->vk_device, &img_mem_ai, NULL, &font->memory);
    vkBindImageMemory(inst->vk_device, font->image, font->memory, 0);

    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = atlas_sz,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging_buf;
    VkDeviceMemory staging_mem;
    vkCreateBuffer(inst->vk_device, &buf_ci, NULL, &staging_buf);

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(inst->vk_device, staging_buf, &buf_req);
    VkMemoryAllocateInfo buf_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = buf_req.size,
        .memoryTypeIndex =
            find_memory_type(inst->vk_gpu, buf_req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    vkAllocateMemory(inst->vk_device, &buf_mem_ai, NULL, &staging_mem);
    vkBindBufferMemory(inst->vk_device, staging_buf, staging_mem, 0);

    void *mapped;
    vkMapMemory(inst->vk_device, staging_mem, 0, atlas_sz, 0, &mapped);
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
    vkCreateImageView(inst->vk_device, &view_ci, NULL, &font->view);

    /* LINEAR filtering on the atlas gives bilinear interpolation of the
       LCD coverage values when glyphs are positioned at fractional pen
       offsets — essential for crisp sub-pixel text positioning.        */
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
    vkCreateSampler(inst->vk_device, &samp_ci, NULL, &font->sampler);
    return true;
}

/* ============================================================
   Shelf packer
   ============================================================ */

typedef struct ShelfPacker {
    int           width, height;
    int           shelf_y;       /* top of the active shelf */
    int           shelf_h;       /* height of the active shelf */
    int           shelf_x;       /* pen x within the active shelf */
    unsigned char *atlas_rgba;   /* width * height * 4 */
} ShelfPacker;

static bool shelf_alloc(ShelfPacker *p, int w, int h, int *out_x, int *out_y)
{
    if (w <= 0 || h <= 0)      return false;
    if (w > p->width)          return false;
    if (h > p->height)         return false;

    /* Open a new shelf if the current one is full or too short. */
    if (p->shelf_x + w > p->width || h > p->shelf_h) {
        int new_top = p->shelf_y + p->shelf_h;
        if (new_top + h > p->height) return false;
        p->shelf_y = new_top;
        p->shelf_h = h;
        p->shelf_x = 0;
    }
    *out_x = p->shelf_x;
    *out_y = p->shelf_y;
    p->shelf_x += w;
    return true;
}

/* Copy a FreeType LCD bitmap into the atlas at (dst_x, dst_y) with a
   1-pixel transparent gutter on all sides (already accounted for by the
   caller's width/height reservation).  Source pixel_mode is LCD: each
   atlas pixel consumes 3 source bytes (R,G,B subpixel coverage).
   For RGBA: .r/.g/.b = subpixel coverage, .a = luma weight.            */
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

/* Blit a normal grayscale FreeType bitmap (icons rendered without LCD)
   into all RGB channels of the atlas so the LCD shader still produces
   sensible output for those glyphs.                                    */
static void blit_gray(unsigned char *atlas, int atlas_w,
                       int dst_x, int dst_y,
                       const FT_Bitmap *src)
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
            d[0] = v; d[1] = v; d[2] = v; d[3] = v;
            d += 4;
        }
        row += src_pitch;
    }
}

/* ============================================================
   Per-style baking
   ============================================================ */

static bool bake_style(FT_Library lib, ShelfPacker *packer,
                       Ca_FontTier *tier,
                       const unsigned char *font_data, size_t font_size,
                       float baked_px)
{
    FT_Face face = NULL;
    FT_Error err = FT_New_Memory_Face(lib, font_data, (FT_Long)font_size, 0, &face);
    if (err) {
        fprintf(stderr, "[font] FT_New_Memory_Face failed: %d\n", err);
        return false;
    }
    err = FT_Set_Pixel_Sizes(face, 0, (FT_UInt)baked_px);
    if (err) {
        fprintf(stderr, "[font] FT_Set_Pixel_Sizes(%u) failed: %d\n",
                (unsigned)baked_px, err);
        FT_Done_Face(face);
        return false;
    }

    /* Vertical metrics from the face (already in 26.6 pixel units). */
    float ascent   = (float)face->size->metrics.ascender   / 64.0f;
    float descent  = (float)face->size->metrics.descender  / 64.0f;
    float height   = (float)face->size->metrics.height     / 64.0f;
    tier->ascent   =  ascent;
    tier->descent  =  descent;
    tier->line_gap =  height - (ascent - descent);

    int cpt = chars_per_style();
    tier->chardata_block = (Ca_Glyph *)CA_CALLOC((size_t)cpt, sizeof(Ca_Glyph));
    if (!tier->chardata_block) {
        FT_Done_Face(face);
        return false;
    }

    int offset = 0;
    for (int r = 0; r < CA_FONT_RANGE_COUNT; r++) {
        tier->ranges[r].first_codepoint = g_range_defs[r].first;
        tier->ranges[r].num_chars       = g_range_defs[r].count;
        tier->ranges[r].chardata        = tier->chardata_block + offset;
        offset += g_range_defs[r].count;
    }

    int packed_count = 0;
    for (int ri = 0; ri < CA_FONT_RANGE_COUNT; ri++) {
        Ca_GlyphRange *range = &tier->ranges[ri];
        bool is_icon_range = (ri >= CA_FONT_TEXT_RANGES);

        /* Use grayscale rendering for icons: they're large enough that
           LCD subpixel artifacts (color fringing on coloured backgrounds)
           outweigh the sharpness benefit, and FreeType's LCD output also
           looks wrong for symbol fonts whose stems align with subpixels. */
        int32_t load_flags = is_icon_range
            ? (FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_RENDER)
            : (FT_LOAD_TARGET_LCD | FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT);
        FT_Render_Mode render_mode = is_icon_range
            ? FT_RENDER_MODE_NORMAL
            : FT_RENDER_MODE_LCD;

        for (int i = 0; i < range->num_chars; i++) {
            uint32_t cp = (uint32_t)(range->first_codepoint + i);
            FT_UInt gi = FT_Get_Char_Index(face, cp);
            if (gi == 0) continue;

            if (FT_Load_Glyph(face, gi, load_flags) != 0) continue;
            FT_GlyphSlot slot = face->glyph;
            if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
                if (FT_Render_Glyph(slot, render_mode) != 0) continue;
            }

            const FT_Bitmap *bmp = &slot->bitmap;
            int pixel_w, pixel_h;
            if (bmp->pixel_mode == FT_PIXEL_MODE_LCD) {
                pixel_w = (int)bmp->width / 3;
                pixel_h = (int)bmp->rows;
            } else {
                pixel_w = (int)bmp->width;
                pixel_h = (int)bmp->rows;
            }

            Ca_Glyph *g = &range->chardata[i];
            /* Empty glyph (e.g. space): no atlas entry, but advance is set. */
            if (pixel_w <= 0 || pixel_h <= 0) {
                g->x0 = g->y0 = g->x1 = g->y1 = 0;
                g->xoff = g->yoff = g->xoff2 = g->yoff2 = 0.0f;
                g->xadvance = (float)slot->advance.x / 64.0f;
                packed_count++;
                continue;
            }

            /* Reserve atlas slot with 1px transparent gutter all around
               so bilinear sampling never picks up a neighbour's pixels. */
            int rx, ry;
            if (!shelf_alloc(packer, pixel_w + 2, pixel_h + 2, &rx, &ry)) {
                fprintf(stderr, "[font] atlas overflow at cp=U+%04X\n", cp);
                continue;
            }
            rx += 1; ry += 1;

            if (bmp->pixel_mode == FT_PIXEL_MODE_LCD)
                blit_lcd (packer->atlas_rgba, packer->width, rx, ry, bmp);
            else
                blit_gray(packer->atlas_rgba, packer->width, rx, ry, bmp);

            g->x0 = (uint16_t)rx;
            g->y0 = (uint16_t)ry;
            g->x1 = (uint16_t)(rx + pixel_w);
            g->y1 = (uint16_t)(ry + pixel_h);
            g->xoff  = (float)slot->bitmap_left;
            g->yoff  = (float)(-slot->bitmap_top);
            g->xoff2 = g->xoff + (float)pixel_w;
            g->yoff2 = g->yoff + (float)pixel_h;
            g->xadvance = (float)slot->advance.x / 64.0f;
            packed_count++;
        }
    }

    /* Validation: a sample of ASCII glyphs must have produced atlas entries. */
    bool ok = true;
    const char test_chars[] = "AaMmWw.,:;";
    for (int i = 0; test_chars[i]; i++) {
        Ca_Glyph *g = &tier->ranges[0].chardata[test_chars[i] - 32];
        if (g->x1 <= g->x0) { ok = false; break; }
    }
    tier->packed = ok && packed_count > 0;

    FT_Done_Face(face);
    return tier->packed;
}

/* ============================================================
   Top-level creation
   ============================================================ */

static bool font_create_internal(Ca_Instance *inst, GLFWwindow *glfw_win,
                                 Ca_Font *out_font,
                                 const unsigned char *regular_data,
                                 size_t               regular_size,
                                 const unsigned char *bold_data,
                                 size_t               bold_size,
                                 float font_px)
{
    memset(out_font, 0, sizeof(*out_font));

    float cx = 1.0f;
    glfwGetWindowContentScale(glfw_win, &cx, NULL);
    out_font->content_scale = cx;
    out_font->default_size  = font_px;

    float baked_px_f = (float)(int)(font_px * cx + 0.5f);
    if (baked_px_f < 8.0f) baked_px_f = 8.0f;

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib) != 0) {
        fprintf(stderr, "[font] FT_Init_FreeType failed\n");
        return false;
    }
    /* Default ClearType-style 5-tap horizontal filter: blends subpixel
       coverage [1,4,7,4,1]/17 to suppress most color fringing.        */
    FT_Library_SetLcdFilter(lib, FT_LCD_FILTER_DEFAULT);

    /* Atlas: 4096x4096 RGBA8 holds both tiers + Nerd Font icon ranges
       at HiDPI baking sizes (up to ~56px on a 2x display).            */
    out_font->atlas_w = 4096;
    out_font->atlas_h = 4096;
    size_t atlas_bytes = (size_t)out_font->atlas_w * out_font->atlas_h * 4;
    unsigned char *atlas = (unsigned char *)CA_CALLOC(1, atlas_bytes);
    if (!atlas) {
        FT_Done_FreeType(lib);
        return false;
    }

    ShelfPacker packer = {
        .width      = out_font->atlas_w,
        .height     = out_font->atlas_h,
        .shelf_y    = 0,
        .shelf_h    = 0,
        .shelf_x    = 0,
        .atlas_rgba = atlas,
    };

    /* logical_px is the *unscaled* size that callers reason about;
       baked_px is the rasterisation size in atlas pixels.            */
    out_font->tiers[CA_FONT_STYLE_REGULAR].logical_px = font_px;
    out_font->tiers[CA_FONT_STYLE_REGULAR].baked_px   = baked_px_f;
    out_font->tiers[CA_FONT_STYLE_BOLD   ].logical_px = font_px;
    out_font->tiers[CA_FONT_STYLE_BOLD   ].baked_px   = baked_px_f;

    bool reg_ok = bake_style(lib, &packer,
                              &out_font->tiers[CA_FONT_STYLE_REGULAR],
                              regular_data, regular_size, baked_px_f);
    bool bold_ok = false;
    if (bold_data && bold_size > 0) {
        bold_ok = bake_style(lib, &packer,
                              &out_font->tiers[CA_FONT_STYLE_BOLD],
                              bold_data, bold_size, baked_px_f);
    }

    /* Metrics are reported in baked-pixel units; layout code divides
       by font_scale = desired_size / logical_px, so we must store
       them so that (ascent - descent + line_gap) * font_scale yields
       the correct logical line height for any desired size.         */
    for (int s = 0; s < CA_FONT_STYLE_COUNT; s++) {
        Ca_FontTier *t = &out_font->tiers[s];
        /* Convert metrics from baked-pixel units → logical-px units. */
        float to_logical = font_px / baked_px_f;
        t->ascent   *= to_logical;
        t->descent  *= to_logical;
        t->line_gap *= to_logical;
    }

    FT_Done_FreeType(lib);

    if (!reg_ok) {
        fprintf(stderr, "[font] regular tier failed to bake\n");
        CA_FREE(atlas);
        for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
            CA_FREE(out_font->tiers[s].chardata_block);
        memset(out_font, 0, sizeof(*out_font));
        return false;
    }

    if (!upload_atlas(inst, out_font, atlas)) {
        CA_FREE(atlas);
        for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
            CA_FREE(out_font->tiers[s].chardata_block);
        memset(out_font, 0, sizeof(*out_font));
        return false;
    }
    CA_FREE(atlas);

    printf("[font] FreeType+LCD atlas %dx%d, size=%.0fpx scale=%.1f, "
           "regular=%s bold=%s\n",
           out_font->atlas_w, out_font->atlas_h, font_px, cx,
           reg_ok ? "ok" : "FAIL",
           bold_data ? (bold_ok ? "ok" : "FAIL") : "n/a");
    return true;
}

bool ca_font_create(Ca_Instance *inst, GLFWwindow *glfw_win,
                    Ca_Font *out_font,
                    const char *regular_path, const char *bold_path,
                    float font_px)
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
                                   bold_buf,    (size_t)bold_sz,
                                   font_px);
    CA_FREE(regular_buf);
    CA_FREE(bold_buf);
    return ok;
}

bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *regular_data, unsigned int regular_size,
                                const unsigned char *bold_data,    unsigned int bold_size,
                                float font_px)
{
    return font_create_internal(inst, glfw_win, out_font,
                                regular_data, (size_t)regular_size,
                                bold_data,    (size_t)bold_size,
                                font_px);
}

/* ============================================================
   System font detection
   ============================================================ */

bool ca_font_detect_system(char *out_path, size_t max_len)
{
    if (!out_path || max_len < 2) return false;

#ifdef _WIN32
    char windir[MAX_PATH];
    if (!GetWindowsDirectoryA(windir, MAX_PATH)) return false;

    const char *candidates[] = { "segoeui.ttf", "arial.ttf" };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, candidates[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            snprintf(out_path, max_len, "%s", path);
            return true;
        }
    }
#elif defined(__APPLE__)
    const char *candidates[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
    };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            snprintf(out_path, max_len, "%s", candidates[i]);
            return true;
        }
    }
#else
    const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            snprintf(out_path, max_len, "%s", candidates[i]);
            return true;
        }
    }
#endif

    return false;
}

void ca_font_destroy(Ca_Instance *inst, Ca_Font *font)
{
    if (!font || font->image == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(inst->vk_device);
    if (font->sampler != VK_NULL_HANDLE)
        vkDestroySampler(inst->vk_device, font->sampler, NULL);
    if (font->view != VK_NULL_HANDLE)
        vkDestroyImageView(inst->vk_device, font->view, NULL);
    if (font->image != VK_NULL_HANDLE)
        vkDestroyImage(inst->vk_device, font->image, NULL);
    if (font->memory != VK_NULL_HANDLE)
        vkFreeMemory(inst->vk_device, font->memory, NULL);
    for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
        CA_FREE(font->tiers[s].chardata_block);
    memset(font, 0, sizeof(*font));
}
