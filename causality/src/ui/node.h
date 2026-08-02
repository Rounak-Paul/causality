// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* node.h — internal header for the node tree system */
#pragma once

#include "ca_internal.h"

/* ---- System lifecycle (called by ca_ui_window_init/shutdown) ---- */
bool ca_node_system_init(Ca_Window *win);
void ca_node_system_shutdown(Ca_Window *win);

/* Bubble CA_DIRTY_LAYOUT up to ancestor nodes. */
void ca_node_propagate_layout(Ca_Window *win);

/* ---- Node tree API (used internally by widget.c) ---- */
Ca_Node *ca_node_root(Ca_Window *window);
Ca_Node *ca_node_add(Ca_Node *parent, const Ca_NodeDesc *desc);
void     ca_node_remove(Ca_Node *node);
void     ca_node_clear(Ca_Node *node);
void     ca_node_trim_children(Ca_Node *parent, uint32_t keep_count);
void     ca_node_set_desc(Ca_Node *node, const Ca_NodeDesc *desc);

/* Desc diff helpers — used by widget.c for post-CSS dirty detection */
bool     layout_desc_changed(const Ca_NodeDesc *a, const Ca_NodeDesc *b);
bool     content_desc_changed(const Ca_NodeDesc *a, const Ca_NodeDesc *b);

/** Ensures CPU draw-command storage and refreshes its direct-access alias. */
bool ca_window_reserve_draw_commands(Ca_Window *win, size_t minimum);

/** Ensures z-sort index storage and refreshes its direct-access alias. */
bool ca_window_reserve_sorted_indices(Ca_Window *win, size_t minimum);

/** Ensures retained paint-cache storage and refreshes its direct-access alias. */
bool ca_window_reserve_paint_cache(Ca_Window *win, size_t minimum);

/** Returns an existing property transition or appends a new stable slot. */
Ca_Transition *ca_node_transition_acquire(Ca_Node *node, uint8_t property);
