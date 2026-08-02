// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

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
    return UINT32_MAX;
}

static bool begin_once(Ca_Instance *inst, VkCommandBuffer *out_cmd)
{
    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = inst->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    *out_cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(inst->vk_device, &ai, out_cmd) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(*out_cmd, &bi) != VK_SUCCESS) {
        vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, out_cmd);
        *out_cmd = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool end_once(Ca_Instance *inst, VkCommandBuffer cmd)
{
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &cmd);
        return false;
    }
    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    VkResult result = vkQueueSubmit(inst->gfx_queue, 1, &si, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) result = vkQueueWaitIdle(inst->gfx_queue);
    vkFreeCommandBuffers(inst->vk_device, inst->cmd_pool, 1, &cmd);
    return result == VK_SUCCESS;
}

enum { CA_IMAGE_DESCRIPTOR_POOL_CHUNK = 64 };

static bool image_descriptor_pool_create(Ca_Instance *inst,
                                         VkDescriptorPool *out_pool)
{
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = CA_IMAGE_DESCRIPTOR_POOL_CHUNK,
    };
    VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = CA_IMAGE_DESCRIPTOR_POOL_CHUNK,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    return vkCreateDescriptorPool(inst->vk_device, &info, NULL, out_pool) ==
           VK_SUCCESS;
}

bool ca_image_descriptor_allocate(Ca_Instance *inst,
                                  VkDescriptorSetLayout layout,
                                  VkDescriptorSet *out_set,
                                  VkDescriptorPool *out_pool)
{
    if (!inst || layout == VK_NULL_HANDLE || !out_set || !out_pool) return false;
    *out_set = VK_NULL_HANDLE;
    *out_pool = VK_NULL_HANDLE;

    VkDescriptorPool *pools = (VkDescriptorPool *)inst->image_desc_pools.data;
    for (size_t i = 0; i < inst->image_desc_pools.count; ++i) {
        VkDescriptorSetAllocateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pools[i],
            .descriptorSetCount = 1,
            .pSetLayouts = &layout,
        };
        VkResult result = vkAllocateDescriptorSets(inst->vk_device, &info,
                                                    out_set);
        if (result == VK_SUCCESS) {
            *out_pool = pools[i];
            return true;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY &&
            result != VK_ERROR_FRAGMENTED_POOL)
            return false;
    }

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!image_descriptor_pool_create(inst, &pool)) return false;
    if (!ca_dyn_array_push(&inst->image_desc_pools, &pool)) {
        vkDestroyDescriptorPool(inst->vk_device, pool, NULL);
        return false;
    }
    VkDescriptorSetAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    if (vkAllocateDescriptorSets(inst->vk_device, &info, out_set) != VK_SUCCESS)
        return false;
    *out_pool = pool;
    return true;
}

void ca_image_descriptor_free(Ca_Instance *inst,
                              VkDescriptorPool pool,
                              VkDescriptorSet set)
{
    if (!inst || pool == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return;
    vkFreeDescriptorSets(inst->vk_device, pool, 1, &set);
}

/* ---- Descriptor pools and image handles ---- */

bool ca_image_pool_init(Ca_Instance *inst)
{
    if (!inst) return false;
    ca_dyn_array_init(&inst->image_desc_pools, sizeof(VkDescriptorPool));
    if (!ca_pool_init(&inst->images, sizeof(Ca_Image),
                      ca_pool_recommended_chunk_capacity(sizeof(Ca_Image)))) {
        ca_dyn_array_destroy(&inst->image_desc_pools);
        return false;
    }
    return true;
}

void ca_image_pool_shutdown(Ca_Instance *inst)
{
    /* Destroy all live images */
    for (size_t i = 0; i < ca_pool_slot_count(&inst->images); ++i) {
        Ca_Image *image = CA_POOL_AT(inst->images, Ca_Image, i);
        if (image->in_use) ca_image_destroy_impl(inst, image);
    }
    ca_pool_destroy(&inst->images, NULL, NULL);
    VkDescriptorPool *pools = (VkDescriptorPool *)inst->image_desc_pools.data;
    for (size_t i = 0; i < inst->image_desc_pools.count; ++i) {
        if (pools[i] != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(inst->vk_device, pools[i], NULL);
    }
    ca_dyn_array_destroy(&inst->image_desc_pools);
}

/* ---- Image creation ---- */

Ca_Image *ca_image_create_impl(Ca_Instance *inst,
                               const uint8_t *pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0) return NULL;

    Ca_Image *img = (Ca_Image *)ca_pool_acquire(&inst->images);
    if (!img) return NULL;
    img->in_use = true;
    img->width  = w;
    img->height = h;

    if ((uint64_t)(uint32_t)w > UINT64_MAX / (uint64_t)(uint32_t)h / 4u) {
        ca_image_destroy_impl(inst, img);
        return NULL;
    }
    VkDeviceSize data_sz = (VkDeviceSize)(uint32_t)w * (uint32_t)h * 4u;
    if (data_sz > SIZE_MAX) {
        ca_image_destroy_impl(inst, img);
        return NULL;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;

    /* VkImage (RGBA8, device-local) */
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_SRGB,
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
        goto fail;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(inst->vk_device, img->vk_image, &req);
    VkMemoryAllocateInfo mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = find_memory_type(inst->vk_gpu, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    if (mem_ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(inst->vk_device, &mem_ai, NULL, &img->memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(inst->vk_device, img->vk_image, img->memory, 0) !=
            VK_SUCCESS)
        goto fail;

    /* Staging buffer */
    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = data_sz,
        .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(inst->vk_device, &buf_ci, NULL, &staging) != VK_SUCCESS)
        goto fail;

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(inst->vk_device, staging, &buf_req);
    VkMemoryAllocateInfo buf_mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = buf_req.size,
        .memoryTypeIndex = find_memory_type(inst->vk_gpu, buf_req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (buf_mem_ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(inst->vk_device, &buf_mem_ai, NULL, &staging_mem) !=
            VK_SUCCESS ||
        vkBindBufferMemory(inst->vk_device, staging, staging_mem, 0) !=
            VK_SUCCESS)
        goto fail;

    void *mapped = NULL;
    if (vkMapMemory(inst->vk_device, staging_mem, 0, data_sz, 0, &mapped) !=
            VK_SUCCESS || !mapped)
        goto fail;
    memcpy(mapped, pixels, (size_t)data_sz);
    vkUnmapMemory(inst->vk_device, staging_mem);

    /* Upload via one-shot command buffer */
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!begin_once(inst, &cmd)) goto fail;

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

    if (!end_once(inst, cmd)) goto fail;

    vkDestroyBuffer(inst->vk_device, staging, NULL);
    vkFreeMemory(inst->vk_device, staging_mem, NULL);
    staging = VK_NULL_HANDLE;
    staging_mem = VK_NULL_HANDLE;

    /* Image view */
    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = img->vk_image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(inst->vk_device, &view_ci, NULL, &img->view) !=
        VK_SUCCESS)
        goto fail;

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
    if (vkCreateSampler(inst->vk_device, &samp_ci, NULL, &img->sampler) !=
        VK_SUCCESS)
        goto fail;

    /* Descriptor set — reuses the text pipeline's descriptor set layout */
    if (!ca_image_descriptor_allocate(inst, inst->text_pipeline.desc_layout,
                                      &img->desc_set, &img->desc_pool)) {
        fprintf(stderr, "[image] descriptor set alloc failed\n");
        goto fail;
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

    printf("[image] created %dx%d RGBA texture\n", w, h);
    return img;

fail:
    if (staging != VK_NULL_HANDLE)
        vkDestroyBuffer(inst->vk_device, staging, NULL);
    if (staging_mem != VK_NULL_HANDLE)
        vkFreeMemory(inst->vk_device, staging_mem, NULL);
    ca_image_destroy_impl(inst, img);
    return NULL;
}

void ca_image_destroy_impl(Ca_Instance *inst, Ca_Image *img)
{
    if (!img || !img->in_use) return;
    vkDeviceWaitIdle(inst->vk_device);

    if (img->desc_set != VK_NULL_HANDLE) {
        ca_image_descriptor_free(inst, img->desc_pool, img->desc_set);
        img->desc_set = VK_NULL_HANDLE;
        img->desc_pool = VK_NULL_HANDLE;
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
    ca_pool_release(&inst->images, img);
}
