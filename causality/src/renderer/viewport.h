// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* viewport.h — offscreen render target for external renderers */
#pragma once

#include "ca_internal.h"

/* Create GPU resources for a viewport (colour image, view, sampler, descriptor). */
bool ca_viewport_gpu_create(Ca_Instance *inst, Ca_Viewport *vp,
                            uint32_t width, uint32_t height, VkFormat format);

/* Detach a viewport's GPU resources for deferred release. In-flight
   compositing submits of win may still wait on render_done or sample
   desc_set, so the objects move to win's retired list and are destroyed by
   ca_viewport_collect_retired once the GPU is done with them. Released
   immediately when win has no swapchain; falls back to a synchronous queue
   wait only if the retired list cannot grow.

   inst  Owning instance.
   win   Window whose swapchain composites vp; NULL forces a synchronous wait.
   vp    Viewport to strip; left with no GPU objects and zero size. */
void ca_viewport_gpu_retire(Ca_Instance *inst, Ca_Window *win, Ca_Viewport *vp);

/* Destroy win's retired viewport generations whose GPU work has completed.
   Non-blocking: polls fence status only.

   inst         Owning instance.
   win          Window whose retired list is collected.
   device_idle  true when the caller has already idled the device; releases
                every entry unconditionally. */
void ca_viewport_collect_retired(Ca_Instance *inst, Ca_Window *win, bool device_idle);

/* Resize the viewport's offscreen image: retires the current generation
   (see ca_viewport_gpu_retire) and creates a new one.

   Returns false when the new generation could not be created. */
bool ca_viewport_gpu_resize(Ca_Instance *inst, Ca_Window *win, Ca_Viewport *vp,
                            uint32_t width, uint32_t height);

/* Invoke on_render callbacks for all active viewports in a window.
   Called from swapchain_frame before compositing.

   Submits each redrawn viewport's GPU work asynchronously (no CPU wait for
   completion) and writes that viewport's completion semaphore into
   out_semaphores, returning the count in *out_count — the caller must wait
   on all of them at the GPU level (e.g. as additional submit wait
   semaphores) before any command buffer that samples a viewport's texture
   (the compositing pass) executes, since nothing else guarantees the
   render has finished by then. The output array is cleared and grown as
   needed. */
void ca_viewport_render_all(Ca_Instance *inst, Ca_Window *win,
                            Ca_DynArray *out_semaphores);
