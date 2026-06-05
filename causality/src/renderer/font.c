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

/* Standard set of visual pixel sizes baked into the atlas at startup.
   Rendering asks for css_font_size * ui_scale, so this list deliberately
   includes larger zoom targets instead of forcing a 2x/3x UI scale to
   magnify a small glyph bitmap.  Only the default tier gets the full icon
   ranges; larger tiers are text-only and fall back for icons.          */
static const float g_std_sizes[] = {
     8.0f, 10.0f, 12.0f, 14.0f, 16.0f,
    18.0f, 20.0f, 22.0f, 24.0f, 28.0f,
    32.0f, 36.0f, 40.0f, 48.0f, 56.0f,
    64.0f, 72.0f, 84.0f, 96.0f, 112.0f
};
static const int g_std_sizes_count =
    (int)(sizeof(g_std_sizes) / sizeof(g_std_sizes[0]));

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
                       float baked_px,
                       int num_ranges)  /* CA_FONT_RANGE_COUNT or CA_FONT_TEXT_RANGES */
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

    int cpt = chars_for_ranges(num_ranges);
    tier->chardata_block = (Ca_Glyph *)CA_CALLOC((size_t)cpt, sizeof(Ca_Glyph));
    if (!tier->chardata_block) {
        FT_Done_Face(face);
        return false;
    }

    int offset = 0;
    for (int r = 0; r < num_ranges; r++) {
        tier->ranges[r].first_codepoint = g_range_defs[r].first;
        tier->ranges[r].num_chars       = g_range_defs[r].count;
        tier->ranges[r].chardata        = tier->chardata_block + offset;
        offset += g_range_defs[r].count;
    }
    /* Ranges beyond num_ranges are not baked — zero them so ca_font_glyph
       skips them cleanly in the lookup loop.                               */
    for (int r = num_ranges; r < CA_FONT_RANGE_COUNT; r++) {
        tier->ranges[r].first_codepoint = 0;
        tier->ranges[r].num_chars       = 0;
        tier->ranges[r].chardata        = NULL;
    }

    int packed_count = 0;
    for (int ri = 0; ri < num_ranges; ri++) {
        Ca_GlyphRange *range = &tier->ranges[ri];
        bool is_icon_range = (ri >= CA_FONT_TEXT_RANGES);

        /* Grayscale antialiasing for all glyph ranges.  Icons skip hinting
           because large symbolic glyphs look better unhinted; text uses
           FreeType's auto-hinter which produces consistent stems across
           platforms without depending on embedded TrueType hint programs. */
        int32_t load_flags = is_icon_range
            ? (FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_RENDER)
            : (FT_LOAD_DEFAULT | FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT);
        FT_Render_Mode render_mode = FT_RENDER_MODE_NORMAL;

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
                                 size_t               bold_size)
{
    memset(out_font, 0, sizeof(*out_font));

    float cx = 1.0f;
    glfwGetWindowContentScale(glfw_win, &cx, NULL);
    out_font->content_scale = cx;
    out_font->default_size  = CA_FONT_DEFAULT_SIZE_PX;

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib) != 0) {
        fprintf(stderr, "[font] FT_Init_FreeType failed\n");
        return false;
    }
    /* Grayscale antialiasing — no LCD filter needed.  Subpixel rendering
       produces color fringing under composited window managers (Wayland,
       X11 compositors) and depends on display subpixel order, so grayscale
       is used for portability and correctness on all platforms.         */

    /* Atlas: 4096x4096 RGBA8 holds all baked size tiers.  Icons (Nerd-Font
       ranges) are baked only at CA_FONT_DEFAULT_SIZE_PX; all other sizes
       carry text-only (ASCII+Latin-1) and fall back to the icon tier via
       Ca_FontTier::icon_fallback for codepoints outside their ranges.   */
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

    /* Collect the size set: always include CA_FONT_DEFAULT_SIZE_PX (the icon
       tier), then fill from the standard list until we reach CA_FONT_MAX_SIZES.
       Sort ascending so the log output is tidy.                              */
    float size_list[CA_FONT_MAX_SIZES];
    int   size_count = 0;
    size_list[size_count++] = CA_FONT_DEFAULT_SIZE_PX;
    for (int i = 0; i < g_std_sizes_count && size_count < CA_FONT_MAX_SIZES; i++) {
        float s = g_std_sizes[i];
        bool already = false;
        for (int j = 0; j < size_count; j++)
            if (fabsf(size_list[j] - s) < 0.5f) { already = true; break; }
        if (!already) size_list[size_count++] = s;
    }
    /* Bubble-sort ascending (tiny array). */
    for (int i = 0; i < size_count - 1; i++)
        for (int j = i + 1; j < size_count; j++)
            if (size_list[i] > size_list[j]) {
                float tmp = size_list[i]; size_list[i] = size_list[j]; size_list[j] = tmp;
            }

    /* Bake every size in the list into the shared atlas. */
    bool any_reg_ok = false;
    for (int si = 0; si < size_count; si++) {
        float logical = size_list[si];
        float baked   = (float)(int)(logical * cx + 0.5f);
        if (baked < 8.0f) baked = 8.0f;

        /* Only CA_FONT_DEFAULT_SIZE_PX gets all icon/Nerd-Font ranges.
           All other sizes bake text-only (ASCII+Latin-1, range 0);
           icon lookups fall back via Ca_FontTier::icon_fallback.     */
        int num_ranges = (fabsf(logical - CA_FONT_DEFAULT_SIZE_PX) < 0.5f)
                         ? CA_FONT_RANGE_COUNT : CA_FONT_TEXT_RANGES;

        Ca_FontTier *reg  = &out_font->size_tiers[si][CA_FONT_STYLE_REGULAR];
        Ca_FontTier *bold = &out_font->size_tiers[si][CA_FONT_STYLE_BOLD];

        reg->logical_px = logical;
        reg->baked_px   = baked;
        bool reg_ok = bake_style(lib, &packer, reg,
                                 regular_data, regular_size, baked, num_ranges);
        if (reg_ok) {
            float to_logical = logical / baked;
            reg->ascent   *= to_logical;
            reg->descent  *= to_logical;
            reg->line_gap *= to_logical;
            any_reg_ok = true;
        }

        if (bold_data && bold_size > 0) {
            bold->logical_px = logical;
            bold->baked_px   = baked;
            bool bold_ok = bake_style(lib, &packer, bold,
                                      bold_data, bold_size, baked, num_ranges);
            if (bold_ok) {
                float to_logical = logical / baked;
                bold->ascent   *= to_logical;
                bold->descent  *= to_logical;
                bold->line_gap *= to_logical;
            }
        }

        out_font->baked_logicals[si] = logical;
    }
    out_font->baked_size_count = size_count;

    /* Wire icon_fallback: find the icon tier slot (CA_FONT_DEFAULT_SIZE_PX)
       then point every text-only tier at it so ca_font_glyph can fall back
       for icon codepoints without knowing which tier has them.             */
    int icon_si = 0;
    for (int si = 0; si < size_count; si++) {
        if (fabsf(size_list[si] - CA_FONT_DEFAULT_SIZE_PX) < 0.5f) {
            icon_si = si; break;
        }
    }
    for (int si = 0; si < size_count; si++) {
        if (si == icon_si) continue;
        for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
            out_font->size_tiers[si][s].icon_fallback =
                &out_font->size_tiers[icon_si][s];
    }

    FT_Done_FreeType(lib);

    if (!any_reg_ok) {
        fprintf(stderr, "[font] no regular tier succeeded in baking\n");
        CA_FREE(atlas);
        for (int si = 0; si < size_count; si++)
            for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
                CA_FREE(out_font->size_tiers[si][s].chardata_block);
        memset(out_font, 0, sizeof(*out_font));
        return false;
    }

    if (!upload_atlas(inst, out_font, atlas)) {
        CA_FREE(atlas);
        for (int si = 0; si < size_count; si++)
            for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
                CA_FREE(out_font->size_tiers[si][s].chardata_block);
        memset(out_font, 0, sizeof(*out_font));
        return false;
    }
    CA_FREE(atlas);

    printf("[font] FreeType+LCD atlas %dx%d, %d sizes (%.0f..%.0fpx) "
           "scale=%.1fx\n",
           out_font->atlas_w, out_font->atlas_h, size_count,
           size_list[0], size_list[size_count - 1], cx);
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
    for (int si = 0; si < font->baked_size_count; si++)
        for (int s = 0; s < CA_FONT_STYLE_COUNT; s++)
            CA_FREE(font->size_tiers[si][s].chardata_block);
    memset(font, 0, sizeof(*font));
}
