/* swapchain.h — per-window swapchain + per-frame sync */
#pragma once

#include "ca_internal.h"

bool ca_swapchain_create(Ca_Instance *inst, Ca_Window *win,
                         uint32_t width, uint32_t height);
void ca_swapchain_destroy(Ca_Instance *inst, Ca_Window *win);
void ca_swapchain_frame(Ca_Instance *inst, Ca_Window *win);
