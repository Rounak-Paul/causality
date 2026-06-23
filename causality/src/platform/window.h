// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* causality/src/window.h — internal window subsystem */
#pragma once

#include "ca_internal.h"

/*
 * Initialize the GLFW window system.
 *
 * Must be called before any window is created. Idempotent on subsequent calls.
 *
 * Returns true on success, false if GLFW initialization fails.
 */
bool        ca_window_system_init(void);

/*
 * Destroy all open windows for an instance and decrement the GLFW reference count.
 *
 * inst  Instance whose windows are to be destroyed.
 */
void        ca_window_system_shutdown(Ca_Instance *inst);

/*
 * Process one frame of GLFW events; returns true while any window remains open.
 * Clears per-frame input state, polls/waits for events, dispatches handlers,
 * and destroys windows that requested close.
 */
bool        ca_window_system_tick(Ca_Instance *inst);

/*
 * Create a user-facing window in the first available pool slot.
 *
 * inst  Instance to create the window on.
 * desc  Window descriptor with title, dimensions, and optional properties.
 * Returns newly created window, or NULL if no slots are available or creation fails.
 */
CA_API Ca_Window  *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc);

/*
 * Create a window in a reserved (system-use) pool slot (e.g., for popups).
 *
 * inst              Instance to create the window on.
 * desc              Window descriptor with title, dimensions, and properties.
 * reserved_index    Index into reserved pool; must be in [0, CA_RESERVED_POPUP_WINDOWS).
 * Returns           Newly created window, or NULL if index is invalid or creation fails.
 */
Ca_Window         *ca_window_create_reserved(Ca_Instance *inst, const Ca_WindowDesc *desc,
											 int reserved_index);

/*
 * Destroy a window and release all associated Vulkan, GLFW, and UI resources.
 *
 * window  Window to destroy; no-op if NULL or not in use.
 */
CA_API void        ca_window_destroy(Ca_Window *window);

/*
 * Get the underlying GLFW window handle for a Causality window.
 *
 * window  Causality window to query.
 * Returns the GLFWwindow pointer, or NULL if the window is not in use.
 */
GLFWwindow *ca_window_glfw(const Ca_Window *window);

/** Shows the horizontal-drag cursor for an interactive numeric control. */
void        ca_window_set_horizontal_drag_cursor(Ca_Window *window);

/** Restores the platform-default cursor for a window. */
void        ca_window_set_default_cursor(Ca_Window *window);
