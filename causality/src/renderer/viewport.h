// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* viewport.h — offscreen render target for external renderers */
#pragma once

#include "ca_internal.h"

/* Create GPU resources for a viewport (colour image, view, sampler, descriptor). */
bool ca_viewport_gpu_create(Ca_Instance *inst, Ca_Viewport *vp,
                            uint32_t width, uint32_t height, VkFormat format);

/* Destroy GPU resources for a viewport. */
void ca_viewport_gpu_destroy(Ca_Instance *inst, Ca_Viewport *vp);

/* Resize the viewport's offscreen image.  Destroys and recreates. */
bool ca_viewport_gpu_resize(Ca_Instance *inst, Ca_Viewport *vp,
                            uint32_t width, uint32_t height);

/* Invoke on_render callbacks for all active viewports in a window.
   Called from swapchain_frame before compositing.

   Submits each redrawn viewport's GPU work asynchronously (no CPU wait for
   completion) and writes that viewport's completion semaphore into
   out_semaphores, returning the count in *out_count — the caller must wait
   on all of them at the GPU level (e.g. as additional submit wait
   semaphores) before any command buffer that samples a viewport's texture
   (the compositing pass) executes, since nothing else guarantees the
   render has finished by then. out_semaphores must have room for at least
   CA_MAX_VIEWPORTS_PER_WINDOW entries. */
void ca_viewport_render_all(Ca_Instance *inst, Ca_Window *win,
                            VkSemaphore *out_semaphores, uint32_t *out_count);
