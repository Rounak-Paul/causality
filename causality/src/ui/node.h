/* node.h — internal header for the node tree system */
#pragma once

#include "ca_internal.h"

/* ---- System lifecycle (called by ca_ui_window_init/shutdown) ---- */
void ca_node_system_init(Ca_Window *win);
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
void     ca_node_subscribe(Ca_Node *node, Ca_State *state, Ca_DirtyFlags on_change);
