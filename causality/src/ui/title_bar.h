// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* title_bar.h — Internal header for window structure initialisation.
 *
 * Called by ui.c.  Not part of the public API.
 */
#pragma once

#include "../core/ca_internal.h"
#include "../../include/ca_components.h"

/* Called once from ca_ui_window_init.  Creates win->root, win->content_root,
   and win->status_bar_node.  win->title_bar_node is left NULL. */
void ca_title_bar_init(Ca_Window *win);

/* Called from ca_ui_update when win->titlebar_needs_rebuild is true.
   No-op for decorated windows; kept so ui.c needn't special-case. */
void ca_title_bar_rebuild(Ca_Window *win);

/* Called from ca_ui_update when win->statusbar_needs_rebuild is true.
   Must be called within a ca_widget_ctx_enter / ca_widget_ctx_leave pair. */
void ca_status_bar_rebuild(Ca_Window *win);
