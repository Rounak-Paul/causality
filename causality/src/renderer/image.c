/* image.c — GPU-uploaded RGBA image textures for ca_image() widget */
#include "image.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Vulkan helpers (same as font.c) ---- */

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
    return 0;
}

static VkCommandBuffer begin_once(Ca_Instance *inst)
{
    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = inst->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(inst->vk_device, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

static void end_once(Ca_Instance *inst, VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    vkQueueSubmit(inst->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(inst->gfx_queue);
    vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &cmd);
}

/* ---- Descriptor pool for image textures ---- */

bool ca_image_pool_init(Ca_Instance *inst)
{
    VkDescriptorPoolSize pool_sz = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = CA_MAX_IMAGES,
    };
    VkDescriptorPoolCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = CA_MAX_IMAGES,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_sz,
    };
    if (vkCreateDescriptorPool(inst->vk_device, &ci, NULL,
                               &inst->image_desc_pool) != VK_SUCCESS) {
        fprintf(stderr, "[image] descriptor pool creation failed\n");
        return false;
    }
    return true;
}

void ca_image_pool_shutdown(Ca_Instance *inst)
{
    /* Destroy all live images */
    for (int i = 0; i < CA_MAX_IMAGES; i++) {
        if (inst->images[i].in_use)
            ca_image_destroy_impl(inst, &inst->images[i]);
    }
    if (inst->image_desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(inst->vk_device, inst->image_desc_pool, NULL);
        inst->image_desc_pool = VK_NULL_HANDLE;
    }
}

/* ---- Image creation ---- */

Ca_Image *ca_image_create_impl(Ca_Instance *inst,
                               const uint8_t *pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0) return NULL;

    /* Find free slot */
    Ca_Image *img = NULL;
    for (int i = 0; i < CA_MAX_IMAGES; i++) {
        if (!inst->images[i].in_use) { img = &inst->images[i]; break; }
    }
    if (!img) {
        fprintf(stderr, "[image] image pool exhausted (max %d)\n", CA_MAX_IMAGES);
        return NULL;
    }

    memset(img, 0, sizeof(*img));
    img->width  = w;
    img->height = h;

    VkDeviceSize data_sz = (VkDeviceSize)(w * h * 4);

    /* VkImage (RGBA8, device-local) */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(inst->vk_device, &img_ci, NULL, &img->vk_image) != VK_SUCCESS) {
        fprintf(stderr, "[image] vkCreateImage failed\n");
        return NULL;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(inst->vk_device, img->vk_image, &req);
    VkMemoryAllocateInfo mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = find_memory_type(inst->vk_gpu, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    vkAllocateMemory(inst->vk_device, &mem_ai, NULL, &img->memory);
    vkBindImageMemory(inst->vk_device, img->vk_image, img->memory, 0);

    /* Staging buffer */
    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = data_sz,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer       staging;
    VkDeviceMemory staging_mem;
    vkCreateBuffer(inst->vk_device, &buf_ci, NULL, &staging);

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(inst->vk_device, staging, &buf_req);
    VkMemoryAllocateInfo buf_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = buf_req.size,
        .memoryTypeIndex = find_memory_type(inst->vk_gpu, buf_req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    vkAllocateMemory(inst->vk_device, &buf_mem_ai, NULL, &staging_mem);
    vkBindBufferMemory(inst->vk_device, staging, staging_mem, 0);

    void *mapped;
    vkMapMemory(inst->vk_device, staging_mem, 0, data_sz, 0, &mapped);
    memcpy(mapped, pixels, (size_t)data_sz);
    vkUnmapMemory(inst->vk_device, staging_mem);

    /* Upload via one-shot command buffer */
    VkCommandBuffer cmd = begin_once(inst);

    VkImageMemoryBarrier bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = img->vk_image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent       = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, img->vk_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &bar);

    end_once(inst, cmd);

    vkDestroyBuffer(inst->vk_device, staging, NULL);
    vkFreeMemory(inst->vk_device, staging_mem, NULL);

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = img->vk_image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCreateImageView(inst->vk_device, &view_ci, NULL, &img->view);

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
    vkCreateSampler(inst->vk_device, &samp_ci, NULL, &img->sampler);

    /* Descriptor set — reuses the text pipeline's descriptor set layout */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = inst->image_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &inst->text_pipeline.desc_layout,
    };
    if (vkAllocateDescriptorSets(inst->vk_device, &ds_ai, &img->desc_set) != VK_SUCCESS) {
        fprintf(stderr, "[image] descriptor set alloc failed\n");
        ca_image_destroy_impl(inst, img);
        return NULL;
    }

    /* Write the RGBA texture into the descriptor set */
    VkDescriptorImageInfo img_info = {
        .sampler     = img->sampler,
        .imageView   = img->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wr = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = img->desc_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &img_info,
    };
    vkUpdateDescriptorSets(inst->vk_device, 1, &wr, 0, NULL);

    img->in_use = true;
    printf("[image] created %dx%d RGBA texture\n", w, h);
    return img;
}

void ca_image_destroy_impl(Ca_Instance *inst, Ca_Image *img)
{
    if (!img || !img->in_use) return;
    vkDeviceWaitIdle(inst->vk_device);

    if (img->desc_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(inst->vk_device, inst->image_desc_pool,
                             1, &img->desc_set);
        img->desc_set = VK_NULL_HANDLE;
    }
    if (img->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(inst->vk_device, img->sampler, NULL);
        img->sampler = VK_NULL_HANDLE;
    }
    if (img->view != VK_NULL_HANDLE) {
        vkDestroyImageView(inst->vk_device, img->view, NULL);
        img->view = VK_NULL_HANDLE;
    }
    if (img->vk_image != VK_NULL_HANDLE) {
        vkDestroyImage(inst->vk_device, img->vk_image, NULL);
        img->vk_image = VK_NULL_HANDLE;
    }
    if (img->memory != VK_NULL_HANDLE) {
        vkFreeMemory(inst->vk_device, img->memory, NULL);
        img->memory = VK_NULL_HANDLE;
    }
    img->in_use = false;
}
