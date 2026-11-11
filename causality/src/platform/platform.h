// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

#include "causality.h"
#include <vulkan/vulkan.h>

/** Acquire and release the selected platform backend for one instance. */
bool ca_platform_init(void);
void ca_platform_shutdown(void);

/** Monotonic seconds on the backend's clock. */
double ca_platform_time(void);

/** Wake an idle event pump. Safe to call from a worker thread. */
void ca_platform_wake(void);

/** Deliver normalized input for this frame, including any events queued by the host.
 *  Zero polls, negative waits indefinitely, and positive values are seconds. */
void ca_platform_process_events(Ca_Instance *instance, double timeout);

/** Create the native handle for window using desc, and release it at teardown. */
bool ca_platform_window_create(Ca_Window *window, const Ca_WindowDesc *desc, void *host_surface);
void ca_platform_window_destroy(Ca_Window *window);

/** Request focus without restoring window from minimization. */
void ca_platform_window_focus(Ca_Window *window);

/** Query window dimensions in logical units or drawable pixels. Outputs are optional. */
void ca_platform_window_size(const Ca_Window *window, int *width, int *height);
void ca_platform_framebuffer_size(const Ca_Window *window, int *width, int *height);

/** Query the window's display scale independently of Causality UI zoom. */
void ca_platform_content_scale(const Ca_Window *window, float *x, float *y);

/** Set the native window's title, or request that the OS minimize it. */
void ca_platform_window_set_title(Ca_Window *window, const char *title);
void ca_platform_window_minimize(Ca_Window *window);

/** Configure a transient drag preview to float above windows and ignore pointer input. */
void ca_platform_window_set_drag_preview(Ca_Window *window);

/** Get the backend's borrowed list of Vulkan instance extensions and its length. */
const char **ca_platform_vk_extensions(uint32_t *count);

/** Check whether queue_family can present using this backend. */
bool ca_platform_vk_presentation_supported(VkInstance instance, VkPhysicalDevice device,
                                            uint32_t queue_family, VkQueueFlags flags);

/** Create a Vulkan surface for window. The renderer owns the returned surface. */
VkResult ca_platform_vk_surface_create(VkInstance instance, const Ca_Window *window,
                                        VkSurfaceKHR *surface);

/** Run native edge and corner resizing before widget input. */
void ca_platform_window_resize_pass(Ca_Window *window);

/** Handle native title-bar movement and report whether it owns input. */
bool ca_platform_titlebar_drag_pass(Ca_Window *window);

/** Query the live primary pointer button state, including during drag capture. */
bool ca_platform_primary_button_held(Ca_Window *window);

/** Set the horizontal resize cursor or restore the platform default. */
void ca_platform_set_horizontal_drag_cursor(Ca_Window *window);
void ca_platform_set_default_cursor(Ca_Window *window);
