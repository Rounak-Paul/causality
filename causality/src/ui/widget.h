// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* widget.h — internal widget implementation helpers */
#pragma once

#include "ca_internal.h"
#include "css.h"

/** The widget's own foreground colour, or 0 when it has none or uses the palette default. */
uint32_t ca_widget_text_color(const Ca_Node *node);

/** The colour a widget paints its text with when it has no colour of its own. */
uint32_t ca_widget_default_text_color(const Ca_Node *node);

/** Sets the widget's foreground colour and marks it for repaint. */
void ca_widget_set_text_color(Ca_Node *node, uint32_t color);

/**
 * Resolves one keyframe's declarations the way a node's own CSS resolves.
 *
 * @param node  Node whose window scale and stylesheets apply.
 * @param owner Stylesheet that holds the keyframes; var() names resolve through its string pool.
 * @param decls Keyframe declarations.
 * @param count Number of declarations.
 * @param rs    Receives the resolved style (zeroed first).
 * @param desc  Receives the node descriptor those declarations produce (zeroed first).
 * @param text  Receives the resolved text colour (0 when unset).
 */
void ca_widget_resolve_keyframe(const Ca_Node *node, const Ca_Stylesheet *owner,
                                const Ca_CssDecl *decls, int count,
                                Ca_ResolvedStyle *rs, Ca_NodeDesc *desc, uint32_t *text);

/* Check mouse position against button hit-boxes and fire callbacks.
   Called once per window per frame inside ca_ui_update. */
void ca_widget_input_pass(Ca_Window *win);

/**
 * Advances every tab bar's sliding indicator toward its active tab.
 *
 * @param win Window whose tab bars are stepped.
 * @param now Current time in seconds (glfwGetTime).
 * @return true while an indicator is still moving or waiting on layout, so
 *         the caller keeps frames coming.
 */
bool ca_tabbar_tick(Ca_Window *win, double now);

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
/** Publish a horizontal offset change to its lazily allocated observer signal. */
void ca_node_sync_scroll_x_signal(Ca_Node *node);

/** Publishes changed laid-out sizes to every size signal in win; may run builders. */
void ca_window_sync_size_signals(Ca_Window *win);

/** Width reserved at a closable tab header's trailing edge for its close glyph. */
float ca_tab_close_zone(const Ca_Node *tab_node);

/** Release a source window's drag snapshot and invalidate pending readbacks. */
void ca_drag_preview_clear(Ca_Window *win);
/** Update the opt-in source snapshot and its non-interactive native window. */
void ca_drag_preview_update(Ca_Window *win);
