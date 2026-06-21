// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* blur.h — backdrop-filter blur pipeline */
#pragma once

#include "ca_internal.h"
#include <stdbool.h>

/* Create/destroy the shared blur pipelines (H-pass, V-pass, composite).
   Called once on first window init alongside the other pipelines.         */
bool ca_blur_pipeline_create(Ca_Instance *inst, VkFormat color_format);
void ca_blur_pipeline_destroy(Ca_Instance *inst);

/* Create/resize/destroy per-window blur images.
   Call ca_blur_window_create after swapchain creation.
   Call ca_blur_window_destroy before swapchain destruction.               */
bool ca_blur_window_create(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height, VkFormat format);
void ca_blur_window_destroy(Ca_Instance *inst, Ca_Window *win);
bool ca_blur_window_resize(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height, VkFormat format);

/* Called each frame in swapchain.c before vkCmdBeginRendering.
   Checks if any backdrop-blur draw commands are present; if so, copies
   the current swapchain image into blur_image and applies the two-pass
   Gaussian blur.  blur_image_valid is set to true on success.            */
void ca_blur_capture_and_blur(Ca_Instance *inst, Ca_Window *win,
                              VkCommandBuffer cmd,
                              VkImage swapchain_image,
                              uint32_t sc_width, uint32_t sc_height,
                              float blur_radius);
