// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/*
 * ca_dock.h — dockable panel workspace.
 *
 * Panels live in tab groups arranged by resizable splits. Each OS window
 * (the host's main window plus any number of floating windows) owns one
 * split/group tree. Users drag tabs to reorder them, drag a tab out to float
 * it as a live window, and drop it on a target cross or window-edge target to
 * dock it beside, inside or around any group in any window. A floating
 * window's whole tree can be dragged by its tab strip or title bar and docked
 * elsewhere in one move. Escape cancels a drag; Shift suppresses targets.
 *
 * BEHAVIOUR — modelled on Dear ImGui's docking:
 *   Central group  The main window may designate one central group. It never
 *                  collapses: undocking its last panel leaves an empty,
 *                  droppable area in place, and when the window resizes only
 *                  the central group changes size. Every other pane keeps its
 *                  pixel size. When a group is split, the side that keeps the
 *                  existing content keeps the central role.
 *   Undock         Dragging out the last panel of any other group removes the
 *                  group; its sibling takes the space.
 *   Close          Closing a panel (tab button, ca_dock_close_panel, closing
 *                  its floating window) remembers its group: the group stays
 *                  hidden in the tree, and reopening the panel returns it to
 *                  the same place and size. Docking the panel elsewhere
 *                  forgets that home.
 *   Resizing       Dragging a divider resizes only the panes touching it;
 *                  panes further away keep their pixel size.
 *   Drops          A new split gives the incoming pane the dragged window's
 *                  size along the split axis, up to half of the target.
 *
 * The component is mechanism only. The host decides which panels exist, what
 * they draw, the default arrangement, and where layouts are persisted.
 *
 * TWO LAYERS
 *   Ca_DockLayout  Pure layout data and operations. No windows required, so
 *                  hosts can seed default layouts and tests can run headless.
 *   Ca_Dock        The live workspace: emits the layout as widgets, drives
 *                  drag and drop, and owns the floating windows.
 *
 * LIFECYCLE
 *   dock = ca_dock_create(main_window, &desc);
 *   ca_dock_register_panel(dock, &panel);   (every panel, before mounting)
 *   ca_dock_mount(dock);                    (inside the main window's build
 *                                            context; emits the dock root)
 *   ...                                     (the dock updates itself each tick)
 *   ca_dock_save(dock, buffer, cap);        (whenever the host persists)
 *   ca_dock_destroy(dock);                  (before the main window's
 *                                            resources are released, e.g. from
 *                                            its on_close callback)
 *
 * PANELS
 *   build     emits the panel's UI into the current container whenever the
 *             panel becomes visible (active tab of a mounted group).
 *   teardown  runs while the panel's widgets are still alive, just before they
 *             are rebuilt or destroyed; drop any widget handles here.
 *
 * OBSERVATION
 *   Ca_DockDesc.on_event reports panels opened, closed and activated, and a
 *   coalesced layout-changed notification at most once per tick, which hosts
 *   use to schedule persistence.
 *
 * PERSISTENCE
 *   ca_dock_save / ca_dock_layout_save write compact JSON with snprintf
 *   semantics; ca_dock_layout_load validates before replacing anything.
 *   Unknown panel ids and emptied groups are dropped on load. Panels with an
 *   `available` predicate are transient: never saved, never restored.
 *
 * STYLING — every part carries a class whose system-sheet defaults read the
 * --ca-* palette, so host themes apply without code:
 *   .ca-dock-root  .ca-dock-host  .ca-dock-group(.ca-dock-empty = empty central
 *   group)  .ca-dock-tabs  .ca-dock-tab
 *   .ca-dock-content  .ca-dock-panel  .ca-dock-overlay  .ca-dock-preview
 *   .ca-dock-target(.hot)  .ca-dock-target-glyph(.hot)
 *
 * THREADING: main (UI) thread only, like the rest of Causality.
 */

#pragma once

#include "causality.h"
#include "ca_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   COMMON
   ============================================================ */

/* "No index" for panels, nodes and windows. */
#define CA_DOCK_NONE        (-1)

/* Window index of the host's main window in every layout. */
#define CA_DOCK_MAIN_WINDOW 0

/* Panel id capacity in bytes, including the terminator. */
#define CA_DOCK_ID_MAX      64

/* Where a panel lands relative to a target group or window. */
typedef enum Ca_DockZone {
    CA_DOCK_ZONE_TAB = 0,   /* join the target as a tab */
    CA_DOCK_ZONE_LEFT,
    CA_DOCK_ZONE_RIGHT,
    CA_DOCK_ZONE_TOP,
    CA_DOCK_ZONE_BOTTOM,
} Ca_DockZone;

/* ============================================================
   LAYOUT — pure data
   ============================================================

   Indices are stable for the lifetime of a node, panel or window entry.
   Every mutating operation is failure-atomic: when it returns false (or
   CA_DOCK_NONE) the layout is unchanged.

   Split ratios passed to docking operations are the fraction of the target
   given to the incoming pane; <= 0 splits evenly. They are clamped to
   [0.05, 0.95].                                                   */

typedef struct Ca_DockLayout Ca_DockLayout;

typedef enum Ca_DockNodeKind {
    CA_DOCK_NODE_NONE = 0,  /* unused slot or invalid index      */
    CA_DOCK_NODE_SPLIT,     /* two children and a ratio           */
    CA_DOCK_NODE_GROUP,     /* tab group of one or more panels    */
} Ca_DockNodeKind;

/** Creates an empty layout: a main window with no groups and no panels. */
CA_API Ca_DockLayout *ca_dock_layout_create(void);

/** Releases a layout. NULL is a no-op. */
CA_API void ca_dock_layout_destroy(Ca_DockLayout *layout);

/**
 * Replaces destination with a deep copy of source.
 * @return False on allocation failure; destination is then unchanged.
 */
CA_API bool ca_dock_layout_copy(Ca_DockLayout *destination, const Ca_DockLayout *source);

/**
 * Registers a panel id (1..CA_DOCK_ID_MAX-1 bytes).
 * @return Panel index; the existing index for a repeated id; CA_DOCK_NONE on
 *         invalid ids or allocation failure.
 */
CA_API int ca_dock_layout_register(Ca_DockLayout *layout, const char *id);

/** @return Panel index for id, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_find(const Ca_DockLayout *layout, const char *id);

/** @return Number of registered panels. */
CA_API int ca_dock_layout_panel_count(const Ca_DockLayout *layout);

/** @return Registered id of a panel, or NULL. Valid until the layout is destroyed. */
CA_API const char *ca_dock_layout_panel_id(const Ca_DockLayout *layout, int panel);

/** Removes every group, split and floating window; registered panels are kept. */
CA_API void ca_dock_layout_clear(Ca_DockLayout *layout);

/** Creates an empty, parentless tab group. @return Node index or CA_DOCK_NONE. */
CA_API int ca_dock_layout_new_group(Ca_DockLayout *layout);

/**
 * Creates a split owning two distinct, existing, parentless nodes.
 * @param horizontal True places the children side by side.
 * @param ratio      Fraction given to `first`, clamped to [0.05, 0.95].
 * @return Node index or CA_DOCK_NONE.
 */
CA_API int ca_dock_layout_new_split(Ca_DockLayout *layout, bool horizontal, float ratio,
                                    int first, int second);

/**
 * Makes node (or CA_DOCK_NONE for empty) the root of an existing window.
 * @return False for an invalid window or node.
 */
CA_API bool ca_dock_layout_set_root(Ca_DockLayout *layout, int window, int node);

/**
 * Designates the main window's central group (see BEHAVIOUR), replacing any
 * previous one; CA_DOCK_NONE clears it.
 * @return False when group is not a group in the main window's tree.
 */
CA_API bool ca_dock_layout_set_central(Ca_DockLayout *layout, int group);

/** @return The central group, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_central(const Ca_DockLayout *layout);

/**
 * Sets the logical pixel size kept by a split's fixed pane: the child that
 * does not contain the central group. <= 0 derives it from the split ratio.
 * @return False when split is not a split node.
 */
CA_API bool ca_dock_layout_set_split_size(Ca_DockLayout *layout, int split, float size);

/**
 * Moves a panel into a group as a tab and activates it.
 * @param index Insert position in the group's current order (before the
 *              move); negative or out of range appends.
 */
CA_API bool ca_dock_layout_add_tab(Ca_DockLayout *layout, int group, int panel, int index);

/**
 * Splits a target group so the panel gets its own group beside it.
 * CA_DOCK_ZONE_TAB behaves like ca_dock_layout_add_tab.
 * @param ratio Fraction of the target given to the panel (<= 0 = half).
 * @return False for a no-op (moving a group's only panel beside itself) or invalid input.
 */
CA_API bool ca_dock_layout_split(Ca_DockLayout *layout, int target, int panel,
                                 Ca_DockZone zone, float ratio);

/**
 * Docks a panel along a whole window's edge, or as its only group when the
 * window is empty. CA_DOCK_ZONE_TAB joins the window's first group.
 * @param ratio Fraction of the window given to the panel (<= 0 = half).
 */
CA_API bool ca_dock_layout_dock_root(Ca_DockLayout *layout, int window, int panel,
                                     Ca_DockZone zone, float ratio);

/**
 * Moves a floating window's whole tree beside target (keeping its splits)
 * or, for CA_DOCK_ZONE_TAB, into the target group: an empty target group is
 * replaced by the whole tree (an empty central group passes its central role
 * to the tree's first visible group); a non-empty target receives the tabs
 * of the tree's only visible group, and is refused when the tree visibly has
 * several. Panels remembered by the consumed tree are re-homed to the
 * target. The source window is removed.
 * @param source Floating window to consume (never the main window).
 * @param window Destination window, distinct from source.
 * @param target Destination group or subtree root in `window`; CA_DOCK_NONE
 *               only when `window` is empty.
 * @param index  Tab insertion position for a merge; negative appends.
 * @param ratio  Fraction of the target given to the tree for edge zones.
 */
CA_API bool ca_dock_layout_dock_window(Ca_DockLayout *layout, int source, int window,
                                       int target, Ca_DockZone zone, int index, float ratio);

/**
 * Moves a panel into a new floating window with the given screen rect.
 * @return Window index; CA_DOCK_NONE for invalid input, when the panel already
 *         is the only content of a floating window, or on allocation failure.
 */
CA_API int ca_dock_layout_float(Ca_DockLayout *layout, int panel,
                                int x, int y, int width, int height);

/**
 * Removes a panel and forgets where it was (an undock). An emptied group
 * collapses unless it is central or remembered by a closed panel, and its
 * sibling takes the space.
 */
CA_API void ca_dock_layout_remove(Ca_DockLayout *layout, int panel);

/**
 * Closes a panel but remembers its group, which stays (hidden when empty) so
 * that adding the panel back to its home restores the same place and size.
 */
CA_API void ca_dock_layout_close(Ca_DockLayout *layout, int panel);

/** @return Group a closed panel will return to, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_home_of(const Ca_DockLayout *layout, int panel);

/** @return True when a node shows anything: a group with tabs, the central group, or a split with a visible child. */
CA_API bool ca_dock_layout_node_visible(const Ca_DockLayout *layout, int node);

/** Selects tab `index` of a group. */
CA_API bool ca_dock_layout_set_active(Ca_DockLayout *layout, int group, int index);

/** @return Group holding the panel, or CA_DOCK_NONE when it is not in the layout. */
CA_API int ca_dock_layout_group_of(const Ca_DockLayout *layout, int panel);

/** @return Window whose tree contains node, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_window_of(const Ca_DockLayout *layout, int node);

/** @return The panel's position in its group's tab order, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_tab_index(const Ca_DockLayout *layout, int panel);

/** @return The panel shown by a group, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_active_panel(const Ca_DockLayout *layout, int group);

/** @return Kind of node at an index (CA_DOCK_NODE_NONE when invalid). */
CA_API Ca_DockNodeKind ca_dock_layout_node_kind(const Ca_DockLayout *layout, int node);

/** @return Number of window entries (used or not); index 0 is the main window. */
CA_API int ca_dock_layout_window_count(const Ca_DockLayout *layout);

/** @return Root node of a used window, or CA_DOCK_NONE. */
CA_API int ca_dock_layout_window_root(const Ca_DockLayout *layout, int window);

/** @return True when every structural invariant of every tree holds. */
CA_API bool ca_dock_layout_valid(const Ca_DockLayout *layout);

/**
 * Serializes the layout as compact JSON.
 * @param out  Destination buffer; may be NULL when cap is 0.
 * @param cap  Capacity of out in bytes.
 * @return Length the full text needs (excluding the terminator). The text is
 *         complete only when the result is less than cap; out is always
 *         terminated when cap > 0.
 */
CA_API size_t ca_dock_layout_save(const Ca_DockLayout *layout, char *out, size_t cap);

/**
 * Replaces the layout's arrangement from JSON produced by ca_dock_layout_save.
 * Registered panels are kept; unknown ids and empty groups are dropped.
 * @return False, leaving the layout untouched, when the text is malformed,
 *         yields an invalid layout, or allocation fails.
 */
CA_API bool ca_dock_layout_load(Ca_DockLayout *layout, const char *text);

/* ============================================================
   DOCK — live workspace
   ============================================================ */

typedef struct Ca_Dock Ca_Dock;

typedef enum Ca_DockEventKind {
    CA_DOCK_EVENT_PANEL_OPENED = 0,   /* panel entered the layout              */
    CA_DOCK_EVENT_PANEL_CLOSED,       /* panel left the layout                 */
    CA_DOCK_EVENT_PANEL_ACTIVATED,    /* panel became its group's visible tab  */
    CA_DOCK_EVENT_LAYOUT_CHANGED,     /* arrangement changed (once per tick)   */
} Ca_DockEventKind;

typedef struct Ca_DockEvent {
    Ca_DockEventKind kind;
    const char      *panel_id;        /* NULL for CA_DOCK_EVENT_LAYOUT_CHANGED */
} Ca_DockEvent;

/**
 * Observer for dock changes. Runs on the UI thread; must not destroy the dock.
 * @param dock       Dock that changed.
 * @param event      What happened; valid only during the call.
 * @param user_data  Ca_DockDesc.event_data.
 */
typedef void (*Ca_DockEventFn)(Ca_Dock *dock, const Ca_DockEvent *event, void *user_data);

/**
 * Seeds the factory arrangement into a cleared layout whose panels are all
 * registered. Runs at mount when no saved layout applies, and on reset.
 */
typedef void (*Ca_DockDefaultLayoutFn)(Ca_DockLayout *layout, void *user_data);

typedef struct Ca_DockPanelDesc {
    const char *id;            /* stable key for persistence and lookups      */
    const char *title;         /* tab caption and floating window title       */
    const char *group_style;   /* extra classes on the hosting group while active, or NULL */
    const char *dock_beside;   /* panel id whose group this joins when opened, or NULL */
    /* Emits the panel UI into the current container. `window` hosts it. */
    void (*build)(Ca_Dock *dock, Ca_Window *window, void *user_data);
    /* Drops widget handles while the panel's widgets are still alive. May be NULL. */
    void (*teardown)(void *user_data);
    /* Readiness predicate; panels that have one are transient. May be NULL. */
    bool (*available)(void *user_data);
    void *user_data;
} Ca_DockPanelDesc;

typedef struct Ca_DockDesc {
    Ca_DockDefaultLayoutFn default_layout;     /* required                          */
    void                  *default_layout_data;
    const char            *saved_layout;       /* JSON from ca_dock_save, or NULL   */
    Ca_DockEventFn         on_event;           /* optional                          */
    void                  *event_data;
    int                    float_width;        /* new floating window size; 0 = 720 */
    int                    float_height;       /* 0 = 480                           */
} Ca_DockDesc;

/**
 * Creates a dock workspace for a main window. Copies desc; saved_layout is
 * read only during ca_dock_mount.
 * @return The dock, or NULL on invalid arguments or allocation failure.
 */
CA_API Ca_Dock *ca_dock_create(Ca_Window *main_window, const Ca_DockDesc *desc);

/**
 * Registers a panel. Strings in desc must outlive the dock.
 * @return False after mounting, for missing id/title/build, a duplicate id,
 *         or allocation failure.
 */
CA_API bool ca_dock_register_panel(Ca_Dock *dock, const Ca_DockPanelDesc *desc);

/**
 * Applies the saved layout (falling back to the default layout), emits the
 * dock root into the current container of the main window's build context,
 * and opens floating windows. A saved layout without a central group adopts
 * the default layout's central panel. Call once.
 * @return False when already mounted or on allocation failure.
 */
CA_API bool ca_dock_mount(Ca_Dock *dock);

/**
 * Tears down mounted panels, closes floating windows and frees the dock.
 * NULL is a no-op. Must run while the main window is still alive.
 */
CA_API void ca_dock_destroy(Ca_Dock *dock);

/** Serializes the current arrangement, including floating window rects; see ca_dock_layout_save. */
CA_API size_t ca_dock_save(Ca_Dock *dock, char *out, size_t cap);

/** Read-only view of the live layout. Valid until the next dock mutation. */
CA_API const Ca_DockLayout *ca_dock_layout(const Ca_Dock *dock);

/** Shows a panel: focuses it when open, otherwise docks it beside its default neighbour. */
CA_API bool ca_dock_open_panel(Ca_Dock *dock, const char *id);

/** Removes a panel from the layout. */
CA_API void ca_dock_close_panel(Ca_Dock *dock, const char *id);

/** Opens a closed panel or closes an open one. @return True when now open. */
CA_API bool ca_dock_toggle_panel(Ca_Dock *dock, const char *id);

/** Brings a panel to the front of its group and focuses its window, opening it if needed. */
CA_API void ca_dock_focus_panel(Ca_Dock *dock, const char *id);

/** @return True when the panel is part of the layout. */
CA_API bool ca_dock_panel_visible(const Ca_Dock *dock, const char *id);

/** Tears down a visible panel and rebuilds it on the next tick. */
CA_API void ca_dock_invalidate_panel(Ca_Dock *dock, const char *id);

/** Restores the default layout and closes floating windows. */
CA_API void ca_dock_reset_layout(Ca_Dock *dock);

/** @return Number of registered panels. */
CA_API int ca_dock_panel_count(const Ca_Dock *dock);

/** @return Registered id of panel `index`, or NULL. */
CA_API const char *ca_dock_panel_id(const Ca_Dock *dock, int index);

/** @return Registered title of panel `index`, or NULL. */
CA_API const char *ca_dock_panel_title(const Ca_Dock *dock, int index);

#ifdef __cplusplus
}
#endif
