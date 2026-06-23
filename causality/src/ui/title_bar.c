// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* title_bar.c — Window structure initialisation for decorated windows.
 *
 * Windows are decorated by the platform (GLFW_DECORATED = TRUE), so the OS
 * provides the title bar, window controls, drag-to-move, and resize.
 * Causality owns only the content area below the system title bar.
 *
 * Architecture:
 *   win->root         (vertical flex, fills the content area)
 *   ├── win->content_root  (flex-grow: 1, user content)
 *   └── win->status_bar_node  (optional fixed-height status bar at bottom)
 */

#include "title_bar.h"
#include "node.h"
#include "../core/ca_internal.h"
#include "../../include/causality.h"

#include <GLFW/glfw3.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/* Initialisation — called once from ca_ui_window_init                */
/* ------------------------------------------------------------------ */

/*
 * Initialise the window's UI structure.
 *
 * Creates win->root as a vertical column filling the content area, with
 * win->content_root (user content) and win->status_bar_node (hidden by
 * default) as children.  win->title_bar_node is left NULL — the platform
 * title bar is owned by the OS.
 *
 * win  Newly created window to initialise.
 */
void ca_title_bar_init(Ca_Window *win)
{
    assert(win);

    Ca_Node *root = ca_node_root(win);
    assert(root && "ca_title_bar_init: failed to allocate root node");
    root->desc.direction  = CA_VERTICAL;
    root->desc.overflow_x = 1;
    root->desc.overflow_y = 1;
    root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;

    win->title_bar_node = NULL;

    Ca_NodeDesc cr = {0};
    cr.direction = CA_VERTICAL;
    cr.flex_grow = 1.0f;
    Ca_Node *crnode = ca_node_add(root, &cr);
    assert(crnode && "ca_title_bar_init: failed to allocate content_root");
    win->content_root = crnode;

    Ca_NodeDesc sb = {0};
    sb.direction  = CA_HORIZONTAL;
    sb.height     = 0.0f;
    sb.hidden     = true;
    sb.overflow_x = 1;
    sb.overflow_y = 1;
    Ca_Node *sbnode = ca_node_add(root, &sb);
    assert(sbnode && "ca_title_bar_init: failed to allocate status_bar_node");
    win->status_bar_node = sbnode;
}

/* ------------------------------------------------------------------ */
/* Title bar rebuild — no-op (platform owns the title bar)            */
/* ------------------------------------------------------------------ */

/*
 * No-op for decorated windows — the platform title bar is not rebuilt.
 *
 * win  Window (unused).
 */
void ca_title_bar_rebuild(Ca_Window *win)
{
    (void)win;
}

/* ------------------------------------------------------------------ */
/* Public title API                                                    */
/* ------------------------------------------------------------------ */

/*
 * Set the window title displayed in the platform title bar.
 *
 * window  Window to update.
 * title   New title string; NULL is treated as an empty string.
 */
void ca_window_set_title(Ca_Window *window, const char *title)
{
    if (!window || !window->in_use) return;
    snprintf(window->title, sizeof(window->title), "%s", title ? title : "");
    glfwSetWindowTitle(window->glfw, window->title);
}

/*
 * No-op for decorated windows — menus are provided by the platform or
 * built inside the content area by the application.
 *
 * window  Window (unused).
 * menus   Menu descriptors (unused).
 * count   Menu count (unused).
 */
void ca_window_set_title_bar_menus(Ca_Window        *window,
                                   const Ca_MenuDesc *menus, int count)
{
    (void)window;
    (void)menus;
    (void)count;
}

/* ------------------------------------------------------------------ */
/* Status bar — public + internal                                      */
/* ------------------------------------------------------------------ */

/*
 * Rebuild the status bar from the registered builder function.
 *
 * win  Window whose status bar needs rebuilding.
 */
void ca_status_bar_rebuild(Ca_Window *win)
{
    assert(win && win->status_bar_node);

    ca_div_clear((Ca_Div *)win->status_bar_node);

    if (win->status_bar_fn)
        win->status_bar_fn(win, win->status_bar_data);

    ca_div_end();
}

/*
 * Install or remove a status bar builder on a window.
 *
 * window     Window to update.
 * fn         Builder callback; NULL removes the status bar.
 * user_data  Passed verbatim to fn.
 * height     Logical (unscaled) height in pixels; ignored when fn is NULL.
 */
void ca_window_set_status_bar(Ca_Window      *window,
                              Ca_StatusBarFn  fn,
                              void           *user_data,
                              float           height)
{
    if (!window || !window->in_use || !window->status_bar_node)
        return;

    window->status_bar_fn     = fn;
    window->status_bar_data   = user_data;
    {
        float sc = window->ui_scale > 0.0f ? window->ui_scale : 1.0f;
        window->status_bar_raw_height = (height > 0.0f && fn) ? height : 0.0f;
        window->status_bar_height = (height > 0.0f && fn) ? (height * sc) : 0.0f;
    }

    Ca_Node *sb = window->status_bar_node;
    sb->desc.height = window->status_bar_height;
    sb->desc.hidden = (fn == NULL || window->status_bar_height <= 0.0f);
    sb->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;

    if (window->root)
        window->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN;

    window->statusbar_needs_rebuild = true;
}

/*
 * Mark the status bar as needing a rebuild on the next frame.
 *
 * window  Window whose status bar content has changed.
 */
void ca_window_invalidate_status_bar(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    window->statusbar_needs_rebuild = true;
}
