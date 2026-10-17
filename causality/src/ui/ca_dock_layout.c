// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_dock_layout.c — pure dock layout model, validation and JSON persistence.
 *
 * Semantics follow Dear ImGui's docking (see ca_dock.h BEHAVIOUR): a central
 * group never collapses, undocking forgets a panel's place while closing
 * remembers it in a hidden group, and splits carry the pixel size of the
 * pane beside the central path.
 *
 * Mutations claim every node, window slot and tab capacity they need before
 * touching the tree, so a failed allocation leaves the layout unchanged
 * without snapshot copies. Removal never allocates.
 */
#include "ca_dock_internal.h"
#include "causality_config.h"

#include <string.h>

#define RATIO_MIN        0.05f
#define RATIO_MAX        0.95f
#define LOAD_DEPTH_MAX   256

/* ============================================================
   STORAGE ACCESS
   ============================================================ */

Ca_DockNodeEntry *ca_dock_node(Ca_DockLayout *layout, int index)
{
    if (!layout || index < 0) return NULL;
    return ca_dyn_array_at(&layout->nodes, (size_t)index);
}

const Ca_DockNodeEntry *ca_dock_node_const(const Ca_DockLayout *layout, int index)
{
    if (!layout || index < 0) return NULL;
    return ca_dyn_array_at_const(&layout->nodes, (size_t)index);
}

Ca_DockWindowEntry *ca_dock_window_entry(Ca_DockLayout *layout, int index)
{
    if (!layout || index < 0) return NULL;
    return ca_dyn_array_at(&layout->windows, (size_t)index);
}

const Ca_DockWindowEntry *ca_dock_window_entry_const(const Ca_DockLayout *layout, int index)
{
    if (!layout || index < 0) return NULL;
    return ca_dyn_array_at_const(&layout->windows, (size_t)index);
}

int ca_dock_node_count(const Ca_DockLayout *layout)
{
    return layout ? (int)layout->nodes.count : 0;
}

static Ca_DockPanelEntry *panel_at(Ca_DockLayout *layout, int index)
{
    if (!layout || index < 0) return NULL;
    return ca_dyn_array_at(&layout->panels, (size_t)index);
}

static const Ca_DockPanelEntry *panel_at_const(const Ca_DockLayout *layout, int index)
{
    if (!layout || index < 0) return NULL;
    return ca_dyn_array_at_const(&layout->panels, (size_t)index);
}

static bool is_group(const Ca_DockLayout *layout, int index)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, index);
    return node && node->kind == CA_DOCK_NODE_GROUP;
}

static bool is_live_node(const Ca_DockLayout *layout, int index)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, index);
    return node && node->kind != CA_DOCK_NODE_NONE;
}

static bool zone_valid(Ca_DockZone zone)
{
    return (unsigned)zone <= (unsigned)CA_DOCK_ZONE_BOTTOM;
}

static float clamp_ratio(float ratio)
{
    if (!(ratio > RATIO_MIN)) return RATIO_MIN;
    return ratio > RATIO_MAX ? RATIO_MAX : ratio;
}

/* Fraction for an incoming pane; <= 0 (or NaN) means an even split. */
static float incoming_ratio(float ratio)
{
    return ratio > 0.0f ? clamp_ratio(ratio) : 0.5f;
}

int ca_dock_group_tab_count(const Ca_DockLayout *layout, int group)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, group);
    return node && node->kind == CA_DOCK_NODE_GROUP ? (int)node->tabs.count : 0;
}

int ca_dock_group_tab(const Ca_DockLayout *layout, int group, int index)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, group);
    if (!node || node->kind != CA_DOCK_NODE_GROUP || index < 0) return CA_DOCK_NONE;
    const int *panel = ca_dyn_array_at_const(&node->tabs, (size_t)index);
    return panel ? *panel : CA_DOCK_NONE;
}

int ca_dock_first_group(const Ca_DockLayout *layout, int node)
{
    const int limit = ca_dock_node_count(layout);
    for (int depth = 0; depth <= limit; ++depth) {
        const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
        if (!entry || entry->kind == CA_DOCK_NODE_NONE) return CA_DOCK_NONE;
        if (entry->kind == CA_DOCK_NODE_GROUP) return node;
        node = entry->first;
    }
    return CA_DOCK_NONE;
}

static bool visible_rec(const Ca_DockLayout *layout, int node, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
    if (!entry || depth > ca_dock_node_count(layout)) return false;
    if (entry->kind == CA_DOCK_NODE_GROUP) return entry->tabs.count > 0 || entry->central;
    if (entry->kind != CA_DOCK_NODE_SPLIT) return false;
    return visible_rec(layout, entry->first, depth + 1) || visible_rec(layout, entry->second, depth + 1);
}

bool ca_dock_layout_node_visible(const Ca_DockLayout *layout, int node)
{
    return visible_rec(layout, node, 0);
}

static int first_visible_rec(const Ca_DockLayout *layout, int node, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
    if (!entry || depth > ca_dock_node_count(layout)) return CA_DOCK_NONE;
    if (entry->kind == CA_DOCK_NODE_GROUP)
        return entry->tabs.count > 0 || entry->central ? node : CA_DOCK_NONE;
    if (entry->kind != CA_DOCK_NODE_SPLIT) return CA_DOCK_NONE;
    const int found = first_visible_rec(layout, entry->first, depth + 1);
    return found != CA_DOCK_NONE ? found : first_visible_rec(layout, entry->second, depth + 1);
}

int ca_dock_first_visible_group(const Ca_DockLayout *layout, int node)
{
    return first_visible_rec(layout, node, 0);
}

static int visible_count_rec(const Ca_DockLayout *layout, int node, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
    if (!entry || depth > ca_dock_node_count(layout)) return 0;
    if (entry->kind == CA_DOCK_NODE_GROUP) return entry->tabs.count > 0 || entry->central ? 1 : 0;
    if (entry->kind != CA_DOCK_NODE_SPLIT) return 0;
    return visible_count_rec(layout, entry->first, depth + 1) +
           visible_count_rec(layout, entry->second, depth + 1);
}

int ca_dock_visible_group_count(const Ca_DockLayout *layout, int node)
{
    return visible_count_rec(layout, node, 0);
}

int ca_dock_layout_central(const Ca_DockLayout *layout)
{
    for (int i = 0; i < ca_dock_node_count(layout); ++i) {
        const Ca_DockNodeEntry *node = ca_dock_node_const(layout, i);
        if (node->kind == CA_DOCK_NODE_GROUP && node->central) return i;
    }
    return CA_DOCK_NONE;
}

bool ca_dock_contains_central(const Ca_DockLayout *layout, int node)
{
    int at = ca_dock_layout_central(layout);
    const int limit = ca_dock_node_count(layout);
    for (int depth = 0; at != CA_DOCK_NONE && depth <= limit; ++depth) {
        if (at == node) return true;
        at = ca_dock_node_const(layout, at)->parent;
    }
    return false;
}

/* True when a closed panel remembers this group as its home. */
static bool group_remembered(const Ca_DockLayout *layout, int group)
{
    for (size_t i = 0; i < layout->panels.count; ++i) {
        const Ca_DockPanelEntry *panel = ca_dyn_array_at_const(&layout->panels, i);
        if (panel->group == CA_DOCK_NONE && panel->home == group) return true;
    }
    return false;
}

/* Claims a free node slot (reusing released ones) and initialises it. */
static int claim_node(Ca_DockLayout *layout, Ca_DockNodeKind kind)
{
    int index = CA_DOCK_NONE;
    for (size_t i = 0; i < layout->nodes.count; ++i) {
        const Ca_DockNodeEntry *node = ca_dyn_array_at_const(&layout->nodes, i);
        if (node->kind == CA_DOCK_NODE_NONE) {
            index = (int)i;
            break;
        }
    }
    if (index == CA_DOCK_NONE) {
        if (layout->nodes.count >= (size_t)INT32_MAX) return CA_DOCK_NONE;
        Ca_DockNodeEntry fresh = { .tabs = CA_DYN_ARRAY_INIT(int) };
        if (!ca_dyn_array_push(&layout->nodes, &fresh)) return CA_DOCK_NONE;
        index = (int)layout->nodes.count - 1;
    }
    Ca_DockNodeEntry *node = ca_dock_node(layout, index);
    Ca_DynArray tabs = node->tabs;
    ca_dyn_array_clear(&tabs);
    *node = (Ca_DockNodeEntry){
        .kind = kind, .parent = CA_DOCK_NONE, .first = CA_DOCK_NONE, .second = CA_DOCK_NONE,
        .ratio = 0.5f, .tabs = tabs,
    };
    return index;
}

static void release_node(Ca_DockLayout *layout, int index)
{
    Ca_DockNodeEntry *node = ca_dock_node(layout, index);
    if (!node) return;
    Ca_DynArray tabs = node->tabs;
    ca_dyn_array_clear(&tabs);
    *node = (Ca_DockNodeEntry){
        .kind = CA_DOCK_NODE_NONE, .parent = CA_DOCK_NONE, .first = CA_DOCK_NONE,
        .second = CA_DOCK_NONE, .tabs = tabs,
    };
}

/* Claims an unused floating-window slot, appending one when none is free. */
static int claim_window(Ca_DockLayout *layout)
{
    for (size_t w = 1; w < layout->windows.count; ++w) {
        const Ca_DockWindowEntry *window = ca_dyn_array_at_const(&layout->windows, w);
        if (!window->used) return (int)w;
    }
    if (layout->windows.count >= (size_t)INT32_MAX) return CA_DOCK_NONE;
    const Ca_DockWindowEntry fresh = { .root = CA_DOCK_NONE };
    if (!ca_dyn_array_push(&layout->windows, &fresh)) return CA_DOCK_NONE;
    return (int)layout->windows.count - 1;
}

static bool reserve_tabs(Ca_DockLayout *layout, int group, size_t extra)
{
    Ca_DockNodeEntry *node = ca_dock_node(layout, group);
    if (!node || node->tabs.count > SIZE_MAX - extra) return false;
    return ca_dyn_array_reserve(&node->tabs, node->tabs.count + extra);
}

/* Inserts a panel into a group whose capacity was reserved; never fails. */
static void insert_tab(Ca_DockLayout *layout, int group, int panel, int index)
{
    Ca_DockNodeEntry *node = ca_dock_node(layout, group);
    if (index < 0 || (size_t)index > node->tabs.count) index = (int)node->tabs.count;
    ca_dyn_array_insert(&node->tabs, (size_t)index, &panel, 1);
    node->active = index;
    panel_at(layout, panel)->group = group;
}

/* Erases a panel from its group's tab list and fixes the active tab. */
static void erase_tab(Ca_DockLayout *layout, int group, int panel)
{
    Ca_DockNodeEntry *node = ca_dock_node(layout, group);
    for (size_t i = 0; i < node->tabs.count; ++i) {
        if (*(const int *)ca_dyn_array_at_const(&node->tabs, i) != panel) continue;
        ca_dyn_array_erase(&node->tabs, i, 1);
        if (node->active > (int)i) node->active--;
        if (node->tabs.count == 0) node->active = 0;
        else if ((size_t)node->active >= node->tabs.count) node->active = (int)node->tabs.count - 1;
        return;
    }
}

/* ============================================================
   LIFECYCLE
   ============================================================ */

static bool layout_init(Ca_DockLayout *layout)
{
    *layout = (Ca_DockLayout){
        .panels  = CA_DYN_ARRAY_INIT(Ca_DockPanelEntry),
        .nodes   = CA_DYN_ARRAY_INIT(Ca_DockNodeEntry),
        .windows = CA_DYN_ARRAY_INIT(Ca_DockWindowEntry),
    };
    const Ca_DockWindowEntry main_window = { .used = true, .root = CA_DOCK_NONE };
    return ca_dyn_array_push(&layout->windows, &main_window);
}

static void layout_release(Ca_DockLayout *layout)
{
    for (size_t i = 0; i < layout->nodes.count; ++i) {
        Ca_DockNodeEntry *node = ca_dyn_array_at(&layout->nodes, i);
        ca_dyn_array_destroy(&node->tabs);
    }
    ca_dyn_array_destroy(&layout->nodes);
    ca_dyn_array_destroy(&layout->panels);
    ca_dyn_array_destroy(&layout->windows);
}

Ca_DockLayout *ca_dock_layout_create(void)
{
    Ca_DockLayout *layout = CA_CALLOC(1, sizeof(*layout));
    if (!layout) return NULL;
    if (!layout_init(layout)) {
        layout_release(layout);
        CA_FREE(layout);
        return NULL;
    }
    return layout;
}

void ca_dock_layout_destroy(Ca_DockLayout *layout)
{
    if (!layout) return;
    layout_release(layout);
    CA_FREE(layout);
}

/* Moves src's storage into dst and releases dst's previous storage. */
static void layout_take(Ca_DockLayout *dst, Ca_DockLayout *src)
{
    Ca_DockLayout old = *dst;
    *dst = *src;
    layout_release(&old);
    *src = (Ca_DockLayout){0};
}

/* Deep copy into an uninitialised layout; on failure tmp is released. */
static bool layout_clone(Ca_DockLayout *tmp, const Ca_DockLayout *source)
{
    *tmp = (Ca_DockLayout){
        .panels  = CA_DYN_ARRAY_INIT(Ca_DockPanelEntry),
        .nodes   = CA_DYN_ARRAY_INIT(Ca_DockNodeEntry),
        .windows = CA_DYN_ARRAY_INIT(Ca_DockWindowEntry),
    };
    if (!ca_dyn_array_copy(&tmp->panels, &source->panels) ||
        !ca_dyn_array_copy(&tmp->windows, &source->windows) ||
        !ca_dyn_array_reserve(&tmp->nodes, source->nodes.count)) {
        layout_release(tmp);
        return false;
    }
    for (size_t i = 0; i < source->nodes.count; ++i) {
        const Ca_DockNodeEntry *from = ca_dyn_array_at_const(&source->nodes, i);
        Ca_DockNodeEntry copy = *from;
        copy.tabs = (Ca_DynArray)CA_DYN_ARRAY_INIT(int);
        if (!ca_dyn_array_copy(&copy.tabs, &from->tabs)) {
            layout_release(tmp);
            return false;
        }
        ca_dyn_array_push(&tmp->nodes, &copy);
    }
    return true;
}

bool ca_dock_layout_copy(Ca_DockLayout *destination, const Ca_DockLayout *source)
{
    if (!destination || !source) return false;
    if (destination == source) return true;
    Ca_DockLayout tmp;
    if (!layout_clone(&tmp, source)) return false;
    layout_take(destination, &tmp);
    return true;
}

/* ============================================================
   PANELS
   ============================================================ */

int ca_dock_layout_find(const Ca_DockLayout *layout, const char *id)
{
    if (!layout || !id) return CA_DOCK_NONE;
    for (size_t i = 0; i < layout->panels.count; ++i) {
        const Ca_DockPanelEntry *panel = ca_dyn_array_at_const(&layout->panels, i);
        if (strcmp(panel->id, id) == 0) return (int)i;
    }
    return CA_DOCK_NONE;
}

int ca_dock_layout_register(Ca_DockLayout *layout, const char *id)
{
    if (!layout || !id || !id[0]) return CA_DOCK_NONE;
    const size_t len = strlen(id);
    if (len >= CA_DOCK_ID_MAX) return CA_DOCK_NONE;
    const int existing = ca_dock_layout_find(layout, id);
    if (existing != CA_DOCK_NONE) return existing;
    if (layout->panels.count >= (size_t)INT32_MAX) return CA_DOCK_NONE;
    Ca_DockPanelEntry entry = { .group = CA_DOCK_NONE, .home = CA_DOCK_NONE };
    memcpy(entry.id, id, len + 1u);
    if (!ca_dyn_array_push(&layout->panels, &entry)) return CA_DOCK_NONE;
    return (int)layout->panels.count - 1;
}

int ca_dock_layout_panel_count(const Ca_DockLayout *layout)
{
    return layout ? (int)layout->panels.count : 0;
}

const char *ca_dock_layout_panel_id(const Ca_DockLayout *layout, int panel)
{
    const Ca_DockPanelEntry *entry = panel_at_const(layout, panel);
    return entry ? entry->id : NULL;
}

int ca_dock_layout_group_of(const Ca_DockLayout *layout, int panel)
{
    const Ca_DockPanelEntry *entry = panel_at_const(layout, panel);
    return entry ? entry->group : CA_DOCK_NONE;
}

int ca_dock_layout_home_of(const Ca_DockLayout *layout, int panel)
{
    const Ca_DockPanelEntry *entry = panel_at_const(layout, panel);
    return entry && entry->group == CA_DOCK_NONE ? entry->home : CA_DOCK_NONE;
}

/* ============================================================
   TREE QUERIES
   ============================================================ */

Ca_DockNodeKind ca_dock_layout_node_kind(const Ca_DockLayout *layout, int node)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
    return entry ? entry->kind : CA_DOCK_NODE_NONE;
}

int ca_dock_layout_window_count(const Ca_DockLayout *layout)
{
    return layout ? (int)layout->windows.count : 0;
}

int ca_dock_layout_window_root(const Ca_DockLayout *layout, int window)
{
    const Ca_DockWindowEntry *entry = ca_dock_window_entry_const(layout, window);
    return entry && entry->used ? entry->root : CA_DOCK_NONE;
}

int ca_dock_layout_window_of(const Ca_DockLayout *layout, int node)
{
    if (!is_live_node(layout, node)) return CA_DOCK_NONE;
    const int limit = ca_dock_node_count(layout);
    for (int depth = 0; depth <= limit; ++depth) {
        const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
        if (!entry) return CA_DOCK_NONE;
        if (entry->parent == CA_DOCK_NONE) break;
        node = entry->parent;
    }
    for (size_t w = 0; w < layout->windows.count; ++w) {
        const Ca_DockWindowEntry *window = ca_dyn_array_at_const(&layout->windows, w);
        if (window->used && window->root == node) return (int)w;
    }
    return CA_DOCK_NONE;
}

int ca_dock_layout_tab_index(const Ca_DockLayout *layout, int panel)
{
    const int group = ca_dock_layout_group_of(layout, panel);
    const int count = ca_dock_group_tab_count(layout, group);
    for (int i = 0; i < count; ++i)
        if (ca_dock_group_tab(layout, group, i) == panel) return i;
    return CA_DOCK_NONE;
}

int ca_dock_layout_active_panel(const Ca_DockLayout *layout, int group)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, group);
    if (!node || node->kind != CA_DOCK_NODE_GROUP || node->tabs.count == 0) return CA_DOCK_NONE;
    const int active = node->active >= 0 && (size_t)node->active < node->tabs.count ? node->active : 0;
    return ca_dock_group_tab(layout, group, active);
}

/* ============================================================
   TREE MUTATION
   ============================================================ */

void ca_dock_layout_clear(Ca_DockLayout *layout)
{
    if (!layout) return;
    for (size_t i = 0; i < layout->nodes.count; ++i) release_node(layout, (int)i);
    for (size_t i = 0; i < layout->panels.count; ++i) {
        Ca_DockPanelEntry *panel = ca_dyn_array_at(&layout->panels, i);
        panel->group = CA_DOCK_NONE;
        panel->home = CA_DOCK_NONE;
    }
    ca_dyn_array_resize(&layout->windows, 1);
    *ca_dock_window_entry(layout, CA_DOCK_MAIN_WINDOW) = (Ca_DockWindowEntry){
        .used = true, .root = CA_DOCK_NONE,
    };
}

int ca_dock_layout_new_group(Ca_DockLayout *layout)
{
    return layout ? claim_node(layout, CA_DOCK_NODE_GROUP) : CA_DOCK_NONE;
}

int ca_dock_layout_new_split(Ca_DockLayout *layout, bool horizontal, float ratio,
                             int first, int second)
{
    if (!layout || first == second || !is_live_node(layout, first) || !is_live_node(layout, second))
        return CA_DOCK_NONE;
    if (ca_dock_node_const(layout, first)->parent != CA_DOCK_NONE ||
        ca_dock_node_const(layout, second)->parent != CA_DOCK_NONE ||
        ca_dock_layout_window_of(layout, first) != CA_DOCK_NONE ||
        ca_dock_layout_window_of(layout, second) != CA_DOCK_NONE)
        return CA_DOCK_NONE;
    const int index = claim_node(layout, CA_DOCK_NODE_SPLIT);
    if (index == CA_DOCK_NONE) return CA_DOCK_NONE;
    Ca_DockNodeEntry *split = ca_dock_node(layout, index);
    split->horizontal = horizontal;
    split->ratio = clamp_ratio(ratio);
    split->first = first;
    split->second = second;
    ca_dock_node(layout, first)->parent = index;
    ca_dock_node(layout, second)->parent = index;
    return index;
}

bool ca_dock_layout_set_root(Ca_DockLayout *layout, int window, int node)
{
    Ca_DockWindowEntry *entry = ca_dock_window_entry(layout, window);
    if (!entry) return false;
    if (node != CA_DOCK_NONE) {
        const Ca_DockNodeEntry *n = ca_dock_node_const(layout, node);
        if (!n || n->kind == CA_DOCK_NODE_NONE || n->parent != CA_DOCK_NONE) return false;
        const int owner = ca_dock_layout_window_of(layout, node);
        if (owner != CA_DOCK_NONE && owner != window) return false;
    }
    entry->used = true;
    entry->root = node;
    return true;
}

bool ca_dock_layout_set_split_size(Ca_DockLayout *layout, int split, float size)
{
    Ca_DockNodeEntry *node = ca_dock_node(layout, split);
    if (!node || node->kind != CA_DOCK_NODE_SPLIT) return false;
    node->size = size > 0.0f ? size : 0.0f;
    return true;
}

/* Replaces old_child under its parent (or as a window root) with new_child. */
static void replace_child(Ca_DockLayout *layout, int old_child, int new_child)
{
    const int parent = ca_dock_node(layout, old_child)->parent;
    if (parent == CA_DOCK_NONE) {
        const int window = ca_dock_layout_window_of(layout, old_child);
        if (window != CA_DOCK_NONE) ca_dock_window_entry(layout, window)->root = new_child;
        if (new_child != CA_DOCK_NONE) ca_dock_node(layout, new_child)->parent = CA_DOCK_NONE;
        return;
    }
    Ca_DockNodeEntry *split = ca_dock_node(layout, parent);
    if (split->first == old_child) split->first = new_child;
    else split->second = new_child;
    if (new_child != CA_DOCK_NONE) ca_dock_node(layout, new_child)->parent = parent;
}

/* Drops an empty group and collapses its parent split onto the sibling. */
static void collapse_group(Ca_DockLayout *layout, int group)
{
    const int parent = ca_dock_node(layout, group)->parent;
    if (parent == CA_DOCK_NONE) {
        const int window = ca_dock_layout_window_of(layout, group);
        if (window != CA_DOCK_NONE) {
            Ca_DockWindowEntry *entry = ca_dock_window_entry(layout, window);
            entry->root = CA_DOCK_NONE;
            if (window != CA_DOCK_MAIN_WINDOW) entry->used = false;
        }
        release_node(layout, group);
        return;
    }
    const Ca_DockNodeEntry *split = ca_dock_node(layout, parent);
    const int sibling = split->first == group ? split->second : split->first;
    replace_child(layout, parent, sibling);
    release_node(layout, group);
    release_node(layout, parent);
}

/* Collapses a group that no longer holds, centres or remembers anything. */
static void prune_group(Ca_DockLayout *layout, int group)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, group);
    if (!node || node->kind != CA_DOCK_NODE_GROUP || node->tabs.count > 0 || node->central) return;
    if (group_remembered(layout, group)) return;
    collapse_group(layout, group);
}

/* Detaches a panel from its current group without touching its home. */
static void take_panel(Ca_DockLayout *layout, int panel)
{
    Ca_DockPanelEntry *entry = panel_at(layout, panel);
    const int group = entry->group;
    entry->group = CA_DOCK_NONE;
    if (!is_group(layout, group)) return;
    erase_tab(layout, group, panel);
    prune_group(layout, group);
}

/* Final step of every placement: a docked panel forgets its old home. */
static void settle_home(Ca_DockLayout *layout, int panel)
{
    Ca_DockPanelEntry *entry = panel_at(layout, panel);
    const int old = entry->home;
    entry->home = CA_DOCK_NONE;
    if (old != CA_DOCK_NONE && old != entry->group) prune_group(layout, old);
}

void ca_dock_layout_remove(Ca_DockLayout *layout, int panel)
{
    Ca_DockPanelEntry *entry = panel_at(layout, panel);
    if (!entry) return;
    const int old_home = entry->home;
    entry->home = CA_DOCK_NONE;
    take_panel(layout, panel);
    if (old_home != CA_DOCK_NONE) prune_group(layout, old_home);
}

void ca_dock_layout_close(Ca_DockLayout *layout, int panel)
{
    Ca_DockPanelEntry *entry = panel_at(layout, panel);
    if (!entry || !is_group(layout, entry->group)) return;
    const int group = entry->group;
    erase_tab(layout, group, panel);
    entry->group = CA_DOCK_NONE;
    entry->home = group;
}

bool ca_dock_layout_set_central(Ca_DockLayout *layout, int group)
{
    if (!layout) return false;
    if (group != CA_DOCK_NONE &&
        (!is_group(layout, group) || ca_dock_layout_window_of(layout, group) != CA_DOCK_MAIN_WINDOW))
        return false;
    const int old = ca_dock_layout_central(layout);
    if (old == group) return true;
    if (group != CA_DOCK_NONE) ca_dock_node(layout, group)->central = true;
    if (old != CA_DOCK_NONE) {
        ca_dock_node(layout, old)->central = false;
        prune_group(layout, old);
    }
    return true;
}

bool ca_dock_layout_add_tab(Ca_DockLayout *layout, int group, int panel, int index)
{
    if (!is_group(layout, group) || !panel_at(layout, panel)) return false;
    if (panel_at(layout, panel)->group == group) {
        Ca_DockNodeEntry *node = ca_dock_node(layout, group);
        const int count = (int)node->tabs.count;
        int at = 0;
        while (at < count && ca_dock_group_tab(layout, group, at) != panel) ++at;
        if (index < 0 || index > count) index = count;
        if (index > at) index--;
        if (at != index) {
            ca_dyn_array_erase(&node->tabs, (size_t)at, 1);
            ca_dyn_array_insert(&node->tabs, (size_t)index, &panel, 1);
        }
        node->active = index;
        return true;
    }
    if (!reserve_tabs(layout, group, 1)) return false;
    take_panel(layout, panel);
    insert_tab(layout, group, panel, index);
    settle_home(layout, panel);
    return true;
}

/* Fills a freshly claimed split; `ratio` is the incoming pane's fraction. */
static void link_split(Ca_DockLayout *layout, int split, Ca_DockZone zone,
                       int incoming, int existing, float ratio)
{
    const bool incoming_first = zone == CA_DOCK_ZONE_LEFT || zone == CA_DOCK_ZONE_TOP;
    Ca_DockNodeEntry *node = ca_dock_node(layout, split);
    node->horizontal = zone == CA_DOCK_ZONE_LEFT || zone == CA_DOCK_ZONE_RIGHT;
    node->ratio = incoming_first ? ratio : 1.0f - ratio;
    node->size = 0.0f;
    node->first = incoming_first ? incoming : existing;
    node->second = incoming_first ? existing : incoming;
    ca_dock_node(layout, incoming)->parent = split;
    ca_dock_node(layout, existing)->parent = split;
}

bool ca_dock_layout_split(Ca_DockLayout *layout, int target, int panel, Ca_DockZone zone, float ratio)
{
    if (!is_group(layout, target) || !panel_at(layout, panel) || !zone_valid(zone)) return false;
    if (zone == CA_DOCK_ZONE_TAB) return ca_dock_layout_add_tab(layout, target, panel, -1);
    if (ca_dock_group_tab_count(layout, target) == 1 && ca_dock_group_tab(layout, target, 0) == panel)
        return false;

    const int group = claim_node(layout, CA_DOCK_NODE_GROUP);
    const int split = group == CA_DOCK_NONE ? CA_DOCK_NONE : claim_node(layout, CA_DOCK_NODE_SPLIT);
    if (split == CA_DOCK_NONE || !reserve_tabs(layout, group, 1)) {
        if (group != CA_DOCK_NONE) release_node(layout, group);
        if (split != CA_DOCK_NONE) release_node(layout, split);
        return false;
    }
    take_panel(layout, panel);
    insert_tab(layout, group, panel, 0);
    replace_child(layout, target, split);
    link_split(layout, split, zone, group, target, incoming_ratio(ratio));
    settle_home(layout, panel);
    return true;
}

bool ca_dock_layout_dock_root(Ca_DockLayout *layout, int window, int panel, Ca_DockZone zone, float ratio)
{
    const Ca_DockWindowEntry *entry = ca_dock_window_entry_const(layout, window);
    if (!entry || !entry->used || !panel_at(layout, panel) || !zone_valid(zone)) return false;
    const int root = entry->root;
    if (is_group(layout, root) && ca_dock_group_tab_count(layout, root) == 1 &&
        ca_dock_group_tab(layout, root, 0) == panel)
        return false;
    if (root != CA_DOCK_NONE && zone == CA_DOCK_ZONE_TAB) {
        const int visible = ca_dock_first_visible_group(layout, root);
        return ca_dock_layout_add_tab(layout, visible != CA_DOCK_NONE ? visible : ca_dock_first_group(layout, root),
                                      panel, -1);
    }

    const int group = claim_node(layout, CA_DOCK_NODE_GROUP);
    const int split = group == CA_DOCK_NONE || root == CA_DOCK_NONE
        ? CA_DOCK_NONE : claim_node(layout, CA_DOCK_NODE_SPLIT);
    if (group == CA_DOCK_NONE || (root != CA_DOCK_NONE && split == CA_DOCK_NONE) ||
        !reserve_tabs(layout, group, 1)) {
        if (group != CA_DOCK_NONE) release_node(layout, group);
        if (split != CA_DOCK_NONE) release_node(layout, split);
        return false;
    }
    take_panel(layout, panel);
    insert_tab(layout, group, panel, 0);

    Ca_DockWindowEntry *target = ca_dock_window_entry(layout, window);
    const int current = target->root;
    if (current == CA_DOCK_NONE) {
        if (split != CA_DOCK_NONE) release_node(layout, split);
        target->root = group;
    } else {
        link_split(layout, split, zone, group, current, incoming_ratio(ratio));
        target->root = split;
    }
    settle_home(layout, panel);
    return true;
}

/* Collects a subtree's tabs in tree order into out (capacity checked by caller). */
static void collect_panels(const Ca_DockLayout *layout, int node, int *out, size_t *count, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
    if (!entry || depth > ca_dock_node_count(layout)) return;
    if (entry->kind == CA_DOCK_NODE_GROUP) {
        for (size_t i = 0; i < entry->tabs.count; ++i)
            out[(*count)++] = *(const int *)ca_dyn_array_at_const(&entry->tabs, i);
        return;
    }
    collect_panels(layout, entry->first, out, count, depth + 1);
    collect_panels(layout, entry->second, out, count, depth + 1);
}

/* Releases every node of a detached subtree. */
static void release_subtree(Ca_DockLayout *layout, int node, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(layout, node);
    if (!entry || entry->kind == CA_DOCK_NODE_NONE || depth > ca_dock_node_count(layout)) return;
    if (entry->kind == CA_DOCK_NODE_SPLIT) {
        const int first = entry->first, second = entry->second;
        release_subtree(layout, first, depth + 1);
        release_subtree(layout, second, depth + 1);
    }
    release_node(layout, node);
}

/* Points every closed panel remembered inside `window` at `group` instead. */
static void rehome_window(Ca_DockLayout *layout, int window, int group)
{
    for (size_t i = 0; i < layout->panels.count; ++i) {
        Ca_DockPanelEntry *panel = ca_dyn_array_at(&layout->panels, i);
        if (panel->group == CA_DOCK_NONE && panel->home != CA_DOCK_NONE &&
            panel->home != group && ca_dock_layout_window_of(layout, panel->home) == window)
            panel->home = group;
    }
}

static void consume_window(Ca_DockLayout *layout, int source)
{
    Ca_DockWindowEntry *consumed = ca_dock_window_entry(layout, source);
    consumed->root = CA_DOCK_NONE;
    consumed->used = false;
}

bool ca_dock_layout_dock_window(Ca_DockLayout *layout, int source, int window,
                                int target, Ca_DockZone zone, int index, float ratio)
{
    const Ca_DockWindowEntry *src = ca_dock_window_entry_const(layout, source);
    const Ca_DockWindowEntry *dst = ca_dock_window_entry_const(layout, window);
    if (source == CA_DOCK_MAIN_WINDOW || source == window || !src || !dst ||
        !src->used || !dst->used || !zone_valid(zone))
        return false;
    const int root = src->root;
    if (root == CA_DOCK_NONE) return false;

    if (target == CA_DOCK_NONE) {
        if (dst->root != CA_DOCK_NONE) return false;
        consume_window(layout, source);
        ca_dock_window_entry(layout, window)->root = root;
        return true;
    }
    if (!is_live_node(layout, target) || ca_dock_layout_window_of(layout, target) != window) return false;

    if (zone != CA_DOCK_ZONE_TAB) {
        const int split = claim_node(layout, CA_DOCK_NODE_SPLIT);
        if (split == CA_DOCK_NONE) return false;
        consume_window(layout, source);
        replace_child(layout, target, split);
        link_split(layout, split, zone, root, target, incoming_ratio(ratio));
        return true;
    }

    if (!is_group(layout, target)) return false;
    if (ca_dock_group_tab_count(layout, target) == 0) {
        /* An empty target is replaced by the whole tree, as ImGui does. */
        const bool central = ca_dock_node_const(layout, target)->central;
        int heir = ca_dock_first_visible_group(layout, root);
        if (heir == CA_DOCK_NONE) heir = ca_dock_first_group(layout, root);
        if (heir == CA_DOCK_NONE) return false;
        /* The tree moves intact, so only the replaced target's memories move. */
        for (size_t i = 0; i < layout->panels.count; ++i) {
            Ca_DockPanelEntry *panel = ca_dyn_array_at(&layout->panels, i);
            if (panel->group == CA_DOCK_NONE && panel->home == target) panel->home = heir;
        }
        consume_window(layout, source);
        replace_child(layout, target, root);
        release_node(layout, target);
        if (central) ca_dock_node(layout, heir)->central = true;
        return true;
    }

    /* A non-empty target only takes a tree with a single visible group. */
    if (ca_dock_visible_group_count(layout, root) > 1) return false;
    const int visible = ca_dock_first_visible_group(layout, root);
    const size_t total = (size_t)ca_dock_group_tab_count(layout, visible);
    int *panels = total ? CA_MALLOC(total * sizeof(int)) : NULL;
    if ((total && !panels) || !reserve_tabs(layout, target, total)) {
        CA_FREE(panels);
        return false;
    }
    size_t count = 0;
    collect_panels(layout, visible, panels, &count, 0);
    const int active = ca_dock_layout_active_panel(layout, visible);
    const int tabs = ca_dock_group_tab_count(layout, target);
    if (index < 0 || index > tabs) index = tabs;
    rehome_window(layout, source, target);
    for (size_t i = 0; i < count; ++i) {
        erase_tab(layout, visible, panels[i]);
        panel_at(layout, panels[i])->group = CA_DOCK_NONE;
        insert_tab(layout, target, panels[i], index + (int)i);
    }
    CA_FREE(panels);
    ca_dock_layout_set_active(layout, target, ca_dock_layout_tab_index(layout, active));
    const int remaining = ca_dock_window_entry(layout, source)->root;
    consume_window(layout, source);
    release_subtree(layout, remaining, 0);
    return true;
}

int ca_dock_layout_float(Ca_DockLayout *layout, int panel, int x, int y, int width, int height)
{
    if (!panel_at(layout, panel) || width <= 0 || height <= 0) return CA_DOCK_NONE;
    const int current = panel_at(layout, panel)->group;
    if (is_group(layout, current)) {
        const int window = ca_dock_layout_window_of(layout, current);
        if (window != CA_DOCK_MAIN_WINDOW && window != CA_DOCK_NONE &&
            ca_dock_group_tab_count(layout, current) == 1 &&
            ca_dock_node_const(layout, current)->parent == CA_DOCK_NONE)
            return CA_DOCK_NONE;
    }
    const int slot = claim_window(layout);
    const int group = slot == CA_DOCK_NONE ? CA_DOCK_NONE : claim_node(layout, CA_DOCK_NODE_GROUP);
    if (group == CA_DOCK_NONE || !reserve_tabs(layout, group, 1)) {
        if (group != CA_DOCK_NONE) release_node(layout, group);
        return CA_DOCK_NONE;
    }
    take_panel(layout, panel);
    insert_tab(layout, group, panel, 0);
    *ca_dock_window_entry(layout, slot) = (Ca_DockWindowEntry){
        .used = true, .root = group, .x = x, .y = y, .width = width, .height = height,
    };
    settle_home(layout, panel);
    return slot;
}

bool ca_dock_layout_set_active(Ca_DockLayout *layout, int group, int index)
{
    if (!is_group(layout, group) || index < 0 || index >= ca_dock_group_tab_count(layout, group))
        return false;
    ca_dock_node(layout, group)->active = index;
    return true;
}

/* ============================================================
   VALIDATION
   ============================================================ */

static bool subtree_valid(const Ca_DockLayout *layout, int index, int parent, int depth,
                          bool *visited, bool *panel_seen)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, index);
    if (!node || depth > ca_dock_node_count(layout) || visited[index] || node->parent != parent)
        return false;
    visited[index] = true;
    if (node->kind == CA_DOCK_NODE_GROUP) {
        if (node->tabs.count == 0) {
            if (!node->central && !group_remembered(layout, index)) return false;
        } else if (node->active < 0 || (size_t)node->active >= node->tabs.count) {
            return false;
        }
        for (size_t i = 0; i < node->tabs.count; ++i) {
            const int panel = *(const int *)ca_dyn_array_at_const(&node->tabs, i);
            const Ca_DockPanelEntry *entry = panel_at_const(layout, panel);
            if (!entry || panel_seen[panel] || entry->group != index) return false;
            panel_seen[panel] = true;
        }
        return true;
    }
    if (node->kind != CA_DOCK_NODE_SPLIT) return false;
    if (!(node->ratio >= RATIO_MIN && node->ratio <= RATIO_MAX) || !(node->size >= 0.0f)) return false;
    return subtree_valid(layout, node->first, index, depth + 1, visited, panel_seen) &&
           subtree_valid(layout, node->second, index, depth + 1, visited, panel_seen);
}

bool ca_dock_layout_valid(const Ca_DockLayout *layout)
{
    if (!layout || layout->windows.count == 0) return false;
    const Ca_DockWindowEntry *main_window = ca_dock_window_entry_const(layout, CA_DOCK_MAIN_WINDOW);
    if (!main_window->used) return false;

    const size_t nodes = layout->nodes.count, panels = layout->panels.count;
    bool *visited = nodes ? CA_CALLOC(nodes, sizeof(bool)) : NULL;
    bool *panel_seen = panels ? CA_CALLOC(panels, sizeof(bool)) : NULL;
    bool ok = (!nodes || visited) && (!panels || panel_seen);

    for (size_t w = 0; ok && w < layout->windows.count; ++w) {
        const Ca_DockWindowEntry *window = ca_dyn_array_at_const(&layout->windows, w);
        if (!window->used) continue;
        if (window->root == CA_DOCK_NONE) {
            ok = w == CA_DOCK_MAIN_WINDOW;
            continue;
        }
        ok = subtree_valid(layout, window->root, CA_DOCK_NONE, 0, visited, panel_seen);
    }
    for (size_t p = 0; ok && p < panels; ++p) {
        const Ca_DockPanelEntry *panel = ca_dyn_array_at_const(&layout->panels, p);
        if (panel->group != CA_DOCK_NONE) ok = panel_seen[p] && panel->home == CA_DOCK_NONE;
        else ok = panel->home == CA_DOCK_NONE ||
                  (is_group(layout, panel->home) && visited[panel->home]);
    }
    int centrals = 0;
    for (size_t i = 0; ok && i < nodes; ++i) {
        const Ca_DockNodeEntry *node = ca_dyn_array_at_const(&layout->nodes, i);
        ok = node->kind == CA_DOCK_NODE_NONE || visited[i];
        if (ok && node->kind == CA_DOCK_NODE_GROUP && node->central) {
            centrals++;
            ok = ca_dock_layout_window_of(layout, (int)i) == CA_DOCK_MAIN_WINDOW;
        }
    }
    ok = ok && centrals <= 1;
    CA_FREE(visited);
    CA_FREE(panel_seen);
    return ok;
}

/* ============================================================
   JSON WRITER — snprintf semantics, locale independent
   ============================================================ */

typedef struct JsonWriter {
    char  *out;
    size_t cap;
    size_t len;
} JsonWriter;

static void w_bytes(JsonWriter *w, const char *data, size_t n)
{
    if (w->len < w->cap) {
        const size_t room = w->cap - 1u - w->len;
        memcpy(w->out + w->len, data, n < room ? n : room);
    }
    w->len += n;
}

static void w_text(JsonWriter *w, const char *text)
{
    w_bytes(w, text, strlen(text));
}

static void w_string(JsonWriter *w, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    w_bytes(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            const char escaped[2] = { '\\', (char)*p };
            w_bytes(w, escaped, 2);
        } else if (*p < 0x20) {
            const char escaped[6] = { '\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15] };
            w_bytes(w, escaped, 6);
        } else {
            w_bytes(w, (const char *)p, 1);
        }
    }
    w_bytes(w, "\"", 1);
}

static void w_int(JsonWriter *w, long long value)
{
    char digits[24];
    int n = 0;
    unsigned long long magnitude = value < 0 ? 0ull - (unsigned long long)value : (unsigned long long)value;
    do {
        digits[n++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude);
    if (value < 0) w_bytes(w, "-", 1);
    while (n) w_bytes(w, &digits[--n], 1);
}

/* Fixed-point with four decimals; ratios are always within (0, 1). */
static void w_ratio(JsonWriter *w, float ratio)
{
    long long scaled = (long long)((double)ratio * 10000.0 + 0.5);
    w_int(w, scaled / 10000);
    char frac[6] = { '.', 0, 0, 0, 0, 0 };
    long long rem = scaled % 10000;
    for (int i = 4; i >= 1; --i) {
        frac[i] = (char)('0' + rem % 10);
        rem /= 10;
    }
    w_bytes(w, frac, 5);
}

static void w_node(JsonWriter *w, const Ca_DockLayout *layout, int index, int depth)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(layout, index);
    if (!node || depth > ca_dock_node_count(layout)) {
        w_text(w, "null");
        return;
    }
    if (node->kind == CA_DOCK_NODE_GROUP) {
        w_text(w, "{\"tabs\":[");
        for (size_t i = 0; i < node->tabs.count; ++i) {
            if (i) w_text(w, ",");
            const int panel = *(const int *)ca_dyn_array_at_const(&node->tabs, i);
            w_string(w, ca_dock_layout_panel_id(layout, panel));
        }
        w_text(w, "],\"active\":");
        w_int(w, node->active);
        if (node->central) w_text(w, ",\"central\":true");
        bool first_closed = true;
        for (size_t p = 0; p < layout->panels.count; ++p) {
            const Ca_DockPanelEntry *panel = ca_dyn_array_at_const(&layout->panels, p);
            if (panel->group != CA_DOCK_NONE || panel->home != index) continue;
            w_text(w, first_closed ? ",\"closed\":[" : ",");
            first_closed = false;
            w_string(w, panel->id);
        }
        if (!first_closed) w_text(w, "]");
        w_text(w, "}");
        return;
    }
    w_text(w, node->horizontal ? "{\"split\":\"h\",\"ratio\":" : "{\"split\":\"v\",\"ratio\":");
    w_ratio(w, node->ratio);
    if (node->size > 0.0f) {
        w_text(w, ",\"size\":");
        w_int(w, (long long)(node->size + 0.5f));
    }
    w_text(w, ",\"first\":");
    w_node(w, layout, node->first, depth + 1);
    w_text(w, ",\"second\":");
    w_node(w, layout, node->second, depth + 1);
    w_text(w, "}");
}

size_t ca_dock_layout_save(const Ca_DockLayout *layout, char *out, size_t cap)
{
    JsonWriter w = { .out = out, .cap = out ? cap : 0u };
    if (!layout) {
        if (w.cap) out[0] = '\0';
        return 0;
    }
    w_text(&w, "{\"windows\":[");
    bool first = true;
    for (size_t i = 0; i < layout->windows.count; ++i) {
        const Ca_DockWindowEntry *window = ca_dyn_array_at_const(&layout->windows, i);
        if (!window->used) continue;
        if (!first) w_text(&w, ",");
        first = false;
        w_text(&w, "{\"x\":");
        w_int(&w, window->x);
        w_text(&w, ",\"y\":");
        w_int(&w, window->y);
        w_text(&w, ",\"w\":");
        w_int(&w, window->width);
        w_text(&w, ",\"h\":");
        w_int(&w, window->height);
        w_text(&w, ",\"root\":");
        if (window->root == CA_DOCK_NONE) w_text(&w, "null");
        else w_node(&w, layout, window->root, 0);
        w_text(&w, "}");
    }
    w_text(&w, "]}");
    if (w.cap) out[w.len < w.cap ? w.len : w.cap - 1u] = '\0';
    return w.len;
}

/* ============================================================
   JSON READER — minimal, bounded-depth, locale independent
   ============================================================ */

typedef enum JsonType {
    JSON_NULL = 0, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT,
} JsonType;

typedef struct JsonValue {
    JsonType          type;
    bool              boolean;
    double            number;
    char             *string;   /* JSON_STRING                        */
    struct JsonValue *items;    /* JSON_ARRAY values / JSON_OBJECT values */
    char            **keys;     /* JSON_OBJECT keys, parallel to items */
    size_t            count;
} JsonValue;

typedef struct JsonParser {
    const char *at;
    int         depth;
} JsonParser;

static void json_free(JsonValue *value)
{
    if (!value) return;
    CA_FREE(value->string);
    for (size_t i = 0; i < value->count; ++i) {
        json_free(&value->items[i]);
        if (value->keys) CA_FREE(value->keys[i]);
    }
    CA_FREE(value->items);
    CA_FREE(value->keys);
    *value = (JsonValue){0};
}

static void skip_ws(JsonParser *p)
{
    while (*p->at == ' ' || *p->at == '\t' || *p->at == '\n' || *p->at == '\r') p->at++;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(JsonParser *p, unsigned *out)
{
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
        int d = hex_digit(p->at[i]);
        if (d < 0) return false;
        value = (value << 4) | (unsigned)d;
    }
    p->at += 4;
    *out = value;
    return true;
}

static size_t utf8_encode(unsigned cp, char *out)
{
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Parses a string literal at p->at (opening quote included) into a heap copy. */
static bool parse_string(JsonParser *p, char **out)
{
    if (*p->at != '"') return false;
    const char *start = ++p->at;
    size_t raw = 0;
    while (start[raw] && start[raw] != '"') {
        if (start[raw] == '\\' && start[raw + 1]) raw++;
        raw++;
    }
    if (start[raw] != '"') return false;
    /* Escapes never expand: \uXXXX (6 bytes) encodes to at most 4. */
    char *text = CA_MALLOC(raw + 1u);
    if (!text) return false;
    size_t n = 0;
    while (*p->at != '"') {
        unsigned char c = (unsigned char)*p->at++;
        if (c < 0x20) goto fail;
        if (c != '\\') {
            text[n++] = (char)c;
            continue;
        }
        char e = *p->at++;
        switch (e) {
        case '"': text[n++] = '"'; break;
        case '\\': text[n++] = '\\'; break;
        case '/': text[n++] = '/'; break;
        case 'b': text[n++] = '\b'; break;
        case 'f': text[n++] = '\f'; break;
        case 'n': text[n++] = '\n'; break;
        case 'r': text[n++] = '\r'; break;
        case 't': text[n++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (!read_hex4(p, &cp)) goto fail;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                unsigned low;
                if (p->at[0] != '\\' || p->at[1] != 'u') goto fail;
                p->at += 2;
                if (!read_hex4(p, &low) || low < 0xDC00u || low > 0xDFFFu) goto fail;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                goto fail;
            }
            if (cp == 0) goto fail;
            n += utf8_encode(cp, text + n);
            break;
        }
        default:
            goto fail;
        }
    }
    p->at++;
    text[n] = '\0';
    *out = text;
    return true;
fail:
    CA_FREE(text);
    return false;
}

static bool parse_number(JsonParser *p, double *out)
{
    const char *s = p->at;
    double sign = 1.0, value = 0.0;
    if (*s == '-') { sign = -1.0; s++; }
    if (*s < '0' || *s > '9') return false;
    if (*s == '0') s++;
    else while (*s >= '0' && *s <= '9') value = value * 10.0 + (double)(*s++ - '0');
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') return false;
        double scale = 0.1;
        while (*s >= '0' && *s <= '9') {
            value += (double)(*s++ - '0') * scale;
            scale *= 0.1;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1, exponent = 0;
        if (*s == '+' || *s == '-') exp_sign = *s++ == '-' ? -1 : 1;
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') {
            if (exponent < 400) exponent = exponent * 10 + (*s - '0');
            s++;
        }
        for (int i = 0; i < exponent; ++i) value = exp_sign > 0 ? value * 10.0 : value / 10.0;
    }
    p->at = s;
    *out = sign * value;
    return true;
}

static bool parse_value(JsonParser *p, JsonValue *out);

static bool push_item(JsonValue *container, JsonValue *item, char *key)
{
    JsonValue *items = CA_REALLOC(container->items, (container->count + 1u) * sizeof(JsonValue));
    if (!items) return false;
    container->items = items;
    if (container->type == JSON_OBJECT) {
        char **keys = CA_REALLOC(container->keys, (container->count + 1u) * sizeof(char *));
        if (!keys) return false;
        container->keys = keys;
        container->keys[container->count] = key;
    }
    container->items[container->count++] = *item;
    return true;
}

static bool parse_container(JsonParser *p, JsonValue *out, bool object)
{
    const char close = object ? '}' : ']';
    *out = (JsonValue){ .type = object ? JSON_OBJECT : JSON_ARRAY };
    p->at++;
    skip_ws(p);
    if (*p->at == close) {
        p->at++;
        return true;
    }
    for (;;) {
        char *key = NULL;
        if (object) {
            skip_ws(p);
            if (!parse_string(p, &key)) goto fail;
            skip_ws(p);
            if (*p->at != ':') {
                CA_FREE(key);
                goto fail;
            }
            p->at++;
        }
        JsonValue item;
        if (!parse_value(p, &item)) {
            CA_FREE(key);
            goto fail;
        }
        if (!push_item(out, &item, key)) {
            json_free(&item);
            CA_FREE(key);
            goto fail;
        }
        skip_ws(p);
        if (*p->at == ',') {
            p->at++;
            continue;
        }
        if (*p->at == close) {
            p->at++;
            return true;
        }
        goto fail;
    }
fail:
    json_free(out);
    return false;
}

static bool parse_literal(JsonParser *p, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(p->at, word, n) != 0) return false;
    p->at += n;
    return true;
}

static bool parse_value(JsonParser *p, JsonValue *out)
{
    *out = (JsonValue){0};
    skip_ws(p);
    if (++p->depth > LOAD_DEPTH_MAX) return false;
    bool ok;
    switch (*p->at) {
    case '{': ok = parse_container(p, out, true); break;
    case '[': ok = parse_container(p, out, false); break;
    case '"': out->type = JSON_STRING; ok = parse_string(p, &out->string); break;
    case 't': out->type = JSON_BOOL; out->boolean = true; ok = parse_literal(p, "true"); break;
    case 'f': out->type = JSON_BOOL; ok = parse_literal(p, "false"); break;
    case 'n': out->type = JSON_NULL; ok = parse_literal(p, "null"); break;
    default:  out->type = JSON_NUMBER; ok = parse_number(p, &out->number); break;
    }
    p->depth--;
    if (!ok) json_free(out);
    return ok;
}

static const JsonValue *json_get(const JsonValue *object, const char *key)
{
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < object->count; ++i)
        if (strcmp(object->keys[i], key) == 0) return &object->items[i];
    return NULL;
}

static int json_int(const JsonValue *value, int fallback)
{
    if (!value || value->type != JSON_NUMBER) return fallback;
    if (value->number >= 2147483647.0) return INT32_MAX;
    if (value->number <= -2147483648.0) return INT32_MIN;
    return (int)value->number;
}

/* ============================================================
   LOAD
   ============================================================ */

#define LOAD_FAILED (-2)

/* Builds a subtree; returns its node, CA_DOCK_NONE when it holds nothing to
   keep, or LOAD_FAILED on malformed input or allocation failure. Central
   flags are honoured only where allow_central is set (the main window). */
static int load_node(Ca_DockLayout *layout, const JsonValue *json, int depth, bool *panel_used,
                     bool allow_central)
{
    if (!json || json->type != JSON_OBJECT || depth > LOAD_DEPTH_MAX) return LOAD_FAILED;
    const JsonValue *tabs = json_get(json, "tabs");
    if (tabs) {
        const JsonValue *closed = json_get(json, "closed");
        const JsonValue *central = json_get(json, "central");
        if (tabs->type != JSON_ARRAY || (closed && closed->type != JSON_ARRAY) ||
            (central && central->type != JSON_BOOL))
            return LOAD_FAILED;
        const int group = claim_node(layout, CA_DOCK_NODE_GROUP);
        if (group == CA_DOCK_NONE || !reserve_tabs(layout, group, tabs->count)) return LOAD_FAILED;
        const int wanted_active = json_int(json_get(json, "active"), 0);
        int active_slot = 0;
        for (size_t i = 0; i < tabs->count; ++i) {
            const JsonValue *tab = &tabs->items[i];
            const int panel = tab->type == JSON_STRING ? ca_dock_layout_find(layout, tab->string) : CA_DOCK_NONE;
            if (panel == CA_DOCK_NONE || panel_used[panel]) continue;
            if ((int)i == wanted_active) active_slot = ca_dock_group_tab_count(layout, group);
            panel_used[panel] = true;
            insert_tab(layout, group, panel, -1);
        }
        bool remembered = false;
        for (size_t i = 0; closed && i < closed->count; ++i) {
            const JsonValue *entry = &closed->items[i];
            const int panel = entry->type == JSON_STRING ? ca_dock_layout_find(layout, entry->string) : CA_DOCK_NONE;
            if (panel == CA_DOCK_NONE || panel_used[panel]) continue;
            panel_used[panel] = true;
            panel_at(layout, panel)->home = group;
            remembered = true;
        }
        Ca_DockNodeEntry *node = ca_dock_node(layout, group);
        node->central = allow_central && central && central->boolean;
        if (node->tabs.count == 0 && !node->central && !remembered) {
            release_node(layout, group);
            return CA_DOCK_NONE;
        }
        node->active = node->tabs.count ? active_slot : 0;
        return group;
    }
    const JsonValue *kind = json_get(json, "split");
    const JsonValue *ratio = json_get(json, "ratio");
    const JsonValue *size = json_get(json, "size");
    if (!kind || kind->type != JSON_STRING || !ratio || ratio->type != JSON_NUMBER ||
        (size && size->type != JSON_NUMBER) ||
        (strcmp(kind->string, "h") != 0 && strcmp(kind->string, "v") != 0))
        return LOAD_FAILED;
    const int first = load_node(layout, json_get(json, "first"), depth + 1, panel_used, allow_central);
    if (first == LOAD_FAILED) return LOAD_FAILED;
    const int second = load_node(layout, json_get(json, "second"), depth + 1, panel_used, allow_central);
    if (second == LOAD_FAILED) return LOAD_FAILED;
    if (first == CA_DOCK_NONE) return second;
    if (second == CA_DOCK_NONE) return first;
    const int split = claim_node(layout, CA_DOCK_NODE_SPLIT);
    if (split == CA_DOCK_NONE) return LOAD_FAILED;
    Ca_DockNodeEntry *node = ca_dock_node(layout, split);
    node->horizontal = strcmp(kind->string, "h") == 0;
    node->ratio = clamp_ratio((float)ratio->number);
    node->size = size && size->number > 0.0 && size->number < 1.0e7 ? (float)size->number : 0.0f;
    node->first = first;
    node->second = second;
    ca_dock_node(layout, first)->parent = split;
    ca_dock_node(layout, second)->parent = split;
    return split;
}

bool ca_dock_layout_load(Ca_DockLayout *layout, const char *text)
{
    if (!layout || !text) return false;
    JsonParser parser = { .at = text };
    JsonValue document;
    if (!parse_value(&parser, &document)) return false;
    skip_ws(&parser);
    const JsonValue *windows = json_get(&document, "windows");
    if (*parser.at != '\0' || !windows || windows->type != JSON_ARRAY || windows->count == 0) {
        json_free(&document);
        return false;
    }

    Ca_DockLayout parsed;
    if (!layout_clone(&parsed, layout)) {
        json_free(&document);
        return false;
    }
    ca_dock_layout_clear(&parsed);
    bool *panel_used = parsed.panels.count ? CA_CALLOC(parsed.panels.count, sizeof(bool)) : NULL;
    bool ok = !parsed.panels.count || panel_used;

    int slot = 0;
    for (size_t i = 0; ok && i < windows->count; ++i) {
        const JsonValue *entry = &windows->items[i];
        const JsonValue *root = json_get(entry, "root");
        if (entry->type != JSON_OBJECT || !root) {
            ok = false;
            break;
        }
        const int node = root->type == JSON_NULL
            ? CA_DOCK_NONE : load_node(&parsed, root, 0, panel_used, slot == CA_DOCK_MAIN_WINDOW);
        if (node == LOAD_FAILED) {
            ok = false;
            break;
        }
        if (node == CA_DOCK_NONE && slot != CA_DOCK_MAIN_WINDOW) continue;
        if (slot != CA_DOCK_MAIN_WINDOW && claim_window(&parsed) != slot) {
            ok = false;
            break;
        }
        Ca_DockWindowEntry *window = ca_dock_window_entry(&parsed, slot);
        *window = (Ca_DockWindowEntry){
            .used = true, .root = node,
            .x = json_int(json_get(entry, "x"), 0), .y = json_int(json_get(entry, "y"), 0),
            .width = json_int(json_get(entry, "w"), 0), .height = json_int(json_get(entry, "h"), 0),
        };
        slot++;
    }
    CA_FREE(panel_used);
    json_free(&document);

    if (!ok || slot == 0 || !ca_dock_layout_valid(&parsed)) {
        layout_release(&parsed);
        return false;
    }
    layout_take(layout, &parsed);
    return true;
}
