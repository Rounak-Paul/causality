// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* blur.h — CSS backdrop-filter: blur() */
#pragma once

#include "ca_internal.h"
#include <stdbool.h>

/**
 * Creates the shared backdrop pipelines: the separable Gaussian pass and the
 * composite pass that draws a blurred region under an element.
 *
 * @param inst         Instance owning the pipelines (text pipeline must exist).
 * @param color_format Swapchain color format the composite pass renders into.
 * @return true on success.
 */
bool ca_blur_pipeline_create(Ca_Instance *inst, VkFormat color_format);

/**
 * Destroys the pipelines created by ca_blur_pipeline_create.
 *
 * @param inst Instance owning the pipelines.
 */
void ca_blur_pipeline_destroy(Ca_Instance *inst);

/**
 * Creates the per-window scratch images for a swapchain of the given extent.
 *
 * @param inst   Instance owning the device.
 * @param win    Window receiving the images.
 * @param width  Swapchain width in physical pixels.
 * @param height Swapchain height in physical pixels.
 * @return true on success, or when the pipelines do not exist yet.
 */
bool ca_blur_window_create(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height);

/**
 * Destroys the per-window scratch images and sampler.
 *
 * @param inst Instance owning the device.
 * @param win  Window whose images are released.
 */
void ca_blur_window_destroy(Ca_Instance *inst, Ca_Window *win);

/**
 * Recreates the per-window scratch images when the swapchain extent changes.
 *
 * @param inst   Instance owning the device.
 * @param win    Window whose images are resized.
 * @param width  New swapchain width in physical pixels.
 * @param height New swapchain height in physical pixels.
 * @return true on success.
 */
bool ca_blur_window_resize(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height);

/**
 * Captures one element's backdrop from the swapchain and blurs it.
 *
 * Must be recorded outside dynamic rendering, with the swapchain image in
 * COLOR_ATTACHMENT_OPTIMAL (restored on return). Only `region` plus the
 * kernel margin is copied and filtered, so the result reflects exactly what
 * has been painted so far this frame — the CSS backdrop of an element
 * painted at this point in paint order.
 *
 * @param inst            Instance owning the pipelines.
 * @param win             Window owning the scratch images.
 * @param cmd             Command buffer being recorded.
 * @param swapchain_image Swapchain image holding the frame painted so far.
 * @param sc_extent       Swapchain extent in physical pixels.
 * @param region          Physical-pixel rect the composite will cover.
 * @param sigma_px        Gaussian standard deviation in physical pixels
 *                        (CSS blur(r) uses sigma = r).
 * @param uv_scale        Out: multiply gl_FragCoord.xy by this to get the
 *                        blur_image UV of a swapchain pixel.
 * @return true when blur_image now holds the blurred region.
 */
bool ca_blur_capture_region(Ca_Instance *inst, Ca_Window *win,
                            VkCommandBuffer cmd, VkImage swapchain_image,
                            VkExtent2D sc_extent, VkRect2D region,
                            float sigma_px, float uv_scale[2]);
