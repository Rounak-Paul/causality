/* font.c — font atlas creation and GPU upload */

#define STB_TRUETYPE_IMPLEMENTATION
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */

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

/* ---- Public API ---- */

bool ca_font_create(Ca_Instance *inst, GLFWwindow *glfw_win,
                    Ca_Font *out_font, const char *path, float font_px)
{
    memset(out_font, 0, sizeof(*out_font));

    /* Query DPI scale */
    float cx = 1.0f;
    glfwGetWindowContentScale(glfw_win, &cx, NULL);
    out_font->content_scale = cx;
    out_font->baked_px      = (float)(int)(font_px * cx + 0.5f);

    /* Load font file */
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

    /* Extract metrics (must be done before freeing ttf) */
    stbtt_fontinfo info = {0};
    stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    float scale = stbtt_ScaleForPixelHeight(&info, out_font->baked_px);
    {
        int a, d, lg;
        stbtt_GetFontVMetrics(&info, &a, &d, &lg);
        out_font->ascent   = (float)a  * scale / cx;
        out_font->descent  = (float)d  * scale / cx;
        out_font->line_gap = (float)lg * scale / cx;
    }

    /* Bake a greyscale atlas */
    out_font->atlas_w = 512;
    out_font->atlas_h = 512;
    unsigned char *bitmap =
        (unsigned char *)calloc(1, (size_t)(out_font->atlas_w * out_font->atlas_h));

    int rows = stbtt_BakeFontBitmap(ttf, 0, out_font->baked_px,
                                    bitmap,
                                    out_font->atlas_w, out_font->atlas_h,
                                    CA_FONT_GLYPH_FIRST, CA_FONT_GLYPH_COUNT,
                                    out_font->glyphs);
    free(ttf);

    if (rows <= 0) {
        fprintf(stderr, "[font] stbtt_BakeFontBitmap failed (rows=%d), "
                "try increasing atlas size.\n", rows);
        free(bitmap);
        return false;
    }

    /* ---- Create VkImage (R8_UNORM, device-local) ---- */
    VkDeviceSize atlas_sz = (VkDeviceSize)(out_font->atlas_w * out_font->atlas_h);

    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8_UNORM,
        .extent        = { (uint32_t)out_font->atlas_w, (uint32_t)out_font->atlas_h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vkCreateImage(inst->vk_device, &img_ci, NULL, &out_font->image);

    VkMemoryRequirements img_req;
    vkGetImageMemoryRequirements(inst->vk_device, out_font->image, &img_req);
    VkMemoryAllocateInfo img_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = img_req.size,
        .memoryTypeIndex = find_memory_type(inst->vk_gpu, img_req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    vkAllocateMemory(inst->vk_device, &img_mem_ai, NULL, &out_font->memory);
    vkBindImageMemory(inst->vk_device, out_font->image, out_font->memory, 0);

    /* ---- Staging buffer (host-visible) ---- */
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
    free(bitmap);

    /* ---- Upload via one-shot command buffer ---- */
    VkCommandBuffer cmd = begin_once(inst);

    /* UNDEFINED → TRANSFER_DST */
    VkImageMemoryBarrier bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = out_font->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { (uint32_t)out_font->atlas_w,
                                (uint32_t)out_font->atlas_h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging_buf, out_font->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    /* TRANSFER_DST → SHADER_READ_ONLY */
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    end_once(inst, cmd);

    /* Free staging */
    vkDestroyBuffer(inst->vk_device, staging_buf, NULL);
    vkFreeMemory(inst->vk_device, staging_mem, NULL);

    /* ---- Image view (R8, colour) ---- */
    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = out_font->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCreateImageView(inst->vk_device, &view_ci, NULL, &out_font->view);

    /* ---- Sampler (linear, clamp-to-edge) ---- */
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
    vkCreateSampler(inst->vk_device, &samp_ci, NULL, &out_font->sampler);

    printf("[font] atlas %dx%d, baked %.0fpx (scale=%.1f), "
           "ascent=%.1f descent=%.1f\n",
           out_font->atlas_w, out_font->atlas_h,
           out_font->baked_px, cx,
           out_font->ascent, out_font->descent);
    return true;
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
    memset(font, 0, sizeof(*font));
}
