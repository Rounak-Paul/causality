// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_dock_internal.h — layout storage shared by the dock model and view. */
#pragma once

#include "ca_dock.h"
#include "ca_array.h"

/* One registered panel. */
typedef struct Ca_DockPanelEntry {
    char id[CA_DOCK_ID_MAX];
    int  group;                 /* hosting group, or CA_DOCK_NONE              */
    int  home;                  /* closed panel's remembered group, or NONE    */
} Ca_DockPanelEntry;

/* One tree node. Free slots keep their tab storage for reuse. */
typedef struct Ca_DockNodeEntry {
    Ca_DockNodeKind kind;
    bool            horizontal; /* split: children side by side       */
    int             parent;     /* parent split, or CA_DOCK_NONE      */
    int             first;      /* split children                     */
    int             second;
    float           ratio;      /* split: fraction given to `first`   */
    float           size;       /* split: logical px of the fixed pane (child
                                   without the central group); 0 = derive */
    int             active;     /* group: active tab                  */
    bool            central;    /* group: the main window's central group */
    Ca_DynArray     tabs;       /* group: panel indices (int)         */
} Ca_DockNodeEntry;

/* One window's tree; index 0 is the host's main window and is always used. */
typedef struct Ca_DockWindowEntry {
    bool used;
    int  root;                  /* root node, or CA_DOCK_NONE */
    int  x, y, width, height;   /* last known client rect     */
} Ca_DockWindowEntry;

struct Ca_DockLayout {
    Ca_DynArray panels;         /* Ca_DockPanelEntry  */
    Ca_DynArray nodes;          /* Ca_DockNodeEntry   */
    Ca_DynArray windows;        /* Ca_DockWindowEntry */
};

/** @return Node at index, or NULL when out of range. */
Ca_DockNodeEntry *ca_dock_node(Ca_DockLayout *layout, int index);
const Ca_DockNodeEntry *ca_dock_node_const(const Ca_DockLayout *layout, int index);

/** @return Window entry at index, or NULL when out of range. */
Ca_DockWindowEntry *ca_dock_window_entry(Ca_DockLayout *layout, int index);
const Ca_DockWindowEntry *ca_dock_window_entry_const(const Ca_DockLayout *layout, int index);

/** @return Number of node slots (used or free). */
int ca_dock_node_count(const Ca_DockLayout *layout);

/** @return Tab count of a group, or 0 for anything else. */
int ca_dock_group_tab_count(const Ca_DockLayout *layout, int group);

/** @return Panel at tab position `index` of a group, or CA_DOCK_NONE. */
int ca_dock_group_tab(const Ca_DockLayout *layout, int group, int index);

/** @return First group of a subtree in tree order, or CA_DOCK_NONE. */
int ca_dock_first_group(const Ca_DockLayout *layout, int node);

/** @return First visible group of a subtree in tree order, or CA_DOCK_NONE. */
int ca_dock_first_visible_group(const Ca_DockLayout *layout, int node);

/** @return Number of visible groups in a subtree. */
int ca_dock_visible_group_count(const Ca_DockLayout *layout, int node);

/** @return True when the subtree rooted at node contains the central group. */
bool ca_dock_contains_central(const Ca_DockLayout *layout, int node);
