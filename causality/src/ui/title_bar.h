/* title_bar.h — Internal header for the custom window title bar.
 *
 * Called by ui.c and window.c.  Not part of the public API.
 */
#pragma once

#include "../core/ca_internal.h"
#include "../../include/ca_components.h"

/* Called once from ca_ui_window_init.
   Creates win->root, win->title_bar_node, and win->content_root. */
void ca_title_bar_init(Ca_Window *win);

/* Called from ca_ui_update when win->titlebar_needs_rebuild is true.
   Must be called within a ca_widget_ctx_enter / ca_widget_ctx_leave pair. */
void ca_title_bar_rebuild(Ca_Window *win);
