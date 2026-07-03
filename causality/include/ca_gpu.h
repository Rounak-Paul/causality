// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ============================================================
   CA_GPU — native graphics integration surface
   ============================================================

   This header is the ONLY part of the Causality public API that
   exposes Vulkan types.  It exists for external renderers (e.g. a
   game engine) that share Causality's GPU context and record their
   own command buffers.

   Ordinary UI applications must not include this header; the core
   API in <causality.h> is fully backend-agnostic.
   ============================================================ */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "ca_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ca_Instance Ca_Instance;
typedef struct Ca_Window   Ca_Window;
typedef struct Ca_Viewport Ca_Viewport;

/* ============================================================
   BACKGROUND RENDER HOOK
   ============================================================ */

/*
 * Callback invoked each frame to render a background directly into the
 * swapchain image, BEFORE any UI elements are composited.  The swapchain is
 * presented with LOAD_OP_LOAD so this content shows through transparent UI.
 *
 * cmd              Command buffer to record into (already begun).
 * window           Window whose swapchain is being rendered.
 * swapchain_image  VkImage of the current swapchain image (needed for image barriers).
 * swapchain_view   VkImageView of the current swapchain image (COLOR_ATTACHMENT_OPTIMAL).
 * format           VkFormat of the swapchain image.
 * image_usage      Usage flags enabled for the swapchain image.
 * frame_slot       In-flight frame slot whose fence has already completed.
 * width, height    Pixel dimensions of the swapchain image.
 * user_data        Pointer passed to ca_window_set_bg_render.
 */
typedef bool (*Ca_BgRenderFn)(VkCommandBuffer cmd,
                              Ca_Window       *window,
                              VkImage         swapchain_image,
                              VkImageView     swapchain_view,
                              VkFormat        format,
                              VkImageUsageFlags image_usage,
                              uint32_t        frame_slot,
                              uint32_t        width,
                              uint32_t        height,
                              void           *user_data);

/*
 * Register or replace the per-window background render callback.
 * Pass NULL for fn to disable background rendering.
 *
 * window     Target window.
 * fn         Callback to invoke before each frame's UI render pass (NULL = none).
 * user_data  Passed unchanged to fn.
 */
CA_API void ca_window_set_bg_render(Ca_Window *window, Ca_BgRenderFn fn, void *user_data);

/*
 * Set the background callback inherited by every window that has no explicit
 * per-window callback. Existing and subsequently created windows both use it.
 * Pass NULL for fn to clear the fallback.
 */
CA_API void ca_instance_set_bg_render(Ca_Instance *instance,
                                      Ca_BgRenderFn fn,
                                      void *user_data);

/* ============================================================
   GPU — Vulkan resource accessors
   ============================================================

   These expose the Vulkan objects owned by causality so that an
   external renderer (e.g. a game engine) can share the same GPU
   context.  The returned handles are owned by causality — do NOT
   destroy them.
   ============================================================ */

/* Returns the VkInstance created by Causality. */
CA_API VkInstance          ca_gpu_instance(Ca_Instance *instance);

/* Returns the VkPhysicalDevice selected during initialisation. */
CA_API VkPhysicalDevice    ca_gpu_physical_device(Ca_Instance *instance);

/* Returns the VkDevice (logical device). */
CA_API VkDevice            ca_gpu_device(Ca_Instance *instance);

/*
 * Return the graphics VkQueue and optionally its queue family index.
 *
 * instance      Owning Ca_Instance.
 * family_index  Written with the queue family index; may be NULL.
 * Returns       The graphics VkQueue.
 */
CA_API VkQueue             ca_gpu_graphics_queue(Ca_Instance *instance, uint32_t *family_index);

/*
 * Return the presentation VkQueue and optionally its queue family index.
 *
 * instance      Owning Ca_Instance.
 * family_index  Written with the queue family index; may be NULL.
 * Returns       The presentation VkQueue.
 */
CA_API VkQueue             ca_gpu_present_queue(Ca_Instance *instance, uint32_t *family_index);

/* Returns the shared graphics-family command pool (buffers are individually resettable). */
CA_API VkCommandPool       ca_gpu_command_pool(Ca_Instance *instance);

/* Returns whether Vulkan 1.2 drawIndirectCount was enabled at device creation. */
CA_API bool                ca_gpu_draw_indirect_count_supported(Ca_Instance *instance);

/*
 * Find a Vulkan memory type index satisfying the given type bits and property flags.
 *
 * instance    Owning Ca_Instance.
 * type_bits   Bitmask of acceptable memory type indices from VkMemoryRequirements.
 * properties  Required memory property flags.
 * Returns     Matching type index, or UINT32_MAX on failure.
 */
CA_API uint32_t            ca_gpu_find_memory_type(Ca_Instance *instance,
                                                   uint32_t type_bits,
                                                   VkMemoryPropertyFlags properties);

/*
 * Allocate and begin a one-shot command buffer for immediate GPU work.
 *
 * instance  Owning Ca_Instance.
 * Returns   A VkCommandBuffer already in the recording state.
 */
CA_API VkCommandBuffer     ca_gpu_begin_transfer(Ca_Instance *instance);

/*
 * End, submit, wait for, and free a one-shot command buffer.
 *
 * instance  Owning Ca_Instance.
 * cmd       Command buffer returned by ca_gpu_begin_transfer.
 */
CA_API void                ca_gpu_end_transfer(Ca_Instance *instance, VkCommandBuffer cmd);

/*
 * Compile a GLSL source string to a VkShaderModule via shaderc.
 *
 * device      Logical device to create the module on.
 * glsl_source Null-terminated GLSL source code.
 * stage       Shader stage (e.g. VK_SHADER_STAGE_VERTEX_BIT).
 * Returns     The compiled VkShaderModule, or VK_NULL_HANDLE on failure.
 */
CA_API VkShaderModule      ca_shader_compile(VkDevice device,
                                             const char *glsl_source,
                                             VkShaderStageFlagBits stage);

/* ============================================================
   VIEWPORT — Vulkan accessors
   ============================================================

   Widget creation and layout queries for viewports live in
   <causality.h>; the accessors below expose the Vulkan objects an
   external renderer needs inside its on_render callback.
   ============================================================ */

/* Returns the command buffer to record into during the on_render callback. */
CA_API VkCommandBuffer ca_viewport_cmd(Ca_Viewport *viewport);

/* Returns the VkImage backing the viewport (useful for explicit barrier/transition). */
CA_API VkImage ca_viewport_image(const Ca_Viewport *viewport);

/* Returns the VkImageView for the viewport's colour attachment. */
CA_API VkImageView ca_viewport_image_view(const Ca_Viewport *viewport);

/* Returns the VkFormat of the viewport's colour attachment. */
CA_API VkFormat ca_viewport_format(const Ca_Viewport *viewport);

#ifdef __cplusplus
}
#endif
