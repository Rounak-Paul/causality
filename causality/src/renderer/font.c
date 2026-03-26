/* font.c — multi-size font atlas creation and GPU upload */

#define STB_TRUETYPE_IMPLEMENTATION
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

/* Tier logical pixel sizes — chosen for common UI text sizes.
   10 tiers fit reliably in a 4096² atlas with 1×1 oversampling. */
static const float g_tier_sizes[CA_FONT_TIER_COUNT] = {
    10, 12, 13, 14, 15, 16, 18, 20, 24, 32
};

/* Codepoint range definitions */
static const struct { int first; int count; } g_range_defs[CA_FONT_RANGE_COUNT] = {
    { 32,     224 },   /* ASCII + Latin-1 Supplement  (32–255)     */
    { 0xE0A0,  56 },   /* Powerline + extras          (E0A0–E0D7) */
    { 0xE5FA, 188 },   /* Seti-UI + Custom            (E5FA–E6B5) */
    { 0xE700, 198 },   /* Devicons                    (E700–E7C5) */
    { 0xEA60, 447 },   /* Codicons                    (EA60–EC1E) */
    { 0xF000, 737 },   /* Font Awesome                (F000–F2E0) */
};

static int chars_per_tier(void)
{
    int n = 0;
    for (int i = 0; i < CA_FONT_RANGE_COUNT; i++)
        n += g_range_defs[i].count;
    return n;
}

/* ---- GPU helpers ---- */

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

static bool upload_atlas(Ca_Instance *inst, Ca_Font *font,
                          const unsigned char *bitmap)
{
    VkDeviceSize atlas_sz = (VkDeviceSize)font->atlas_w * font->atlas_h;

    /* VkImage (R8_UNORM, device-local) */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8_UNORM,
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

    /* Staging buffer */
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
    memcpy(mapped, bitmap, (size_t)atlas_sz);
    vkUnmapMemory(inst->vk_device, staging_mem);

    /* Upload */
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

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = font->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCreateImageView(inst->vk_device, &view_ci, NULL, &font->view);

    /* Sampler */
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

/* ---- Core creation ---- */

static bool font_create_internal(Ca_Instance *inst, GLFWwindow *glfw_win,
                                 Ca_Font *out_font,
                                 const unsigned char *text_data,
                                 size_t text_size,
                                 const unsigned char *icon_data,
                                 size_t icon_size,
                                 float font_px)
{
    (void)text_size;
    (void)icon_size;
    memset(out_font, 0, sizeof(*out_font));

    float cx = 1.0f;
    glfwGetWindowContentScale(glfw_win, &cx, NULL);
    out_font->content_scale = cx;
    out_font->default_size  = font_px;

    /* Use text font for metrics; icon_data defaults to text_data if NULL */
    const unsigned char *icon_font = icon_data ? icon_data : text_data;

    stbtt_fontinfo info;
    memset(&info, 0, sizeof(info));
    if (!stbtt_InitFont(&info, text_data,
                        stbtt_GetFontOffsetForIndex(text_data, 0)))
        return false;

    int cpt = chars_per_tier();

    /* Allocate chardata for each tier */
    for (int t = 0; t < CA_FONT_TIER_COUNT; t++) {
        Ca_FontTier *tier = &out_font->tiers[t];
        tier->logical_px = g_tier_sizes[t];
        tier->baked_px   = (float)(int)(tier->logical_px * cx + 0.5f);
        if (tier->baked_px < 8) tier->baked_px = 8;

        float scale = stbtt_ScaleForPixelHeight(&info, tier->baked_px);
        int a, d, lg;
        stbtt_GetFontVMetrics(&info, &a, &d, &lg);
        tier->ascent   = (float)a  * scale / cx;
        tier->descent  = (float)d  * scale / cx;
        tier->line_gap = (float)lg * scale / cx;

        tier->chardata_block = (stbtt_packedchar *)calloc(
            (size_t)cpt, sizeof(stbtt_packedchar));
        if (!tier->chardata_block) goto fail;

        int offset = 0;
        for (int r = 0; r < CA_FONT_RANGE_COUNT; r++) {
            tier->ranges[r].first_codepoint = g_range_defs[r].first;
            tier->ranges[r].num_chars       = g_range_defs[r].count;
            tier->ranges[r].chardata        = tier->chardata_block + offset;
            offset += g_range_defs[r].count;
        }
    }

    /* Pack all tiers into one atlas */
    out_font->atlas_w = 4096;
    out_font->atlas_h = 4096;
    size_t bmp_sz = (size_t)out_font->atlas_w * out_font->atlas_h;
    unsigned char *bitmap = (unsigned char *)calloc(1, bmp_sz);
    if (!bitmap) goto fail;

    stbtt_pack_context ctx;
    stbtt_PackBegin(&ctx, bitmap, out_font->atlas_w, out_font->atlas_h,
                    0, 2, NULL);
    stbtt_PackSetSkipMissingCodepoints(&ctx, 1);

    for (int t = 0; t < CA_FONT_TIER_COUNT; t++) {
        Ca_FontTier *tier = &out_font->tiers[t];

        /* 1x1 oversampling: glyph origins are snapped to physical pixels at
           render time (see paint.c), so each atlas texel maps to exactly one
           screen pixel. Sub-pixel phase is encoded in the coverage values by
           the grayscale rasterizer — no multi-column decode needed. */
        stbtt_PackSetOversampling(&ctx, 1, 1);
        stbtt_pack_range text_ranges[CA_FONT_TEXT_RANGES];
        for (int r = 0; r < CA_FONT_TEXT_RANGES; r++) {
            text_ranges[r].font_size                        = tier->baked_px;
            text_ranges[r].first_unicode_codepoint_in_range = tier->ranges[r].first_codepoint;
            text_ranges[r].num_chars                        = tier->ranges[r].num_chars;
            text_ranges[r].chardata_for_range               = tier->ranges[r].chardata;
            text_ranges[r].array_of_unicode_codepoints      = NULL;
        }
        stbtt_PackFontRanges(&ctx, text_data, 0,
                             text_ranges, CA_FONT_TEXT_RANGES);

        /* Icon ranges: 1x1 oversampling — icons are simple shapes,
           no subpixel precision needed, saves major atlas space. */
        stbtt_PackSetOversampling(&ctx, 1, 1);
        stbtt_pack_range icon_ranges[CA_FONT_ICON_RANGES];
        for (int r = 0; r < CA_FONT_ICON_RANGES; r++) {
            int ri = CA_FONT_TEXT_RANGES + r;
            icon_ranges[r].font_size                        = tier->baked_px;
            icon_ranges[r].first_unicode_codepoint_in_range = tier->ranges[ri].first_codepoint;
            icon_ranges[r].num_chars                        = tier->ranges[ri].num_chars;
            icon_ranges[r].chardata_for_range               = tier->ranges[ri].chardata;
            icon_ranges[r].array_of_unicode_codepoints      = NULL;
        }
        stbtt_PackFontRanges(&ctx, icon_font, 0,
                             icon_ranges, CA_FONT_ICON_RANGES);

        /* A tier is usable only if multiple representative glyphs packed.
           If the atlas overflowed mid-tier, some glyphs have zero-size rects
           and would render as missing/cut shapes. */
        bool ok = true;
        const char test_chars[] = "AaMmWw.,:;";
        for (int i = 0; test_chars[i]; i++) {
            stbtt_packedchar *pc = &tier->ranges[0].chardata[test_chars[i] - 32];
            if (pc->x1 <= pc->x0) { ok = false; break; }
        }
        tier->packed = ok;
    }

    stbtt_PackEnd(&ctx);

    if (!upload_atlas(inst, out_font, bitmap)) {
        free(bitmap);
        goto fail;
    }
    free(bitmap);

    int packed_count = 0;
    for (int t = 0; t < CA_FONT_TIER_COUNT; t++)
        if (out_font->tiers[t].packed) packed_count++;

    printf("[font] atlas %dx%d, %d/%d tiers packed (scale=%.1f, text=1x1-snap, icons=1x1%s)\n",
           out_font->atlas_w, out_font->atlas_h,
           packed_count, CA_FONT_TIER_COUNT, cx,
           icon_data ? ", dual-font" : "");
    return true;

fail:
    for (int t = 0; t < CA_FONT_TIER_COUNT; t++)
        free(out_font->tiers[t].chardata_block);
    memset(out_font, 0, sizeof(*out_font));
    return false;
}

bool ca_font_create(Ca_Instance *inst, GLFWwindow *glfw_win,
                    Ca_Font *out_font, const char *path, float font_px)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[font] cannot open: %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    rewind(f);
    unsigned char *ttf = (unsigned char *)malloc((size_t)file_sz);
    if (!ttf) { fclose(f); return false; }
    fread(ttf, 1, (size_t)file_sz, f);
    fclose(f);

    bool ok = font_create_internal(inst, glfw_win, out_font,
                                   ttf, (size_t)file_sz,
                                   NULL, 0, font_px);
    free(ttf);
    return ok;
}

bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *data, unsigned int data_size,
                                float font_px)
{
    return font_create_internal(inst, glfw_win, out_font,
                                data, (size_t)data_size,
                                NULL, 0, font_px);
}

bool ca_font_create_dual_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                     Ca_Font *out_font,
                                     const unsigned char *text_data, unsigned int text_size,
                                     const unsigned char *icon_data, unsigned int icon_size,
                                     float font_px)
{
    return font_create_internal(inst, glfw_win, out_font,
                                text_data, (size_t)text_size,
                                icon_data, (size_t)icon_size,
                                font_px);
}

/* ---- System font detection ---- */

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
    for (int t = 0; t < CA_FONT_TIER_COUNT; t++)
        free(font->tiers[t].chardata_block);
    memset(font, 0, sizeof(*font));
}
