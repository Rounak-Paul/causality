// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_dock.c — live dock workspace: emission, drag and drop, floating windows.
 *
 * The layout (ca_dock_layout.c) is the single source of truth. Structural
 * changes tear mounted panels down and bump an epoch signal that every window
 * host's builder reads, so the next tick re-emits the trees. Group content
 * divs carry their own builders, so switching tabs rebuilds one group only.
 *
 * Callbacks receive stable heap bindings ({dock, node|window}) rather than
 * pointers into view arrays, which relocate as the layout grows.
 */
#include "ca_dock_internal.h"
#include "ca_reactive.h"
#include "causality_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DRAG_THRESHOLD_PX     5.0f
#define STRIP_BAND_PX         14.0f
#define GHOST_OPACITY         0.62f
#define DEFAULT_FLOAT_WIDTH   720
#define DEFAULT_FLOAT_HEIGHT  480
#define MIN_FLOAT_WIDTH_PX    240.0f
#define MIN_FLOAT_HEIGHT_PX   160.0f
#define TARGET_SIZE_PX        34.0f
#define TARGET_GAP_PX         6.0f
#define TARGET_OUTER_MARGIN   10.0f
#define TARGET_GLYPH_INSET_PX 6.0f
#define CROSS_MIN_PX          170.0f
#define MARKER_WIDTH_PX       3.0f
#define SPLIT_MIN_RATIO       0.08f
#define SPLIT_MAX_RATIO       0.92f
#define DRAG_FRAME_SECONDS    (1.0 / 120.0)
#define STYLE_MAX             256
#define KEY_MAX               (CA_DOCK_ID_MAX + 16)

typedef enum DockTarget {
    TARGET_CENTER,
    TARGET_LEFT,
    TARGET_RIGHT,
    TARGET_TOP,
    TARGET_BOTTOM,
    TARGET_OUTER_LEFT,
    TARGET_OUTER_RIGHT,
    TARGET_OUTER_TOP,
    TARGET_OUTER_BOTTOM,
    TARGET_COUNT,
} DockTarget;

/* Stable callback context for one node or window index. */
typedef struct DockBinding {
    Ca_Dock *dock;
    int      index;
} DockBinding;

typedef struct TargetView {
    Ca_Div *box;
    Ca_Div *glyph;
} TargetView;

typedef struct WindowView {
    Ca_Window *window;
    Ca_Div    *host;
    Ca_Div    *overlay;
    Ca_Div    *preview;
    TargetView targets[TARGET_COUNT];
    bool       close_requested;
    bool       closing;
    uint64_t   focus_order;
} WindowView;

typedef struct GroupView {
    Ca_Div    *container;
    Ca_Div    *content;
    Ca_TabBar *tabs;
} GroupView;

typedef struct SplitView {
    Ca_Splitter *splitter;
    bool         locked;     /* pinned while an ancestor divider is dragged */
} SplitView;

typedef struct PanelSlot {
    Ca_DockPanelDesc desc;
    bool             mounted;
} PanelSlot;

/* What a drop at the cursor would do, and what to draw for it. */
typedef struct DockHover {
    int   window;          /* window under the cursor, or CA_DOCK_NONE        */
    int   node;            /* group under the cursor, or CA_DOCK_NONE         */
    bool  empty_layout;    /* window has no groups: one centre target         */
    bool  show_cross;      /* draw the inner targets around (cx, cy)          */
    bool  show_outer;      /* draw the window-edge targets                    */
    float cx, cy;          /* cross centre in window coordinates              */
    int   target;          /* hovered DockTarget, or -1                       */
    int   tab_index;       /* tab-strip insertion index, or -1                */
    float ratio;           /* incoming pane's share of the hovered target     */
    float incoming;        /* incoming pane size along the split axis         */
    bool  center_ok;       /* centre (tab) drop allowed on the hovered group  */
    bool  sides_ok;        /* inner side targets allowed on the hovered group */
    bool  preview;
    float px, py, pw, ph;  /* preview rectangle in window coordinates         */
} DockHover;

typedef enum DragPhase {
    DRAG_NONE,
    DRAG_PRESSED,          /* button down on a tab, below the threshold       */
    DRAG_REORDER,          /* moving along the source strip                   */
    DRAG_FLOATING,         /* panel is a live window following the cursor     */
} DragPhase;

typedef struct DragSession {
    DragPhase phase;
    int       panel;
    int       source_window;
    float     press_x, press_y;
    float     grab_x, grab_y;
    int       ghost_window;
    bool      whole_window;
    bool      external_move;   /* the OS title bar moves the window, not the dock */
    DockHover hover;
} DragSession;

struct Ca_Dock {
    Ca_Instance   *instance;
    Ca_Window     *main_window;
    Ca_DockDesc    desc;
    Ca_DockLayout *layout;
    Ca_DockLayout *drag_origin;
    Ca_DynArray    panels;           /* PanelSlot, index = layout panel  */
    Ca_DynArray    windows;          /* WindowView, index = layout window */
    Ca_DynArray    groups;           /* GroupView, index = layout node    */
    Ca_DynArray    splits;           /* SplitView, index = layout node    */
    Ca_DynArray    node_bindings;    /* DockBinding *                     */
    Ca_DynArray    window_bindings;  /* DockBinding *                     */
    Ca_DynArray    labels;           /* const char * scratch for tab bars */
    Ca_Div        *root;
    Ca_Signal     *epoch;
    Ca_Effect     *pump;
    DragSession    drag;
    uint64_t       focus_order;
    int            focused_window;
    int            resizing_split;   /* split whose divider is being dragged */
    bool           spawn_unfocused;
    bool           structure_pending;
    bool           layout_dirty;
    bool           mounted;
    bool           destroying;
};

static void apply_structure(Ca_Dock *dock);
static void open_missing_windows(Ca_Dock *dock);

/* ============================================================
   VIEW STORAGE
   ============================================================ */

static PanelSlot *panel_slot(Ca_Dock *dock, int panel)
{
    return panel < 0 ? NULL : ca_dyn_array_at(&dock->panels, (size_t)panel);
}

static WindowView *window_view(Ca_Dock *dock, int window)
{
    return window < 0 ? NULL : ca_dyn_array_at(&dock->windows, (size_t)window);
}

static GroupView *group_view(Ca_Dock *dock, int node)
{
    return node < 0 ? NULL : ca_dyn_array_at(&dock->groups, (size_t)node);
}

static SplitView *split_view(Ca_Dock *dock, int node)
{
    return node < 0 ? NULL : ca_dyn_array_at(&dock->splits, (size_t)node);
}

/* Grows a binding table to cover `count` indices; existing bindings never move. */
static bool grow_bindings(Ca_Dock *dock, Ca_DynArray *table, size_t count)
{
    if (table->count >= count) return true;
    if (!ca_dyn_array_reserve(table, count)) return false;
    while (table->count < count) {
        DockBinding *binding = CA_MALLOC(sizeof(*binding));
        if (!binding) return false;
        *binding = (DockBinding){ .dock = dock, .index = (int)table->count };
        ca_dyn_array_push(table, &binding);
    }
    return true;
}

/* Sizes every view table to the layout. Returns false on allocation failure. */
static bool sync_views(Ca_Dock *dock)
{
    const size_t nodes = (size_t)ca_dock_node_count(dock->layout);
    const size_t windows = (size_t)ca_dock_layout_window_count(dock->layout);
    bool ok = true;
    if (dock->groups.count < nodes) ok = ca_dyn_array_resize(&dock->groups, nodes) && ok;
    if (dock->splits.count < nodes) ok = ca_dyn_array_resize(&dock->splits, nodes) && ok;
    if (dock->windows.count < windows) ok = ca_dyn_array_resize(&dock->windows, windows) && ok;
    ok = grow_bindings(dock, &dock->node_bindings, nodes) && ok;
    ok = grow_bindings(dock, &dock->window_bindings, windows) && ok;
    return ok;
}

static DockBinding *node_binding(Ca_Dock *dock, int node)
{
    DockBinding **slot = node < 0 ? NULL : ca_dyn_array_at(&dock->node_bindings, (size_t)node);
    return slot ? *slot : NULL;
}

static DockBinding *window_binding(Ca_Dock *dock, int window)
{
    DockBinding **slot = window < 0 ? NULL : ca_dyn_array_at(&dock->window_bindings, (size_t)window);
    return slot ? *slot : NULL;
}

static int window_slot_of_native(Ca_Dock *dock, const Ca_Window *window)
{
    for (size_t w = 0; w < dock->windows.count; ++w)
        if (((WindowView *)ca_dyn_array_at(&dock->windows, w))->window == window) return (int)w;
    return CA_DOCK_NONE;
}

static bool window_live(Ca_Dock *dock, int slot)
{
    const WindowView *view = window_view(dock, slot);
    const Ca_DockWindowEntry *entry = ca_dock_window_entry_const(dock->layout, slot);
    return view && entry && view->window && entry->used && !view->closing;
}

/* ============================================================
   EVENTS
   ============================================================ */

static void emit_event(Ca_Dock *dock, Ca_DockEventKind kind, int panel)
{
    if (dock->destroying || !dock->desc.on_event) return;
    const Ca_DockEvent event = {
        .kind = kind,
        .panel_id = panel >= 0 ? ca_dock_layout_panel_id(dock->layout, panel) : NULL,
    };
    dock->desc.on_event(dock, &event, dock->desc.event_data);
}

static void mark_layout_dirty(Ca_Dock *dock)
{
    dock->layout_dirty = true;
}

static void mark_structure_changed(Ca_Dock *dock)
{
    dock->structure_pending = true;
    dock->layout_dirty = true;
}

/* ============================================================
   MOUNTING
   ============================================================ */

static void teardown_panel(Ca_Dock *dock, int panel)
{
    PanelSlot *slot = panel_slot(dock, panel);
    if (!slot || !slot->mounted) return;
    slot->mounted = false;
    if (slot->desc.teardown) slot->desc.teardown(slot->desc.user_data);
}

static void teardown_mounted(Ca_Dock *dock)
{
    for (size_t p = 0; p < dock->panels.count; ++p) teardown_panel(dock, (int)p);
}

/* Names every floating window after its first group's active panel. */
static void refresh_window_titles(Ca_Dock *dock)
{
    for (int w = CA_DOCK_MAIN_WINDOW + 1; w < (int)dock->windows.count; ++w) {
        const int root = ca_dock_layout_window_root(dock->layout, w);
        WindowView *view = window_view(dock, w);
        if (root == CA_DOCK_NONE || !view || !view->window) continue;
        const PanelSlot *slot = panel_slot(dock, ca_dock_layout_active_panel(
            dock->layout, ca_dock_first_visible_group(dock->layout, root)));
        if (slot) ca_window_set_title(view->window, slot->desc.title);
    }
}

static void bump_epoch(Ca_Dock *dock)
{
    ca_signal_set_u32(dock->epoch, ca_signal_get_u32(dock->epoch) + 1u);
}

/* ============================================================
   EMISSION
   ============================================================ */

static void format_group_style(Ca_Dock *dock, int group, char *out, size_t cap)
{
    const PanelSlot *slot = panel_slot(dock, ca_dock_layout_active_panel(dock->layout, group));
    const char *extra = slot ? slot->desc.group_style : NULL;
    const int n = extra && extra[0] ? snprintf(out, cap, "ca-dock-group %s", extra)
                                    : snprintf(out, cap, "ca-dock-group");
    if (n < 0 || (size_t)n >= cap) snprintf(out, cap, "ca-dock-group");
}

static void group_content_builder(Ca_Div *content, void *user_data)
{
    (void)content;
    const DockBinding *binding = user_data;
    Ca_Dock *dock = binding->dock;
    const int node = binding->index;
    const int panel = ca_dock_layout_active_panel(dock->layout, node);
    const int window = ca_dock_layout_window_of(dock->layout, node);
    PanelSlot *slot = panel_slot(dock, panel);
    const WindowView *view = window_view(dock, window);
    if (!slot || !view || !view->window) return;

    char key[KEY_MAX];
    snprintf(key, sizeof(key), "ca-dock-p-%s", ca_dock_layout_panel_id(dock->layout, panel));
    ca_div_begin(&(Ca_DivDesc){ .direction = CA_VERTICAL, .style = "ca-dock-panel", .id = key });
    slot->desc.build(dock, view->window, slot->desc.user_data);
    ca_div_end();
    slot->mounted = true;
}

static void on_tab_change(Ca_TabBar *tabs, void *user_data)
{
    const DockBinding *binding = user_data;
    Ca_Dock *dock = binding->dock;
    const int node = binding->index;
    const int index = ca_tabs_active(tabs);
    const int panel = ca_dock_group_tab(dock->layout, node, index);
    const int previous = ca_dock_layout_active_panel(dock->layout, node);
    if (panel == CA_DOCK_NONE || panel == previous) return;
    teardown_panel(dock, previous);
    ca_dock_layout_set_active(dock->layout, node, index);
    GroupView *view = group_view(dock, node);
    if (view && view->container) {
        char style[STYLE_MAX];
        format_group_style(dock, node, style, sizeof(style));
        ca_set_style(view->container, style);
    }
    if (view && view->content) ca_div_invalidate(view->content);
    refresh_window_titles(dock);
    mark_layout_dirty(dock);
    emit_event(dock, CA_DOCK_EVENT_PANEL_ACTIVATED, panel);
}

/* Closes a panel, remembering its place so reopening restores it. */
static void close_panel_index(Ca_Dock *dock, int panel)
{
    if (ca_dock_layout_group_of(dock->layout, panel) == CA_DOCK_NONE) return;
    teardown_panel(dock, panel);
    ca_dock_layout_close(dock->layout, panel);
    mark_structure_changed(dock);
    emit_event(dock, CA_DOCK_EVENT_PANEL_CLOSED, panel);
}

static void on_tab_close(Ca_TabBar *tabs, int index, void *user_data)
{
    (void)tabs;
    const DockBinding *binding = user_data;
    close_panel_index(binding->dock, ca_dock_group_tab(binding->dock->layout, binding->index, index));
}

/* Pane of a split that keeps its size: the side without the central group. */
static int fixed_pane_of(const Ca_Dock *dock, int split)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(dock->layout, split);
    if (!node || node->kind != CA_DOCK_NODE_SPLIT) return CA_SPLIT_FIXED_NONE;
    if (ca_dock_contains_central(dock->layout, node->first)) return CA_SPLIT_FIXED_SECOND;
    if (ca_dock_contains_central(dock->layout, node->second)) return CA_SPLIT_FIXED_FIRST;
    return CA_SPLIT_FIXED_NONE;
}

/* Applies a split's layout state (ratio, fixed pane and size) to its splitter. */
static void apply_split_state(Ca_Dock *dock, int split, Ca_Splitter *splitter)
{
    const Ca_DockNodeEntry *node = ca_dock_node_const(dock->layout, split);
    const int fixed = fixed_pane_of(dock, split);
    ca_split_set_ratio(splitter, node->ratio);
    ca_split_set_fixed(splitter, fixed, fixed != CA_SPLIT_FIXED_NONE ? node->size : 0.0f);
}

/* Reads a split's laid-out pane sizes back into the layout. */
static void capture_split_state(Ca_Dock *dock, int split, const Ca_Splitter *splitter)
{
    Ca_DockNodeEntry *node = ca_dock_node(dock->layout, split);
    const float first = ca_split_get_pane_size(splitter, 0);
    const float second = ca_split_get_pane_size(splitter, 1);
    if (!node || first + second <= 0.0f) return;
    node->ratio = first / (first + second);
    if (node->ratio < SPLIT_MIN_RATIO) node->ratio = SPLIT_MIN_RATIO;
    if (node->ratio > SPLIT_MAX_RATIO) node->ratio = SPLIT_MAX_RATIO;
    const int fixed = fixed_pane_of(dock, split);
    if (fixed != CA_SPLIT_FIXED_NONE) node->size = fixed == CA_SPLIT_FIXED_FIRST ? first : second;
}

/*
 * Pins panes that do not touch a dragged divider (ImGui WantLockSizeOnce).
 * touch_end: the divider touches this subtree's end edge (it is the first
 * child of the dragged split) rather than its start edge.
 */
static void lock_untouched(Ca_Dock *dock, int node, bool horizontal, bool touch_end, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(dock->layout, node);
    if (!entry || entry->kind != CA_DOCK_NODE_SPLIT || depth > ca_dock_node_count(dock->layout)) return;
    const int first = entry->first, second = entry->second;
    const bool first_visible = ca_dock_layout_node_visible(dock->layout, first);
    const bool second_visible = ca_dock_layout_node_visible(dock->layout, second);
    if (!first_visible || !second_visible) {
        if (first_visible || second_visible)
            lock_untouched(dock, first_visible ? first : second, horizontal, touch_end, depth + 1);
        return;
    }
    if (entry->horizontal != horizontal) {
        lock_untouched(dock, first, horizontal, touch_end, depth + 1);
        lock_untouched(dock, second, horizontal, touch_end, depth + 1);
        return;
    }
    SplitView *view = split_view(dock, node);
    if (view && view->splitter) {
        const int far_pane = touch_end ? 0 : 1;
        const float size = ca_split_get_pane_size(view->splitter, far_pane);
        if (size > 0.0f) {
            ca_split_set_fixed(view->splitter, far_pane == 0 ? CA_SPLIT_FIXED_FIRST : CA_SPLIT_FIXED_SECOND, size);
            view->locked = true;
        }
    }
    lock_untouched(dock, touch_end ? second : first, horizontal, touch_end, depth + 1);
}

/* Ends a divider drag: write the settled sizes back and restore splitter policies. */
static void end_split_resize(Ca_Dock *dock)
{
    for (int s = 0; s < (int)dock->splits.count; ++s) {
        SplitView *view = split_view(dock, s);
        if (!view->splitter || (!view->locked && s != dock->resizing_split)) continue;
        capture_split_state(dock, s, view->splitter);
        apply_split_state(dock, s, view->splitter);
        view->locked = false;
    }
    dock->resizing_split = CA_DOCK_NONE;
    mark_layout_dirty(dock);
}

static void on_split_resize(float ratio, void *user_data)
{
    const DockBinding *binding = user_data;
    Ca_Dock *dock = binding->dock;
    const Ca_DockNodeEntry *entry = ca_dock_node_const(dock->layout, binding->index);
    if (!entry || entry->kind != CA_DOCK_NODE_SPLIT) return;
    if (dock->resizing_split != binding->index) {
        if (dock->resizing_split != CA_DOCK_NONE) end_split_resize(dock);
        dock->resizing_split = binding->index;
        const bool horizontal = entry->horizontal;
        const int first = entry->first, second = entry->second;
        lock_untouched(dock, first, horizontal, true, 0);
        lock_untouched(dock, second, horizontal, false, 0);
    }
    ca_dock_node(dock->layout, binding->index)->ratio = ratio;
    mark_layout_dirty(dock);
}

static void on_tab_drag_start(Ca_TabBar *tabs, int index, const Ca_DragEvent *event, void *user_data);

static void emit_node(Ca_Dock *dock, int node, int depth);

static void emit_group(Ca_Dock *dock, int node)
{
    GroupView *view = group_view(dock, node);
    DockBinding *binding = node_binding(dock, node);
    if (!view || !binding) return;
    const int count = ca_dock_group_tab_count(dock->layout, node);
    const Ca_DockNodeEntry *entry = ca_dock_node_const(dock->layout, node);

    char group_key[KEY_MAX], tabs_key[KEY_MAX], content_key[KEY_MAX], style[STYLE_MAX];
    snprintf(group_key, sizeof(group_key), "ca-dock-g%d", node);
    snprintf(tabs_key, sizeof(tabs_key), "ca-dock-t%d", node);
    snprintf(content_key, sizeof(content_key), "ca-dock-c%d", node);
    format_group_style(dock, node, style, sizeof(style));

    if (count == 0) {
        /* Only an empty central group is ever emitted: it keeps its area as a drop target. */
        view->container = ca_div_begin(&(Ca_DivDesc){
            .direction = CA_VERTICAL, .style = "ca-dock-group ca-dock-empty", .id = group_key,
        });
        ca_div_end();
        view->tabs = NULL;
        view->content = NULL;
        return;
    }
    const int window = ca_dock_layout_window_of(dock->layout, node);
    const int root = ca_dock_layout_window_root(dock->layout, window);
    const bool standalone = window != CA_DOCK_MAIN_WINDOW && count == 1 &&
                            ca_dock_visible_group_count(dock->layout, root) == 1;
    view->container = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL, .style = style, .id = group_key,
    });
    view->tabs = NULL;
    if (!standalone && ca_dyn_array_resize(&dock->labels, (size_t)count)) {
        const char **labels = dock->labels.data;
        for (int i = 0; i < count; ++i) {
            const PanelSlot *slot = panel_slot(dock, ca_dock_group_tab(dock->layout, node, i));
            labels[i] = slot ? slot->desc.title : "";
        }
        view->tabs = ca_tabs(&(Ca_TabBarDesc){
            .labels = labels, .count = count, .active = entry->active,
            .style = "ca-dock-tabs", .tab_style = "ca-dock-tab", .id = tabs_key,
            .on_change = on_tab_change, .change_data = binding,
            .closable = true, .on_close = on_tab_close, .close_data = binding,
            .on_drag_start = on_tab_drag_start, .drag_data = binding,
        });
    }
    view->content = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL, .style = "ca-dock-content", .id = content_key,
    });
    ca_div_end();
    ca_div_set_builder(view->content, group_content_builder, binding);
    ca_div_end();
}

static void emit_node(Ca_Dock *dock, int node, int depth)
{
    const Ca_DockNodeEntry *entry = ca_dock_node_const(dock->layout, node);
    if (!entry || depth > ca_dock_node_count(dock->layout)) return;
    if (entry->kind == CA_DOCK_NODE_GROUP) {
        emit_group(dock, node);
        return;
    }
    if (entry->kind != CA_DOCK_NODE_SPLIT) return;
    const int first = entry->first, second = entry->second;
    const bool first_visible = ca_dock_layout_node_visible(dock->layout, first);
    const bool second_visible = ca_dock_layout_node_visible(dock->layout, second);
    if (!first_visible || !second_visible) {
        /* A hidden side (closed panels' remembered groups) yields its space. */
        if (first_visible || second_visible) emit_node(dock, first_visible ? first : second, depth + 1);
        return;
    }
    char key[KEY_MAX];
    snprintf(key, sizeof(key), "ca-dock-s%d", node);
    const int fixed = fixed_pane_of(dock, node);
    Ca_Splitter *splitter = ca_split_begin(&(Ca_SplitDesc){
        .direction = entry->horizontal ? CA_HORIZONTAL : CA_VERTICAL,
        .ratio = entry->ratio, .min_ratio = SPLIT_MIN_RATIO, .max_ratio = SPLIT_MAX_RATIO,
        .bar_size = 1.0f, .on_resize = on_split_resize, .user_data = node_binding(dock, node),
        .fixed_pane = fixed, .fixed_size = entry->size,
        .id = key,
    });
    if (!splitter) return;
    SplitView *view = split_view(dock, node);
    if (view) *view = (SplitView){ .splitter = splitter };
    apply_split_state(dock, node, splitter);
    emit_node(dock, first, depth + 1);
    emit_node(dock, second, depth + 1);
    ca_split_end();
}

static float ui_scale(const Ca_Dock *dock)
{
    const float scale = ca_instance_get_scale(dock->instance);
    return scale > 0.0f ? scale : 1.0f;
}

static void emit_overlay(Ca_Dock *dock, WindowView *view)
{
    const float size = TARGET_SIZE_PX * ui_scale(dock);
    const float inset = TARGET_GLYPH_INSET_PX * ui_scale(dock);
    view->overlay = ca_div_begin(&(Ca_DivDesc){
        .position = CA_POSITION_ABSOLUTE, .style = "ca-dock-overlay", .id = "ca-dock-overlay",
        .hidden = true, .no_hover = true, .z_index = 1000,
    });
    view->preview = ca_div_begin(&(Ca_DivDesc){
        .position = CA_POSITION_ABSOLUTE, .style = "ca-dock-preview", .id = "ca-dock-preview",
        .hidden = true, .no_hover = true,
    });
    ca_div_end();
    for (int t = 0; t < TARGET_COUNT; ++t) {
        char box_key[KEY_MAX], glyph_key[KEY_MAX];
        snprintf(box_key, sizeof(box_key), "ca-dock-tg%d", t);
        snprintf(glyph_key, sizeof(glyph_key), "ca-dock-gl%d", t);
        TargetView *target = &view->targets[t];
        target->box = ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE, .style = "ca-dock-target", .id = box_key,
            .hidden = true, .no_hover = true,
        });
        target->glyph = ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE, .style = "ca-dock-target-glyph", .id = glyph_key,
            .no_hover = true,
        });
        ca_div_end();
        ca_div_end();
        const float inner = size - 2.0f * inset;
        float gx = inset, gy = inset, gw = inner, gh = inner;
        switch (t) {
        case TARGET_LEFT:   case TARGET_OUTER_LEFT:   gw = inner * 0.5f; break;
        case TARGET_RIGHT:  case TARGET_OUTER_RIGHT:  gx = inset + inner * 0.5f; gw = inner * 0.5f; break;
        case TARGET_TOP:    case TARGET_OUTER_TOP:    gh = inner * 0.5f; break;
        case TARGET_BOTTOM: case TARGET_OUTER_BOTTOM: gy = inset + inner * 0.5f; gh = inner * 0.5f; break;
        default: break;
        }
        ca_div_set_absolute_rect(target->glyph, gx, gy, gw, gh);
    }
    ca_div_end();
}

static void host_builder(Ca_Div *container, void *user_data)
{
    (void)container;
    const DockBinding *binding = user_data;
    Ca_Dock *dock = binding->dock;
    const int window = binding->index;
    (void)ca_signal_get_u32(dock->epoch);
    if (dock->destroying || !sync_views(dock)) return;

    WindowView *view = window_view(dock, window);
    const Ca_DockWindowEntry *entry = ca_dock_window_entry_const(dock->layout, window);
    if (!view || !entry) return;
    if (window != CA_DOCK_MAIN_WINDOW && entry->root != CA_DOCK_NONE && view->window) {
        const int only = ca_dock_first_visible_group(dock->layout, entry->root);
        const bool single = ca_dock_visible_group_count(dock->layout, entry->root) == 1 &&
                            ca_dock_group_tab_count(dock->layout, only) == 1;
        ca_window_set_title_bar_visible(view->window, single);
    }
    view->host = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL, .style = "ca-dock-host", .id = "ca-dock-host",
    });
    if (entry->used && entry->root != CA_DOCK_NONE) emit_node(dock, entry->root, 0);
    emit_overlay(dock, view);
    ca_div_end();
}

/* ============================================================
   DRAG AND DROP
   ============================================================ */

static void window_local(Ca_Dock *dock, int slot, float sx, float sy, float *lx, float *ly, bool *inside)
{
    int x = 0, y = 0, width = 0, height = 0;
    ca_window_get_rect(window_view(dock, slot)->window, &x, &y, &width, &height);
    *lx = sx - (float)x;
    *ly = sy - (float)y;
    *inside = *lx >= 0.0f && *ly >= 0.0f && *lx < (float)width && *ly < (float)height;
}

static bool rect_contains(float x, float y, float w, float h, float px, float py)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* Top-left of a target box; host is the window's dock host rectangle. */
static void target_origin(const Ca_Dock *dock, int target, const DockHover *hover,
                          const float host[4], float *x, float *y)
{
    const float size = TARGET_SIZE_PX * ui_scale(dock);
    const float pitch = size + TARGET_GAP_PX * ui_scale(dock);
    const float margin = TARGET_OUTER_MARGIN * ui_scale(dock);
    switch (target) {
    case TARGET_CENTER:      *x = hover->cx - size * 0.5f;         *y = hover->cy - size * 0.5f; break;
    case TARGET_LEFT:        *x = hover->cx - size * 0.5f - pitch; *y = hover->cy - size * 0.5f; break;
    case TARGET_RIGHT:       *x = hover->cx - size * 0.5f + pitch; *y = hover->cy - size * 0.5f; break;
    case TARGET_TOP:         *x = hover->cx - size * 0.5f;         *y = hover->cy - size * 0.5f - pitch; break;
    case TARGET_BOTTOM:      *x = hover->cx - size * 0.5f;         *y = hover->cy - size * 0.5f + pitch; break;
    case TARGET_OUTER_LEFT:  *x = host[0] + margin;                *y = host[1] + host[3] * 0.5f - size * 0.5f; break;
    case TARGET_OUTER_RIGHT: *x = host[0] + host[2] - margin - size; *y = host[1] + host[3] * 0.5f - size * 0.5f; break;
    case TARGET_OUTER_TOP:   *x = host[0] + host[2] * 0.5f - size * 0.5f; *y = host[1] + margin; break;
    default:                 *x = host[0] + host[2] * 0.5f - size * 0.5f; *y = host[1] + host[3] - margin - size; break;
    }
}

static Ca_DockZone target_zone(int target)
{
    switch (target) {
    case TARGET_LEFT:   case TARGET_OUTER_LEFT:   return CA_DOCK_ZONE_LEFT;
    case TARGET_RIGHT:  case TARGET_OUTER_RIGHT:  return CA_DOCK_ZONE_RIGHT;
    case TARGET_TOP:    case TARGET_OUTER_TOP:    return CA_DOCK_ZONE_TOP;
    case TARGET_BOTTOM: case TARGET_OUTER_BOTTOM: return CA_DOCK_ZONE_BOTTOM;
    default:                                      return CA_DOCK_ZONE_TAB;
    }
}

static void preview_for_target(int target, const float rect[4], float ratio, DockHover *hover)
{
    hover->preview = true;
    hover->px = rect[0];
    hover->py = rect[1];
    hover->pw = rect[2];
    hover->ph = rect[3];
    switch (target_zone(target)) {
    case CA_DOCK_ZONE_LEFT:   hover->pw = rect[2] * ratio; break;
    case CA_DOCK_ZONE_RIGHT:  hover->px = rect[0] + rect[2] * (1.0f - ratio); hover->pw = rect[2] * ratio; break;
    case CA_DOCK_ZONE_TOP:    hover->ph = rect[3] * ratio; break;
    case CA_DOCK_ZONE_BOTTOM: hover->py = rect[1] + rect[3] * (1.0f - ratio); hover->ph = rect[3] * ratio; break;
    default: break;
    }
}

/* Tab strip rectangle of a group in window coordinates; false before layout or without a strip. */
static bool strip_rect(Ca_Dock *dock, int node, float *x, float *y, float *w, float *h)
{
    const GroupView *view = group_view(dock, node);
    float gx, gy, gw, gh, ty, th;
    if (!view || !view->container || !ca_tabs_tab_rect(view->tabs, 0, NULL, &ty, NULL, &th)) return false;
    ca_div_screen_rect(view->container, &gx, &gy, &gw, &gh);
    *x = gx;
    *y = gy;
    *w = gw;
    *h = ty + th - gy;
    return true;
}

/* Insertion index among a group's tabs for cursor x, plus the marker x. */
static int strip_insertion(Ca_Dock *dock, int node, float lx, float *marker_x)
{
    const GroupView *view = group_view(dock, node);
    const int count = ca_dock_group_tab_count(dock->layout, node);
    int index = count;
    float boundary = 0.0f;
    for (int i = 0; view && i < count; ++i) {
        float tx, tw;
        if (!ca_tabs_tab_rect(view->tabs, i, &tx, NULL, &tw, NULL)) continue;
        boundary = tx + tw;
        if (lx < tx + tw * 0.5f) {
            index = i;
            boundary = tx;
            break;
        }
    }
    *marker_x = boundary;
    return index;
}

/* ImGui DockNodeCalcSplitRects: the incoming pane takes the dragged window's
   size along the split axis when that fits in half the target, else half. */
static void incoming_share(Ca_Dock *dock, int target, const float rect[4], DockHover *hover)
{
    hover->ratio = 0.5f;
    hover->incoming = 0.0f;
    const Ca_DockZone zone = target_zone(target);
    if (zone == CA_DOCK_ZONE_TAB) return;
    const bool horizontal = zone == CA_DOCK_ZONE_LEFT || zone == CA_DOCK_ZONE_RIGHT;
    const float avail = horizontal ? rect[2] : rect[3];
    const WindowView *ghost = window_view(dock, dock->drag.ghost_window);
    int width = 0, height = 0;
    if (ghost && ghost->window) ca_window_get_rect(ghost->window, NULL, NULL, &width, &height);
    const float desired = horizontal ? (float)width : (float)height;
    if (avail > 0.0f && desired > 0.0f && desired <= avail * 0.5f) hover->ratio = desired / avail;
    hover->incoming = avail * hover->ratio;
}

/* Finds the topmost window under the cursor (never the ghost) and what a drop there would do. */
static void compute_hover(Ca_Dock *dock, float sx, float sy, DockHover *hover)
{
    const int panel = dock->drag.panel;
    const int ghost = dock->drag.ghost_window;
    *hover = (DockHover){ .window = CA_DOCK_NONE, .node = CA_DOCK_NONE, .target = -1, .tab_index = -1,
                          .ratio = 0.5f };

    Ca_Window *source = window_view(dock, dock->drag.source_window)->window;
    if (ca_window_key_down(source, CA_KEY_LEFT_SHIFT) || ca_window_key_down(source, CA_KEY_RIGHT_SHIFT))
        return;

    int under = CA_DOCK_NONE;
    for (int w = 0; w < (int)dock->windows.count; ++w) {
        if (w == ghost || !window_live(dock, w)) continue;
        float lx, ly;
        bool inside;
        window_local(dock, w, sx, sy, &lx, &ly, &inside);
        if (inside && (under == CA_DOCK_NONE ||
                       window_view(dock, w)->focus_order > window_view(dock, under)->focus_order))
            under = w;
    }
    if (under == CA_DOCK_NONE) return;

    WindowView *view = window_view(dock, under);
    float lx, ly;
    bool inside;
    window_local(dock, under, sx, sy, &lx, &ly, &inside);
    hover->window = under;
    if (!view->host) return;

    float host[4];
    ca_div_screen_rect(view->host, &host[0], &host[1], &host[2], &host[3]);
    const float size = TARGET_SIZE_PX * ui_scale(dock);
    if (!rect_contains(host[0], host[1], host[2], host[3], lx, ly)) return;

    const int root = ca_dock_layout_window_root(dock->layout, under);
    if (root == CA_DOCK_NONE || !ca_dock_layout_node_visible(dock->layout, root)) {
        hover->empty_layout = true;
        hover->cx = host[0] + host[2] * 0.5f;
        hover->cy = host[1] + host[3] * 0.5f;
        float x, y;
        target_origin(dock, TARGET_CENTER, hover, host, &x, &y);
        if (rect_contains(x, y, size, size, lx, ly)) {
            hover->target = TARGET_CENTER;
            preview_for_target(TARGET_CENTER, host, 1.0f, hover);
        }
        return;
    }

    /* A visibly split floating tree can only land on an empty group or beside one. */
    const bool payload_split = dock->drag.whole_window &&
        ca_dock_visible_group_count(dock->layout, ca_dock_layout_window_root(dock->layout, ghost)) > 1;

    hover->show_outer = true;
    for (int node = 0; node < ca_dock_node_count(dock->layout); ++node) {
        const GroupView *group = group_view(dock, node);
        if (ca_dock_layout_node_kind(dock->layout, node) != CA_DOCK_NODE_GROUP || !group ||
            !group->container || ca_dock_layout_window_of(dock->layout, node) != under)
            continue;
        float gx, gy, gw, gh;
        ca_div_screen_rect(group->container, &gx, &gy, &gw, &gh);
        if (gw <= 0.0f || gh <= 0.0f || !rect_contains(gx, gy, gw, gh, lx, ly)) continue;
        const int tabs = ca_dock_group_tab_count(dock->layout, node);
        const bool own_sole = tabs == 1 && ca_dock_group_tab(dock->layout, node, 0) == panel;
        if (!own_sole) {
            const Ca_DockNodeEntry *entry = ca_dock_node_const(dock->layout, node);
            const float cross_min = CROSS_MIN_PX * ui_scale(dock);
            hover->node = node;
            hover->center_ok = !(payload_split && tabs > 0);
            /* A central group that is the window's root only offers the outer edges. */
            hover->sides_ok = gw >= cross_min && gh >= cross_min &&
                              !(entry->central && entry->parent == CA_DOCK_NONE);
            float sxr, syr, swr, shr;
            float strip_bottom = gy;
            if (strip_rect(dock, node, &sxr, &syr, &swr, &shr)) strip_bottom = syr + shr;
            hover->cx = gx + gw * 0.5f;
            hover->cy = strip_bottom + (gy + gh - strip_bottom) * 0.5f;
            hover->show_cross = true;
            const float group_rect[4] = { gx, gy, gw, gh };
            for (int t = TARGET_CENTER; t <= TARGET_BOTTOM; ++t) {
                if (t == TARGET_CENTER ? !hover->center_ok : !hover->sides_ok) continue;
                float x, y;
                target_origin(dock, t, hover, host, &x, &y);
                if (!rect_contains(x, y, size, size, lx, ly)) continue;
                hover->target = t;
                incoming_share(dock, t, group_rect, hover);
                preview_for_target(t, group_rect, t == TARGET_CENTER ? 1.0f : hover->ratio, hover);
                break;
            }
            if (hover->target < 0 && hover->center_ok && tabs > 0 && ly < strip_bottom) {
                float marker;
                hover->tab_index = strip_insertion(dock, node, lx, &marker);
                hover->preview = true;
                hover->px = marker - MARKER_WIDTH_PX * 0.5f;
                hover->py = gy;
                hover->pw = MARKER_WIDTH_PX;
                hover->ph = strip_bottom - gy;
            }
        }
        break;
    }
    for (int t = TARGET_OUTER_LEFT; t <= TARGET_OUTER_BOTTOM; ++t) {
        float x, y;
        target_origin(dock, t, hover, host, &x, &y);
        if (!rect_contains(x, y, size, size, lx, ly)) continue;
        hover->target = t;
        hover->tab_index = -1;
        incoming_share(dock, t, host, hover);
        preview_for_target(t, host, hover->ratio, hover);
        break;
    }
}

static void hide_overlays(Ca_Dock *dock)
{
    for (size_t w = 0; w < dock->windows.count; ++w) {
        WindowView *view = ca_dyn_array_at(&dock->windows, w);
        if (view->overlay) ca_set_hidden(view->overlay, true);
    }
}

/* Draws the hover result into its window's overlay and hides every other overlay. */
static void show_overlay(Ca_Dock *dock, const DockHover *hover)
{
    hide_overlays(dock);
    WindowView *view = window_view(dock, hover->window);
    if (!view || !view->overlay || !view->host) return;
    float host[4];
    ca_div_screen_rect(view->host, &host[0], &host[1], &host[2], &host[3]);
    ca_div_set_absolute_rect(view->overlay, 0.0f, 0.0f, host[2], host[3]);
    ca_set_hidden(view->overlay, false);

    const float size = TARGET_SIZE_PX * ui_scale(dock);
    for (int t = 0; t < TARGET_COUNT; ++t) {
        const TargetView *target = &view->targets[t];
        bool visible;
        if (t == TARGET_CENTER) visible = (hover->show_cross && hover->center_ok) || hover->empty_layout;
        else if (t <= TARGET_BOTTOM) visible = hover->show_cross && hover->sides_ok;
        else visible = hover->show_outer;
        ca_set_hidden(target->box, !visible);
        if (!visible) continue;
        float x, y;
        target_origin(dock, t, hover, host, &x, &y);
        ca_div_set_absolute_rect(target->box, x - host[0], y - host[1], size, size);
        const bool hot = hover->target == t;
        ca_set_style(target->box, hot ? "ca-dock-target hot" : "ca-dock-target");
        ca_set_style(target->glyph, hot ? "ca-dock-target-glyph hot" : "ca-dock-target-glyph");
    }
    ca_set_hidden(view->preview, !hover->preview);
    if (hover->preview)
        ca_div_set_absolute_rect(view->preview, hover->px - host[0], hover->py - host[1],
                                 hover->pw, hover->ph);
}

/* After a drop created a split beside the central path, pin the incoming pane's size. */
static void pin_incoming_size(Ca_Dock *dock, int incoming, float size)
{
    const Ca_DockNodeEntry *child = ca_dock_node_const(dock->layout, incoming);
    if (!child || size <= 0.0f) return;
    const int split = child->parent;
    const int fixed = fixed_pane_of(dock, split);
    const Ca_DockNodeEntry *parent = ca_dock_node_const(dock->layout, split);
    if (!parent || fixed == CA_SPLIT_FIXED_NONE) return;
    if ((fixed == CA_SPLIT_FIXED_FIRST) == (parent->first == incoming))
        ca_dock_layout_set_split_size(dock->layout, split, size);
}

/* Applies the drop a hover result describes. Returns true when the layout changed. */
static bool apply_hover_drop(Ca_Dock *dock, const DockHover *hover, int panel)
{
    if (hover->window == CA_DOCK_NONE) return false;
    const Ca_DockZone zone = hover->target >= 0 ? target_zone(hover->target) : CA_DOCK_ZONE_TAB;
    if (dock->drag.whole_window) {
        const bool outer = hover->target >= TARGET_OUTER_LEFT || hover->empty_layout;
        if (hover->target < 0 && hover->tab_index < 0) return false;
        const int payload = ca_dock_layout_window_root(dock->layout, dock->drag.ghost_window);
        const int target = outer ? ca_dock_layout_window_root(dock->layout, hover->window) : hover->node;
        const bool docked = ca_dock_layout_dock_window(dock->layout, dock->drag.ghost_window, hover->window,
                                                       target, zone, hover->tab_index, hover->ratio);
        if (docked && zone != CA_DOCK_ZONE_TAB) pin_incoming_size(dock, payload, hover->incoming);
        return docked;
    }
    bool docked = false;
    if (hover->target >= TARGET_OUTER_LEFT || hover->empty_layout) {
        docked = hover->target >= 0 &&
                 ca_dock_layout_dock_root(dock->layout, hover->window, panel,
                                          hover->empty_layout ? CA_DOCK_ZONE_TAB : zone, hover->ratio);
    } else if (hover->node == CA_DOCK_NONE) {
        return false;
    } else if (hover->target >= 0) {
        docked = zone == CA_DOCK_ZONE_TAB
            ? ca_dock_layout_add_tab(dock->layout, hover->node, panel, -1)
            : ca_dock_layout_split(dock->layout, hover->node, panel, zone, hover->ratio);
    } else if (hover->tab_index >= 0) {
        docked = ca_dock_layout_add_tab(dock->layout, hover->node, panel, hover->tab_index);
    }
    if (docked && zone != CA_DOCK_ZONE_TAB)
        pin_incoming_size(dock, ca_dock_layout_group_of(dock->layout, panel), hover->incoming);
    return docked;
}

static void drag_reset(Ca_Dock *dock)
{
    hide_overlays(dock);
    dock->drag = (DragSession){
        .phase = DRAG_NONE, .panel = CA_DOCK_NONE,
        .ghost_window = CA_DOCK_NONE, .source_window = CA_DOCK_NONE,
    };
}

static void ghost_release(Ca_Dock *dock)
{
    const WindowView *ghost = window_view(dock, dock->drag.ghost_window);
    if (ghost && ghost->window) ca_window_set_opacity(ghost->window, 1.0f);
}

/* Snapshots the layout so a cancelled drag can restore it. */
static bool remember_origin(Ca_Dock *dock)
{
    return ca_dock_layout_copy(dock->drag_origin, dock->layout);
}

static void on_tab_drag_start(Ca_TabBar *tabs, int index, const Ca_DragEvent *event, void *user_data)
{
    (void)tabs;
    const DockBinding *binding = user_data;
    Ca_Dock *dock = binding->dock;
    const int node = binding->index;
    if (dock->drag.phase != DRAG_NONE) return;
    const int count = ca_dock_group_tab_count(dock->layout, node);
    const int source = window_slot_of_native(dock, event->window);
    if (count == 0 || index < -1 || index >= count || source == CA_DOCK_NONE ||
        (index == -1 && source == CA_DOCK_MAIN_WINDOW) || !remember_origin(dock))
        return;
    int wx = 0, wy = 0;
    ca_window_get_rect(event->window, &wx, &wy, NULL, NULL);
    drag_reset(dock);
    dock->drag.phase = DRAG_PRESSED;
    dock->drag.panel = index >= 0 ? ca_dock_group_tab(dock->layout, node, index)
                                  : ca_dock_layout_active_panel(dock->layout, node);
    dock->drag.source_window = source;
    dock->drag.press_x = (float)wx + event->start_x;
    dock->drag.press_y = (float)wy + event->start_y;
    if (index == -1) {
        dock->drag.whole_window = true;
        dock->drag.ghost_window = source;
        dock->drag.grab_x = event->start_x;
        dock->drag.grab_y = event->start_y;
        Ca_DockWindowEntry *original = ca_dock_window_entry(dock->drag_origin, source);
        ca_window_get_rect(event->window, &original->x, &original->y, &original->width, &original->height);
    }
}

static bool sole_in_floating_window(const Ca_Dock *dock, int panel, int *window)
{
    const int group = ca_dock_layout_group_of(dock->layout, panel);
    if (group == CA_DOCK_NONE) return false;
    const int w = ca_dock_layout_window_of(dock->layout, group);
    *window = w;
    return w != CA_DOCK_MAIN_WINDOW && w != CA_DOCK_NONE &&
           ca_dock_group_tab_count(dock->layout, group) == 1 &&
           ca_dock_node_const(dock->layout, group)->parent == CA_DOCK_NONE;
}

/* Turns the dragged panel into a live window under the cursor. */
static bool begin_floating(Ca_Dock *dock, float sx, float sy)
{
    DragSession *drag = &dock->drag;
    int existing;
    if (sole_in_floating_window(dock, drag->panel, &existing)) {
        int x = 0, y = 0, width = 0, height = 0;
        ca_window_get_rect(window_view(dock, existing)->window, &x, &y, &width, &height);
        Ca_DockWindowEntry *original = ca_dock_window_entry(dock->drag_origin, existing);
        if (original) *original = (Ca_DockWindowEntry){
            .used = original->used, .root = original->root, .x = x, .y = y, .width = width, .height = height,
        };
        drag->ghost_window = existing;
        drag->grab_x = sx - (float)x;
        drag->grab_y = sy - (float)y;
        drag->phase = DRAG_FLOATING;
        return true;
    }
    const GroupView *group = group_view(dock, ca_dock_layout_group_of(dock->layout, drag->panel));
    float gx = 0.0f, gy = 0.0f, width = 0.0f, height = 0.0f;
    if (group && group->container) ca_div_screen_rect(group->container, &gx, &gy, &width, &height);
    float lx, ly;
    bool inside;
    window_local(dock, drag->source_window, drag->press_x, drag->press_y, &lx, &ly, &inside);
    width = fmaxf(width, MIN_FLOAT_WIDTH_PX * ui_scale(dock));
    height = fmaxf(height, MIN_FLOAT_HEIGHT_PX * ui_scale(dock));
    drag->grab_x = fminf(fmaxf(lx - gx, 16.0f), width - 16.0f);
    drag->grab_y = ca_window_get_title_bar_height(window_view(dock, drag->source_window)->window) + ly - gy;
    const int slot = ca_dock_layout_float(dock->layout, drag->panel, (int)(sx - drag->grab_x),
                                          (int)(sy - drag->grab_y), (int)width, (int)height);
    if (slot == CA_DOCK_NONE) return false;
    dock->spawn_unfocused = true;
    apply_structure(dock);
    open_missing_windows(dock);
    dock->spawn_unfocused = false;
    dock->structure_pending = false;
    drag->ghost_window = slot;
    drag->phase = DRAG_FLOATING;
    if (window_view(dock, slot) && window_view(dock, slot)->window) return true;
    ca_dock_layout_copy(dock->layout, dock->drag_origin);
    mark_structure_changed(dock);
    return false;
}

static bool cursor_in_source_strip_band(Ca_Dock *dock, float sx, float sy)
{
    const int group = ca_dock_layout_group_of(dock->layout, dock->drag.panel);
    const int source = dock->drag.source_window;
    float x, y, w, h, lx, ly;
    bool inside;
    if (group == CA_DOCK_NONE || !window_live(dock, source) || !strip_rect(dock, group, &x, &y, &w, &h))
        return false;
    window_local(dock, source, sx, sy, &lx, &ly, &inside);
    const float band = STRIP_BAND_PX * ui_scale(dock);
    return rect_contains(x - band, y - band, w + 2.0f * band, h + 2.0f * band, lx, ly);
}

static void drag_finish(Ca_Dock *dock, float sx, float sy)
{
    DragSession *drag = &dock->drag;
    const int panel = drag->panel;
    bool changed = false;
    if (drag->phase == DRAG_REORDER) {
        const int group = ca_dock_layout_group_of(dock->layout, panel);
        const int target = drag->hover.tab_index;
        const int at = ca_dock_layout_tab_index(dock->layout, panel);
        if (target >= 0 && group != CA_DOCK_NONE && target != at && target != at + 1)
            changed = ca_dock_layout_add_tab(dock->layout, group, panel, target);
    } else if (drag->phase == DRAG_FLOATING) {
        compute_hover(dock, sx, sy, &drag->hover);
        changed = apply_hover_drop(dock, &drag->hover, panel);
        if (!changed) mark_layout_dirty(dock);
    }
    ghost_release(dock);
    drag_reset(dock);
    if (changed) mark_structure_changed(dock);
}

/* Restores the layout and the source window's geometry when a drag is cancelled. */
static void cancel_drag(Ca_Dock *dock)
{
    if (dock->drag.phase == DRAG_NONE) return;
    const int source = dock->drag.source_window;
    ghost_release(dock);
    ca_dock_layout_copy(dock->layout, dock->drag_origin);
    const WindowView *view = window_view(dock, source);
    const Ca_DockWindowEntry *original = ca_dock_window_entry_const(dock->layout, source);
    if (source > CA_DOCK_MAIN_WINDOW && view && view->window && original) {
        ca_window_cancel_move(view->window);
        ca_window_set_position(view->window, original->x, original->y);
    }
    drag_reset(dock);
    mark_structure_changed(dock);
}

/* Advances the drag state machine from live platform input; runs once per tick. */
static void drag_update(Ca_Dock *dock)
{
    DragSession *drag = &dock->drag;
    if (drag->phase == DRAG_NONE) return;
    ca_instance_request_frame_after(dock->instance, DRAG_FRAME_SECONDS);
    for (int w = 0; w < (int)dock->windows.count; ++w) {
        if (window_live(dock, w) && ca_window_key_down(window_view(dock, w)->window, CA_KEY_ESCAPE)) {
            cancel_drag(dock);
            return;
        }
    }
    const int source = drag->source_window;
    float sx, sy;
    if (!window_live(dock, source) || ca_dock_layout_group_of(dock->layout, drag->panel) == CA_DOCK_NONE ||
        !ca_window_cursor_screen_position(window_view(dock, source)->window, &sx, &sy)) {
        ghost_release(dock);
        drag_reset(dock);
        return;
    }
    Ca_Window *source_window = window_view(dock, source)->window;
    const bool held = ca_window_mouse_button_down(source_window, 0);
    if (!held) ca_window_mouse_release_position(source_window, 0, &sx, &sy);

    if (drag->phase == DRAG_PRESSED) {
        const float dx = sx - drag->press_x, dy = sy - drag->press_y;
        const float threshold = DRAG_THRESHOLD_PX * ui_scale(dock);
        if (dx * dx + dy * dy < threshold * threshold) {
            if (!held) drag_finish(dock, sx, sy);
            return;
        }
        const int group = ca_dock_layout_group_of(dock->layout, drag->panel);
        int existing;
        if (drag->whole_window) {
            drag->phase = DRAG_FLOATING;
        } else if (sole_in_floating_window(dock, drag->panel, &existing)) {
            if (!begin_floating(dock, sx, sy)) drag_reset(dock);
        } else if (ca_dock_group_tab_count(dock->layout, group) > 1 &&
                   cursor_in_source_strip_band(dock, sx, sy)) {
            drag->phase = DRAG_REORDER;
        } else if (!begin_floating(dock, sx, sy)) {
            drag_reset(dock);
        }
        if (drag->phase == DRAG_NONE) return;
    }

    if (drag->phase == DRAG_REORDER) {
        if (!cursor_in_source_strip_band(dock, sx, sy)) {
            drag->phase = DRAG_FLOATING;
            if (!begin_floating(dock, sx, sy)) {
                drag_reset(dock);
                return;
            }
        } else {
            const int group = ca_dock_layout_group_of(dock->layout, drag->panel);
            float x, y, w, h, lx, ly, marker;
            bool inside;
            DockHover hover = { .window = CA_DOCK_NONE, .node = CA_DOCK_NONE, .target = -1, .tab_index = -1 };
            window_local(dock, source, sx, sy, &lx, &ly, &inside);
            if (strip_rect(dock, group, &x, &y, &w, &h)) {
                hover.window = source;
                hover.node = group;
                hover.tab_index = strip_insertion(dock, group, lx, &marker);
                hover.preview = true;
                hover.px = marker - MARKER_WIDTH_PX * 0.5f;
                hover.py = y;
                hover.pw = MARKER_WIDTH_PX;
                hover.ph = h;
            }
            drag->hover = hover;
            show_overlay(dock, &hover);
            if (!held) drag_finish(dock, sx, sy);
            return;
        }
    }

    if (drag->phase == DRAG_FLOATING) {
        const WindowView *ghost = window_view(dock, drag->ghost_window);
        if (ghost && ghost->window) {
            if (!drag->external_move)
                ca_window_set_position(ghost->window, (int)(sx - drag->grab_x), (int)(sy - drag->grab_y));
            ca_window_set_opacity(ghost->window, GHOST_OPACITY);
        }
    }
    compute_hover(dock, sx, sy, &drag->hover);
    show_overlay(dock, &drag->hover);
    if (!held) drag_finish(dock, sx, sy);
}

/* ============================================================
   FLOATING WINDOWS
   ============================================================ */

static bool on_window_close_request(Ca_Window *window, void *user_data)
{
    (void)window;
    const DockBinding *binding = user_data;
    WindowView *view = window_view(binding->dock, binding->index);
    if (!view || binding->index == CA_DOCK_MAIN_WINDOW || view->closing) return true;
    view->close_requested = true;
    return false;
}

static void on_window_destroyed(Ca_Window *window, void *user_data)
{
    const DockBinding *binding = user_data;
    WindowView *view = window_view(binding->dock, binding->index);
    if (view && view->window == window && binding->index != CA_DOCK_MAIN_WINDOW)
        *view = (WindowView){0};
}

/* Drags a floating window's complete layout by its title bar. */
static void on_window_title_drag(Ca_Window *window, bool active, void *user_data)
{
    const DockBinding *binding = user_data;
    Ca_Dock *dock = binding->dock;
    const int slot = binding->index;
    if (!active || dock->drag.phase != DRAG_NONE || slot <= CA_DOCK_MAIN_WINDOW || !window_live(dock, slot))
        return;
    const int root = ca_dock_layout_window_root(dock->layout, slot);
    if (root == CA_DOCK_NONE || !remember_origin(dock)) return;
    Ca_DockWindowEntry *original = ca_dock_window_entry(dock->drag_origin, slot);
    ca_window_get_rect(window, &original->x, &original->y, &original->width, &original->height);
    drag_reset(dock);
    dock->drag.phase = DRAG_PRESSED;
    ca_window_cursor_screen_position(window, &dock->drag.press_x, &dock->drag.press_y);
    dock->drag.panel = ca_dock_layout_active_panel(dock->layout, ca_dock_first_group(dock->layout, root));
    dock->drag.whole_window = true;
    dock->drag.source_window = slot;
    dock->drag.ghost_window = slot;
    dock->drag.external_move = true;
}

/* Returns a window that could not be opened to the main window so its panels stay reachable. */
static void reclaim_window(Ca_Dock *dock, int slot)
{
    const int main_root = ca_dock_layout_window_root(dock->layout, CA_DOCK_MAIN_WINDOW);
    if (!ca_dock_layout_dock_window(dock->layout, slot, CA_DOCK_MAIN_WINDOW, main_root,
                                    main_root == CA_DOCK_NONE ? CA_DOCK_ZONE_TAB : CA_DOCK_ZONE_RIGHT, -1, 0.0f)) {
        for (int p = 0; p < ca_dock_layout_panel_count(dock->layout); ++p) {
            const int group = ca_dock_layout_group_of(dock->layout, p);
            if (group != CA_DOCK_NONE && ca_dock_layout_window_of(dock->layout, group) == slot)
                close_panel_index(dock, p);
        }
    }
    mark_structure_changed(dock);
}

static void create_floating_window(Ca_Dock *dock, int slot)
{
    const Ca_DockWindowEntry *layout = ca_dock_window_entry_const(dock->layout, slot);
    DockBinding *binding = window_binding(dock, slot);
    WindowView *view = window_view(dock, slot);
    if (!layout || !binding || !view) return;
    const PanelSlot *active = panel_slot(dock, ca_dock_layout_active_panel(
        dock->layout, ca_dock_first_visible_group(dock->layout, layout->root)));
    const int width = layout->width > 0 ? layout->width
                    : (dock->desc.float_width > 0 ? dock->desc.float_width : DEFAULT_FLOAT_WIDTH);
    const int height = layout->height > 0 ? layout->height
                     : (dock->desc.float_height > 0 ? dock->desc.float_height : DEFAULT_FLOAT_HEIGHT);
    Ca_Window *window = ca_window_create(dock->instance, &(Ca_WindowDesc){
        .title = active ? active->desc.title : "Panel",
        .width = width,
        .height = height,
        .on_close = on_window_destroyed,
        .on_close_request = on_window_close_request,
        .on_close_data = binding,
        .no_focus = dock->spawn_unfocused,
        .title_left_aligned = true,
        .on_title_drag = on_window_title_drag,
        .on_title_drag_data = binding,
    });
    if (!window) {
        reclaim_window(dock, slot);
        return;
    }
    if (layout->width > 0 && layout->height > 0) ca_window_set_position(window, layout->x, layout->y);
    *view = (WindowView){ .window = window, .focus_order = ++dock->focus_order };
    ca_div_set_builder(ca_window_content_div(window), host_builder, binding);
}

/* Closes each panel of a dismissed window through its normal lifecycle. */
static void close_window_panels(Ca_Dock *dock, int slot)
{
    for (int p = 0; p < ca_dock_layout_panel_count(dock->layout); ++p) {
        const int group = ca_dock_layout_group_of(dock->layout, p);
        if (group != CA_DOCK_NONE && ca_dock_layout_window_of(dock->layout, group) == slot)
            close_panel_index(dock, p);
    }
}

static void apply_structure(Ca_Dock *dock)
{
    teardown_mounted(dock);
    sync_views(dock);
    for (size_t g = 0; g < dock->groups.count; ++g)
        *(GroupView *)ca_dyn_array_at(&dock->groups, g) = (GroupView){0};
    for (size_t s = 0; s < dock->splits.count; ++s)
        *(SplitView *)ca_dyn_array_at(&dock->splits, s) = (SplitView){0};
    dock->resizing_split = CA_DOCK_NONE;
    bump_epoch(dock);
    for (int w = CA_DOCK_MAIN_WINDOW + 1; w < (int)dock->windows.count; ++w) {
        WindowView *view = window_view(dock, w);
        const Ca_DockWindowEntry *entry = ca_dock_window_entry_const(dock->layout, w);
        /* A window whose panels were all closed hides, but keeps its place in the layout. */
        const bool shown = entry && entry->used && ca_dock_layout_node_visible(dock->layout, entry->root);
        if (shown || !view->window || view->closing) continue;
        view->closing = true;
        view->overlay = NULL;
        view->host = NULL;
        ca_window_close(view->window);
    }
    refresh_window_titles(dock);
}

/* Opens an OS window for every used layout window that has none yet. */
static void open_missing_windows(Ca_Dock *dock)
{
    if (!sync_views(dock)) return;
    for (int w = CA_DOCK_MAIN_WINDOW + 1; w < ca_dock_layout_window_count(dock->layout); ++w) {
        const Ca_DockWindowEntry *entry = ca_dock_window_entry_const(dock->layout, w);
        const WindowView *view = window_view(dock, w);
        if (entry->used && ca_dock_layout_node_visible(dock->layout, entry->root) && view && !view->window)
            create_floating_window(dock, w);
    }
}

/* Per-tick driver: focus order, drag, close requests, structure, notifications. */
static void pump(void *user_data)
{
    Ca_Dock *dock = user_data;
    if (!dock->mounted || dock->destroying) return;
    if (dock->resizing_split != CA_DOCK_NONE) {
        const SplitView *resizing = split_view(dock, dock->resizing_split);
        if (!resizing || !resizing->splitter || !ca_split_is_dragging(resizing->splitter))
            end_split_resize(dock);
    }
    for (int w = 0; w < (int)dock->windows.count; ++w) {
        if (window_live(dock, w) && ca_window_focused(window_view(dock, w)->window) &&
            dock->focused_window != w) {
            dock->focused_window = w;
            window_view(dock, w)->focus_order = ++dock->focus_order;
        }
    }
    drag_update(dock);
    for (int w = CA_DOCK_MAIN_WINDOW + 1; w < (int)dock->windows.count; ++w) {
        WindowView *view = window_view(dock, w);
        if (!view->close_requested) continue;
        view->close_requested = false;
        ghost_release(dock);
        drag_reset(dock);
        close_window_panels(dock, w);
        mark_structure_changed(dock);
    }
    if (dock->structure_pending) {
        dock->structure_pending = false;
        apply_structure(dock);
    }
    open_missing_windows(dock);
    if (dock->structure_pending) {
        dock->structure_pending = false;
        apply_structure(dock);
    }
    if (dock->layout_dirty && dock->drag.phase == DRAG_NONE) {
        dock->layout_dirty = false;
        emit_event(dock, CA_DOCK_EVENT_LAYOUT_CHANGED, CA_DOCK_NONE);
    }
}

/* ============================================================
   PUBLIC API
   ============================================================ */

static void free_bindings(Ca_DynArray *table)
{
    for (size_t i = 0; i < table->count; ++i) CA_FREE(*(DockBinding **)ca_dyn_array_at(table, i));
    ca_dyn_array_destroy(table);
}

static void dock_free(Ca_Dock *dock)
{
    free_bindings(&dock->node_bindings);
    free_bindings(&dock->window_bindings);
    ca_dyn_array_destroy(&dock->panels);
    ca_dyn_array_destroy(&dock->windows);
    ca_dyn_array_destroy(&dock->groups);
    ca_dyn_array_destroy(&dock->splits);
    ca_dyn_array_destroy(&dock->labels);
    ca_dock_layout_destroy(dock->layout);
    ca_dock_layout_destroy(dock->drag_origin);
    CA_FREE(dock);
}

Ca_Dock *ca_dock_create(Ca_Window *main_window, const Ca_DockDesc *desc)
{
    if (!main_window || !desc || !desc->default_layout || !ca_window_is_open(main_window)) return NULL;
    Ca_Dock *dock = CA_CALLOC(1, sizeof(*dock));
    if (!dock) return NULL;
    *dock = (Ca_Dock){
        .instance        = ca_window_instance(main_window),
        .main_window     = main_window,
        .desc            = *desc,
        .panels          = CA_DYN_ARRAY_INIT(PanelSlot),
        .windows         = CA_DYN_ARRAY_INIT(WindowView),
        .groups          = CA_DYN_ARRAY_INIT(GroupView),
        .splits          = CA_DYN_ARRAY_INIT(SplitView),
        .node_bindings   = CA_DYN_ARRAY_INIT(DockBinding *),
        .window_bindings = CA_DYN_ARRAY_INIT(DockBinding *),
        .labels          = CA_DYN_ARRAY_INIT(const char *),
        .focused_window  = CA_DOCK_NONE,
        .resizing_split  = CA_DOCK_NONE,
    };
    dock->layout = ca_dock_layout_create();
    dock->drag_origin = ca_dock_layout_create();
    dock->epoch = dock->instance ? ca_signal_u32(dock->instance, 0) : NULL;
    if (!dock->layout || !dock->drag_origin || !dock->epoch || !sync_views(dock)) {
        if (dock->epoch) ca_signal_destroy(dock->epoch);
        dock_free(dock);
        return NULL;
    }
    drag_reset(dock);
    window_view(dock, CA_DOCK_MAIN_WINDOW)->window = main_window;
    dock->pump = ca_frame_effect(dock->instance, pump, dock);
    if (!dock->pump) {
        ca_signal_destroy(dock->epoch);
        dock_free(dock);
        return NULL;
    }
    return dock;
}

bool ca_dock_register_panel(Ca_Dock *dock, const Ca_DockPanelDesc *desc)
{
    if (!dock || dock->mounted || !desc || !desc->id || !desc->title || !desc->build) return false;
    if (ca_dock_layout_find(dock->layout, desc->id) != CA_DOCK_NONE) return false;
    if (!ca_dyn_array_reserve(&dock->panels, dock->panels.count + 1u)) return false;
    const int index = ca_dock_layout_register(dock->layout, desc->id);
    if (index == CA_DOCK_NONE) return false;
    const PanelSlot slot = { .desc = *desc };
    ca_dyn_array_push(&dock->panels, &slot);
    return true;
}

/* Removes transient panels (those with an availability predicate) from a layout. */
static void drop_transient(const Ca_Dock *dock, Ca_DockLayout *layout)
{
    for (size_t p = 0; p < dock->panels.count; ++p) {
        const PanelSlot *slot = ca_dyn_array_at_const(&dock->panels, p);
        if (slot->desc.available) ca_dock_layout_remove(layout, (int)p);
    }
}

static void seed_default_layout(Ca_Dock *dock)
{
    ca_dock_layout_clear(dock->layout);
    dock->desc.default_layout(dock->layout, dock->desc.default_layout_data);
    if (!ca_dock_layout_valid(dock->layout)) ca_dock_layout_clear(dock->layout);
}

/*
 * A saved layout without a central group (e.g. from before the host defined
 * one) adopts the default layout's central panel: that panel's group (or its
 * remembered home) in the main window becomes central.
 */
static void adopt_default_central(Ca_Dock *dock)
{
    if (ca_dock_layout_central(dock->layout) != CA_DOCK_NONE) return;
    Ca_DockLayout *defaults = ca_dock_layout_create();
    if (!defaults || !ca_dock_layout_copy(defaults, dock->layout)) {
        ca_dock_layout_destroy(defaults);
        return;
    }
    ca_dock_layout_clear(defaults);
    dock->desc.default_layout(defaults, dock->desc.default_layout_data);
    const int central = ca_dock_layout_central(defaults);
    const int panel = ca_dock_layout_active_panel(defaults, central);
    ca_dock_layout_destroy(defaults);
    if (panel == CA_DOCK_NONE) return;
    int group = ca_dock_layout_group_of(dock->layout, panel);
    if (group == CA_DOCK_NONE) group = ca_dock_layout_home_of(dock->layout, panel);
    if (ca_dock_layout_window_of(dock->layout, group) == CA_DOCK_MAIN_WINDOW)
        ca_dock_layout_set_central(dock->layout, group);
}

bool ca_dock_mount(Ca_Dock *dock)
{
    if (!dock || dock->mounted) return false;
    const char *saved = dock->desc.saved_layout;
    dock->desc.saved_layout = NULL;
    if (saved && ca_dock_layout_load(dock->layout, saved)) {
        drop_transient(dock, dock->layout);
        adopt_default_central(dock);
    } else {
        seed_default_layout(dock);
    }
    if (!sync_views(dock)) return false;

    dock->root = ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL, .style = "ca-dock-root", .id = "ca-dock-root",
    });
    ca_div_end();
    if (!dock->root) return false;
    ca_div_set_builder(dock->root, host_builder, window_binding(dock, CA_DOCK_MAIN_WINDOW));
    dock->mounted = true;
    open_missing_windows(dock);
    return true;
}

void ca_dock_destroy(Ca_Dock *dock)
{
    if (!dock) return;
    ghost_release(dock);
    drag_reset(dock);
    teardown_mounted(dock);
    dock->destroying = true;
    for (int w = CA_DOCK_MAIN_WINDOW + 1; w < (int)dock->windows.count; ++w) {
        WindowView *view = window_view(dock, w);
        if (!view->window) continue;
        view->closing = true;
        ca_window_destroy(view->window);
        *view = (WindowView){0};
    }
    ca_effect_destroy(dock->pump);
    if (dock->root && ca_window_is_open(dock->main_window)) ca_div_destroy(dock->root);
    ca_signal_destroy(dock->epoch);
    dock_free(dock);
}

/* Copies current floating window rects into the layout before persisting. */
static void capture_window_rects(Ca_Dock *dock)
{
    for (int w = CA_DOCK_MAIN_WINDOW + 1; w < (int)dock->windows.count; ++w) {
        const WindowView *view = window_view(dock, w);
        Ca_DockWindowEntry *entry = ca_dock_window_entry(dock->layout, w);
        if (!entry || !entry->used || !view->window) continue;
        ca_window_get_rect(view->window, &entry->x, &entry->y, &entry->width, &entry->height);
    }
}

/* Records live split ratios. Requested fixed sizes are kept (a clamped pane
   regains its size when space returns, as ImGui keeps SizeRef); a fixed pane
   still deriving its size from the ratio records its laid-out size, so it
   persists in pixels across window sizes. */
static void capture_split_ratios(Ca_Dock *dock)
{
    for (int s = 0; s < (int)dock->splits.count; ++s) {
        const SplitView *view = split_view(dock, s);
        Ca_DockNodeEntry *node = ca_dock_node(dock->layout, s);
        if (!view->splitter || view->locked || !node || node->kind != CA_DOCK_NODE_SPLIT) continue;
        const float first = ca_split_get_pane_size(view->splitter, 0);
        const float second = ca_split_get_pane_size(view->splitter, 1);
        if (first + second <= 0.0f) continue;
        float ratio = first / (first + second);
        if (ratio < SPLIT_MIN_RATIO) ratio = SPLIT_MIN_RATIO;
        if (ratio > SPLIT_MAX_RATIO) ratio = SPLIT_MAX_RATIO;
        node->ratio = ratio;
        const int fixed = fixed_pane_of(dock, s);
        if (fixed != CA_SPLIT_FIXED_NONE && node->size <= 0.0f)
            node->size = fixed == CA_SPLIT_FIXED_FIRST ? first : second;
    }
}

size_t ca_dock_save(Ca_Dock *dock, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!dock) return 0;
    capture_window_rects(dock);
    capture_split_ratios(dock);
    Ca_DockLayout *copy = ca_dock_layout_create();
    if (!copy || !ca_dock_layout_copy(copy, dock->layout)) {
        ca_dock_layout_destroy(copy);
        return 0;
    }
    drop_transient(dock, copy);
    const size_t needed = ca_dock_layout_save(copy, out, cap);
    ca_dock_layout_destroy(copy);
    return needed;
}

const Ca_DockLayout *ca_dock_layout(const Ca_Dock *dock)
{
    return dock ? dock->layout : NULL;
}

bool ca_dock_panel_visible(const Ca_Dock *dock, const char *id)
{
    if (!dock) return false;
    return ca_dock_layout_group_of(dock->layout, ca_dock_layout_find(dock->layout, id)) != CA_DOCK_NONE;
}

void ca_dock_focus_panel(Ca_Dock *dock, const char *id)
{
    if (!dock) return;
    const int panel = ca_dock_layout_find(dock->layout, id);
    if (panel == CA_DOCK_NONE) return;
    if (ca_dock_layout_group_of(dock->layout, panel) == CA_DOCK_NONE && !ca_dock_open_panel(dock, id)) return;
    const int group = ca_dock_layout_group_of(dock->layout, panel);
    const int index = ca_dock_layout_tab_index(dock->layout, panel);
    const GroupView *view = group_view(dock, group);
    if (ca_dock_layout_active_panel(dock->layout, group) != panel) {
        /* A live tab bar reports the switch through on_tab_change; otherwise
           update the layout here and let the pending rebuild show it. */
        if (view && view->tabs && !dock->structure_pending) {
            ca_tabs_set_active(view->tabs, index);
        } else {
            teardown_panel(dock, ca_dock_layout_active_panel(dock->layout, group));
            ca_dock_layout_set_active(dock->layout, group, index);
            mark_structure_changed(dock);
            emit_event(dock, CA_DOCK_EVENT_PANEL_ACTIVATED, panel);
        }
    }
    const int window = ca_dock_layout_window_of(dock->layout, group);
    if (window_live(dock, window)) ca_window_focus(window_view(dock, window)->window);
}

bool ca_dock_open_panel(Ca_Dock *dock, const char *id)
{
    if (!dock || !dock->mounted) return false;
    const int panel = ca_dock_layout_find(dock->layout, id);
    const PanelSlot *slot = panel_slot(dock, panel);
    if (!slot || (slot->desc.available && !slot->desc.available(slot->desc.user_data))) return false;
    if (ca_dock_layout_group_of(dock->layout, panel) != CA_DOCK_NONE) {
        ca_dock_focus_panel(dock, id);
        return true;
    }
    /* A closed panel returns to the group that remembered it (ImGui keeps its DockId). */
    int target = ca_dock_layout_home_of(dock->layout, panel);
    if (target == CA_DOCK_NONE && slot->desc.dock_beside)
        target = ca_dock_layout_group_of(dock->layout, ca_dock_layout_find(dock->layout, slot->desc.dock_beside));
    if (target == CA_DOCK_NONE)
        target = ca_dock_first_visible_group(dock->layout, ca_dock_layout_window_root(dock->layout, CA_DOCK_MAIN_WINDOW));
    const bool opened = target == CA_DOCK_NONE
        ? ca_dock_layout_dock_root(dock->layout, CA_DOCK_MAIN_WINDOW, panel, CA_DOCK_ZONE_TAB, 0.0f)
        : ca_dock_layout_add_tab(dock->layout, target, panel, -1);
    if (!opened) return false;
    mark_structure_changed(dock);
    const int window = ca_dock_layout_window_of(dock->layout, ca_dock_layout_group_of(dock->layout, panel));
    if (window_live(dock, window)) ca_window_focus(window_view(dock, window)->window);
    emit_event(dock, CA_DOCK_EVENT_PANEL_OPENED, panel);
    emit_event(dock, CA_DOCK_EVENT_PANEL_ACTIVATED, panel);
    return true;
}

void ca_dock_close_panel(Ca_Dock *dock, const char *id)
{
    if (dock) close_panel_index(dock, ca_dock_layout_find(dock->layout, id));
}

bool ca_dock_toggle_panel(Ca_Dock *dock, const char *id)
{
    if (ca_dock_panel_visible(dock, id)) {
        ca_dock_close_panel(dock, id);
        return false;
    }
    return ca_dock_open_panel(dock, id);
}

void ca_dock_invalidate_panel(Ca_Dock *dock, const char *id)
{
    if (!dock) return;
    const int panel = ca_dock_layout_find(dock->layout, id);
    const int group = ca_dock_layout_group_of(dock->layout, panel);
    if (group == CA_DOCK_NONE || ca_dock_layout_active_panel(dock->layout, group) != panel) return;
    teardown_panel(dock, panel);
    const GroupView *view = group_view(dock, group);
    if (!dock->structure_pending && view && view->content) ca_div_invalidate(view->content);
}

void ca_dock_reset_layout(Ca_Dock *dock)
{
    if (!dock || !dock->mounted) return;
    cancel_drag(dock);
    teardown_mounted(dock);
    const int count = ca_dock_layout_panel_count(dock->layout);
    bool *was_open = count ? CA_MALLOC((size_t)count * sizeof(bool)) : NULL;
    for (int p = 0; was_open && p < count; ++p)
        was_open[p] = ca_dock_layout_group_of(dock->layout, p) != CA_DOCK_NONE;
    seed_default_layout(dock);
    mark_structure_changed(dock);
    for (int p = 0; was_open && p < count; ++p) {
        const bool now_open = ca_dock_layout_group_of(dock->layout, p) != CA_DOCK_NONE;
        if (was_open[p] && !now_open) emit_event(dock, CA_DOCK_EVENT_PANEL_CLOSED, p);
        else if (!was_open[p] && now_open) emit_event(dock, CA_DOCK_EVENT_PANEL_OPENED, p);
    }
    CA_FREE(was_open);
}

int ca_dock_panel_count(const Ca_Dock *dock)
{
    return dock ? ca_dock_layout_panel_count(dock->layout) : 0;
}

const char *ca_dock_panel_id(const Ca_Dock *dock, int index)
{
    return dock ? ca_dock_layout_panel_id(dock->layout, index) : NULL;
}

const char *ca_dock_panel_title(const Ca_Dock *dock, int index)
{
    const PanelSlot *slot = dock && index >= 0 ? ca_dyn_array_at_const(&dock->panels, (size_t)index) : NULL;
    return slot ? slot->desc.title : NULL;
}
