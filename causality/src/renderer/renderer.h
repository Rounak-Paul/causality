/* renderer.h — internal Vulkan renderer */
#pragma once

#include "ca_internal.h"

/* ---- Renderer lifecycle ---- */

bool ca_renderer_init(Ca_Instance *inst, const Ca_InstanceDesc *desc);
void ca_renderer_shutdown(Ca_Instance *inst);

/* ---- Per-window surface / swapchain ---- */

/* Create VkSurfaceKHR + swapchain for a window.  Called from ca_window_create. */
bool ca_renderer_window_init(Ca_Instance *inst, Ca_Window *win);

/* Destroy swapchain + surface for a window.  Called from ca_window_destroy. */
void ca_renderer_window_shutdown(Ca_Instance *inst, Ca_Window *win);

/* Rebuild swapchain after a resize. */
bool ca_renderer_window_resize(Ca_Instance *inst, Ca_Window *win, int w, int h);

/* Render one frame for every live window (acquire → clear → present). */
void ca_renderer_frame(Ca_Instance *inst);
