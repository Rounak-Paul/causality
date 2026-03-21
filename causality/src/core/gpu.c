/* gpu.c — public GPU resource accessors for external renderers */
#include "ca_internal.h"

VkInstance ca_gpu_instance(Ca_Instance *instance)
{
    return instance ? instance->vk_instance : VK_NULL_HANDLE;
}

VkPhysicalDevice ca_gpu_physical_device(Ca_Instance *instance)
{
    return instance ? instance->vk_gpu : VK_NULL_HANDLE;
}

VkDevice ca_gpu_device(Ca_Instance *instance)
{
    return instance ? instance->vk_device : VK_NULL_HANDLE;
}

VkQueue ca_gpu_graphics_queue(Ca_Instance *instance, uint32_t *family_index)
{
    if (!instance) return VK_NULL_HANDLE;
    if (family_index) *family_index = instance->gfx_family;
    return instance->gfx_queue;
}

VkQueue ca_gpu_present_queue(Ca_Instance *instance, uint32_t *family_index)
{
    if (!instance) return VK_NULL_HANDLE;
    if (family_index) *family_index = instance->present_family;
    return instance->present_queue;
}

VkCommandPool ca_gpu_command_pool(Ca_Instance *instance)
{
    return instance ? instance->cmd_pool : VK_NULL_HANDLE;
}

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

VkCommandBuffer ca_gpu_begin_transfer(Ca_Instance *instance)
{
    if (!instance) return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = instance->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(instance->vk_device, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void ca_gpu_end_transfer(Ca_Instance *instance, VkCommandBuffer cmd)
{
    if (!instance || !cmd) return;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cmd,
    };
    vkQueueSubmit(instance->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(instance->gfx_queue);
    vkFreeCommandBuffers(instance->vk_device, instance->cmd_pool, 1, &cmd);
}
