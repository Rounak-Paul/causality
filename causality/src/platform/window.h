// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* causality/src/window.h — internal window subsystem */
#pragma once

#include "ca_internal.h"

/* Initialise the GLFW window system; must be called before any window is created. */
bool        ca_window_system_init(void);

/* Destroy all open windows for inst and decrement the GLFW refcount. */
void        ca_window_system_shutdown(Ca_Instance *inst);

/*
 * Process one frame of GLFW events; returns true while any window remains open.
 * Clears per-frame input state, polls/waits for events, dispatches handlers,
 * and destroys windows that requested close.
 */
bool        ca_window_system_tick(Ca_Instance *inst);

/* Create a user window in the first available pool slot. */
CA_API Ca_Window  *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc);

/*
 * Create a window in a reserved (system-use) pool slot.
 * reserved_index must be in [0, CA_RESERVED_POPUP_WINDOWS).
 */
Ca_Window         *ca_window_create_reserved(Ca_Instance *inst, const Ca_WindowDesc *desc,
											 int reserved_index);

/* Destroy window and release all Vulkan/GLFW/UI resources. */
CA_API void        ca_window_destroy(Ca_Window *window);

/* Return the underlying GLFWwindow* handle, or NULL if the window is not in use. */
GLFWwindow *ca_window_glfw(const Ca_Window *window);

/*
 * Process per-frame edge/corner resize hit-testing for undecorated windows.
 * Must be called once per tick before the UI update pass.
 */
void        ca_window_resize_pass(Ca_Window *window);

