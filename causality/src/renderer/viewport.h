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
   Called from swapchain_frame before compositing. */
void ca_viewport_render_all(Ca_Instance *inst, Ca_Window *win);
