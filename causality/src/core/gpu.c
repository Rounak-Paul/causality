// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* gpu.c — public GPU resource accessors for external renderers */
#include "ca_internal.h"

/*
 * Return the Vulkan instance handle owned by this Ca_Instance.
 *
 * instance  Ca_Instance to query; returns VK_NULL_HANDLE if NULL.
 * Returns   The VkInstance, or VK_NULL_HANDLE.
 */
VkInstance ca_gpu_instance(Ca_Instance *instance)
{
    return instance ? instance->vk_instance : VK_NULL_HANDLE;
}

/*
 * Return the selected Vulkan physical device (GPU) handle.
 *
 * instance  Ca_Instance to query; returns VK_NULL_HANDLE if NULL.
 * Returns   The VkPhysicalDevice, or VK_NULL_HANDLE.
 */
VkPhysicalDevice ca_gpu_physical_device(Ca_Instance *instance)
{
    return instance ? instance->vk_gpu : VK_NULL_HANDLE;
}

/*
 * Return the Vulkan logical device handle.
 *
 * instance  Ca_Instance to query; returns VK_NULL_HANDLE if NULL.
 * Returns   The VkDevice, or VK_NULL_HANDLE.
 */
VkDevice ca_gpu_device(Ca_Instance *instance)
{
    return instance ? instance->vk_device : VK_NULL_HANDLE;
}

/*
 * Return whether the Vulkan 1.2 drawIndirectCount feature was enabled at
 * device creation.  False on MoltenVK and other drivers without support;
 * callers must fall back to fixed-count indirect draws.
 */
bool ca_gpu_draw_indirect_count_supported(Ca_Instance *instance)
{
    return instance ? instance->draw_indirect_count : false;
}

/*
 * Return whether the Vulkan 1.2 descriptor-indexing feature set (bindless
 * textures: update-after-bind + partially-bound + non-uniform dynamic
 * indexing) was enabled at device creation. False on drivers missing any
 * one of the required bits; callers must fall back to fixed per-texture
 * descriptor bindings.
 */
bool ca_gpu_bindless_supported(Ca_Instance *instance)
{
    return instance ? instance->descriptor_indexing_supported : false;
}

/*
 * Return the Vulkan graphics queue and optionally its queue family index.
 *
 * instance      Ca_Instance to query; returns VK_NULL_HANDLE if NULL.
 * family_index  If non-NULL, receives the graphics queue family index.
 * Returns       The graphics VkQueue, or VK_NULL_HANDLE.
 */
VkQueue ca_gpu_graphics_queue(Ca_Instance *instance, uint32_t *family_index)
{
    if (!instance) return VK_NULL_HANDLE;
    if (family_index) *family_index = instance->gfx_family;
    return instance->gfx_queue;
}

/*
 * Return the Vulkan present queue and optionally its queue family index.
 *
 * instance      Ca_Instance to query; returns VK_NULL_HANDLE if NULL.
 * family_index  If non-NULL, receives the present queue family index.
 * Returns       The present VkQueue, or VK_NULL_HANDLE.
 */
VkQueue ca_gpu_present_queue(Ca_Instance *instance, uint32_t *family_index)
{
    if (!instance) return VK_NULL_HANDLE;
    if (family_index) *family_index = instance->present_family;
    return instance->present_queue;
}

/*
 * Return the shared Vulkan command pool for the instance.
 *
 * instance  Ca_Instance to query; returns VK_NULL_HANDLE if NULL.
 * Returns   The VkCommandPool, or VK_NULL_HANDLE.
 */
VkCommandPool ca_gpu_command_pool(Ca_Instance *instance)
{
    return instance ? instance->cmd_pool : VK_NULL_HANDLE;
}

/*
 * Find a Vulkan memory type index satisfying the given property flags.
 *
 * Queries the physical device's memory properties and returns the index of
 * the first memory type whose bit is set in type_bits and whose flags are
 * a superset of properties.
 *
 * instance    Ca_Instance providing the physical device; returns UINT32_MAX if NULL.
 * type_bits   Bitmask from VkMemoryRequirements.memoryTypeBits.
 * properties  Required VkMemoryPropertyFlags.
 * Returns     Matching memory type index, or UINT32_MAX if none found.
 */
uint32_t ca_gpu_find_memory_type(Ca_Instance *instance,
                                  uint32_t type_bits,
                                  VkMemoryPropertyFlags properties)
{
    if (!instance) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(instance->vk_gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

/*
 * Allocate and begin a one-time-submit command buffer for transfer operations.
 *
 * Allocates a primary command buffer from the shared pool and calls
 * vkBeginCommandBuffer with ONE_TIME_SUBMIT_BIT.  Pair with
 * ca_gpu_end_transfer() to submit and free it.
 *
 * instance  Ca_Instance providing the device and command pool.
 * Returns   Started VkCommandBuffer, or VK_NULL_HANDLE if instance is NULL.
 */
VkCommandBuffer ca_gpu_begin_transfer(Ca_Instance *instance)
{
    if (!instance) return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = instance->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(instance->vk_device, &ai, &cmd) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        vkFreeCommandBuffers(instance->vk_device, instance->cmd_pool, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

/*
 * End, submit, and free a one-time-submit transfer command buffer.
 *
 * Ends recording, submits to the graphics queue, waits for the queue to
 * become idle, then frees the command buffer back to the pool.
 *
 * instance  Ca_Instance providing the device and queue.
 * cmd       Command buffer returned by ca_gpu_begin_transfer().
 */
void ca_gpu_end_transfer(Ca_Instance *instance, VkCommandBuffer cmd)
{
    if (!instance || !cmd) return;
    if (vkEndCommandBuffer(cmd) == VK_SUCCESS) {
        VkSubmitInfo si = {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cmd,
        };
        if (vkQueueSubmit(instance->gfx_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS)
            vkQueueWaitIdle(instance->gfx_queue);
    }
    vkFreeCommandBuffers(instance->vk_device, instance->cmd_pool, 1, &cmd);
}

/*
 * Registers the external renderer's device-resource predestroy hook — see
 * ca_gpu_set_predestroy_callback's doc comment in ca_gpu.h. Only one
 * callback is held; a later call replaces the previous one.
 */
void ca_gpu_set_predestroy_callback(Ca_Instance *instance,
                                     void (*fn)(void *user_data),
                                     void *user_data)
{
    if (!instance) return;
    instance->gpu_predestroy_fn   = fn;
    instance->gpu_predestroy_data = user_data;
}
