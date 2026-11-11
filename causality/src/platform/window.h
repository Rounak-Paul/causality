// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

#include "ca_internal.h"

/** Destroy all logical windows owned by inst before shutting down the backend. */
void ca_window_system_shutdown(Ca_Instance *inst);

/** Process input and close requests for inst. Wait for events only when wait is true. */
bool ca_window_system_tick(Ca_Instance *inst, bool wait);

/** Create a logical window backed by a native rendering surface supplied by the host. */
Ca_Window *ca_window_create_hosted(Ca_Instance *inst, const Ca_WindowDesc *desc, void *host_surface);

/** Create a system-use window for a popup at reserved_index. */
Ca_Window *ca_window_create_reserved(Ca_Instance *inst, const Ca_WindowDesc *desc,
                                     int reserved_index);

/* Backends deliver these events on the UI thread during their event pump. */

/** Receive a platform key event in Causality key, action and modifier values. */
void ca_window_input_key(Ca_Window *window, int key, int scancode, int action, int mods);

/** Receive a committed Unicode codepoint for window. */
void ca_window_input_char(Ca_Window *window, uint32_t codepoint);

/** Receive pointer movement or a button event at logical window coordinates x and y. */
void ca_window_input_move(Ca_Window *window, double x, double y);
void ca_window_input_button(Ca_Window *window, int button, int action, int mods,
                            double x, double y);

/** Receive scroll deltas for window in wheel units, positive up or right. */
void ca_window_input_scroll(Ca_Window *window, double dx, double dy);

/** Clear pointer hover on exit and reset latched input when the window loses focus. */
void ca_window_input_leave(Ca_Window *window);
void ca_window_input_unfocus(Ca_Window *window);

/** Report logical resize or a new drawable extent in pixels. */
void ca_window_handle_resize(Ca_Window *window, int width, int height);
void ca_window_handle_framebuffer_resize(Ca_Window *window, int width, int height);
