// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* widget.h — internal widget implementation helpers */
#pragma once

#include "ca_internal.h"

/* Check mouse position against button hit-boxes and fire callbacks.
   Called once per window per frame inside ca_ui_update. */
void ca_widget_input_pass(Ca_Window *win);

/* Activate / deactivate the widget build context for a window.
   Used by ca_ui_update to wrap the on_frame callback so that
   widget creation functions work inside per-frame callbacks. */
void ca_widget_ctx_enter(Ca_Window *win);
void ca_widget_ctx_leave(void);

/** Releases retained process-global build storage owned by an instance. */
void ca_widget_ctx_release_instance(Ca_Instance *instance);

/* Re-resolve CSS for a single node without rebuilding its subtree.
   Resets node->desc = node->base_desc, runs ca_style_resolve +
   ca_style_apply_to_node, then diffs the post-CSS desc against the
   pre-reapply desc and sets CA_DIRTY_CONTENT / CA_DIRTY_LAYOUT only
   when the resolved values actually changed. Called from the
   hover/focus/active chain walk in ca_ui_update so pseudo-state CSS
   rules (:hover etc.) take effect on builder-pattern panels (whose
   builders don't re-run every frame). */
void ca_widget_reapply_css(Ca_Node *node);

/* Re-resolve every CSS-owned field, including layout, after a stylesheet swap. */
void ca_widget_refresh_css(Ca_Node *node);

/* Mirrors node->scroll_y into its lazily-created scroll_y_signal (see
   ca_get_scroll_y_signal), if one has ever been requested. No-op for
   nodes nothing has ever asked to observe reactively. Called from every
   scroll_y mutation site, including layout.c's auto-clamp when shrinking
   content pulls scroll_y back into range — a scroll change from any
   source must be visible to ca_div_set_builder consumers subscribed via
   ca_get_scroll_y_signal, not just user-drag/wheel mutations. */
void ca_node_sync_scroll_y_signal(Ca_Node *node);
