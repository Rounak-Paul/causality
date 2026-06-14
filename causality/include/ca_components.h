// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

/*
 * ca_components.h — Higher-level UI components built on the Causality core.
 *
 * These are ready-made widgets (checkbox, slider, tabs, tree view, etc.)
 * that wrap the core primitives (div, button, label, input).
 * Include this header when you need these components; the core API
 * in causality.h is self-contained without them.
 */

#include "causality.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   COMPONENT CALLBACK TYPES
   ============================================================ */

/*
 * Callback fired when a checkbox or radio button changes state.
 *
 * cb         The checkbox (or Ca_Checkbox-compatible handle) that changed.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_CheckFn)(Ca_Checkbox *cb, void *user_data);

/*
 * Callback fired when a slider value changes.
 *
 * slider     The slider that changed.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_SliderFn)(Ca_Slider *slider, void *user_data);

/*
 * Callback fired when a toggle switch changes state.
 *
 * toggle     The toggle that changed.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_ToggleFn)(Ca_Toggle *toggle, void *user_data);

/*
 * Callback fired when a select/dropdown selection changes.
 *
 * sel        The select widget that changed.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_SelectFn)(Ca_Select *sel, void *user_data);

/*
 * Callback fired when the active tab in a tab bar changes.
 *
 * tabs       The tab bar that changed.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_TabFn)(Ca_TabBar *tabs, void *user_data);

/*
 * Callback fired when a tree node is expanded or collapsed.
 *
 * tn         The tree node that was toggled.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_TreeToggleFn)(Ca_TreeNode *tn, void *user_data);

/*
 * Callback fired when a context menu item is selected.
 *
 * item_index  Zero-based index of the selected item.
 * user_data   Caller-supplied context pointer.
 */
typedef void (*Ca_MenuFn)(int item_index, void *user_data);

/*
 * Callback fired when a context menu is about to open.
 *
 * local_x   X position relative to the target widget's top-left.
 * local_y   Y position relative to the target widget's top-left.
 * screen_x  X position in window screen coordinates.
 * screen_y  Y position in window screen coordinates.
 * user_data Caller-supplied context pointer.
 */
typedef void (*Ca_ContextMenuOpenFn)(float local_x, float local_y,
                                     float screen_x, float screen_y,
                                     void *user_data);

/*
 * Callback fired when a menu bar item action is invoked.
 *
 * user_data  Caller-supplied context pointer bound to the Ca_MenuItemDesc.
 */
typedef void (*Ca_MenuActionFn)(void *user_data);

/* ============================================================
   COMPONENT DESCRIPTORS
   ============================================================ */

typedef struct Ca_CheckboxDesc {
    const char *text;
    bool        checked;
    Ca_CheckFn  on_change;
    void       *change_data;
    const char *id, *style;
    bool        hidden;
    bool        disabled;
} Ca_CheckboxDesc;

typedef struct Ca_RadioDesc {
    const char *text;
    int         group;
    int         value;
    Ca_CheckFn  on_change;     /* reuses check callback signature */
    void       *change_data;
    const char *id, *style;
    bool        hidden;
    bool        disabled;
} Ca_RadioDesc;

typedef struct Ca_SliderDesc {
    float       min, max, value;
    float       width;
    Ca_SliderFn on_change;
    void       *change_data;
    const char *id, *style;
    bool        hidden;
    bool        disabled;
} Ca_SliderDesc;

typedef struct Ca_ToggleDesc {
    bool        on;
    Ca_ToggleFn on_change;
    void       *change_data;
    const char *id, *style;
    bool        hidden;
    bool        disabled;
} Ca_ToggleDesc;

typedef struct Ca_ProgressDesc {
    float       value;         /* 0.0 – 1.0 */
    float       width, height;
    uint32_t    bar_color;
    const char *id, *style;
    bool        hidden;
} Ca_ProgressDesc;

typedef struct Ca_SelectDesc {
    const char *const *options;
    int          option_count;
    int          selected;
    float        width;
    Ca_SelectFn  on_change;
    void        *change_data;
    const char  *id, *style;
    bool         hidden;
    bool         disabled;
} Ca_SelectDesc;

typedef struct Ca_TabBarDesc {
    const char **labels;
    int          count;
    int          active;
    Ca_TabFn     on_change;
    void        *change_data;
    const char  *id, *style;
    uint32_t     active_bg;        /* active tab background   (0 = default) */
    uint32_t     inactive_bg;      /* inactive tab background (0 = default) */
    uint32_t     active_text;      /* active tab text color   (0 = default) */
    uint32_t     inactive_text;    /* inactive tab text color (0 = default) */
    float        tab_padding_x;    /* per-tab horizontal padding in logical px (0 = default 8) */
    bool         tabs_fill;        /* true: tabs flex-grow to fill available row space */
    bool         tabs_left_align;  /* true: left-align tab labels (false = centered) */
    bool         hidden;
    bool         disabled;
} Ca_TabBarDesc;

typedef struct Ca_TreeNodeDesc {
    const char      *text;
    bool             expanded;
    Ca_TreeToggleFn  on_toggle;
    void            *toggle_data;
    const char      *id, *style;
    bool             hidden;
    bool             is_leaf;        /* suppress disclosure triangle */
    const char      *icon;           /* UTF-8 icon string, e.g. CA_ICON_* */
    uint32_t         icon_color;     /* packed RGBA for icon (0 = text_color) */
    /* Additional class(es) appended to the built-in "tree-row" class on
       the inner clickable header row. Lets callers attach state-aware
       styling (e.g. selected/highlighted) directly to the header where
       :hover / :focus actually fire, instead of the container. */
    const char      *row_style;
} Ca_TreeNodeDesc;

typedef struct Ca_TableDesc {
    int          column_count;
    const float *column_widths;
    const char  *id, *style;
} Ca_TableDesc;

typedef struct Ca_TooltipDesc {
    const char *text;
    const char *id, *style;
} Ca_TooltipDesc;

typedef struct Ca_CtxMenuDesc {
    const char **items;
    int          item_count;
    Ca_MenuFn    on_select;
    void        *select_data;
    Ca_ContextMenuOpenFn on_open;
    void        *open_data;
    const char  *id, *style;
} Ca_CtxMenuDesc;

typedef struct Ca_ModalDesc {
    bool        visible;
    uint32_t    overlay_color;
    const char *id, *style;
} Ca_ModalDesc;

/* Menu bar item — a single clickable entry inside a dropdown menu. */
typedef struct Ca_MenuItemDesc {
    const char                   *label;
    Ca_MenuActionFn               action;
    void                         *action_data;
    bool                          separator;      /* render as a divider line    */
    const struct Ca_MenuItemDesc *sub_items;      /* nested items (or NULL)      */
    int                           sub_item_count;
} Ca_MenuItemDesc;

/* Menu — a labelled group of items that drops down from the bar. */
typedef struct Ca_MenuDesc {
    const char            *label;
    const Ca_MenuItemDesc *items;
    int                    item_count;
} Ca_MenuDesc;

/* Menu bar — horizontal strip of menus at the top of a container. */
typedef struct Ca_MenuBarDesc {
    const Ca_MenuDesc *menus;
    int                menu_count;
    const char        *id, *style;
    const char        *item_style;        /* CSS class for each menu header item (NULL = none) */
    uint32_t header_highlight;   /* active-header bg     (0 = default) */
    uint32_t dropdown_bg;        /* dropdown background  (0 = default) */
    uint32_t dropdown_border;    /* dropdown border      (0 = default) */
    uint32_t dropdown_hover;     /* item hover bg        (0 = default) */
    uint32_t dropdown_text;      /* dropdown item text   (0 = default) */
    uint32_t text_color;         /* header label text    (0 = default) */
    /* Inline fallbacks — used when style/item_style CSS classes are absent */
    float    bar_height;         /* outer bar fixed height (0 = CSS/auto)  */
    float    item_padding_lr;    /* per-side padding for each header item   */
    float    item_font_size;     /* font size for header item labels        */
} Ca_MenuBarDesc;

/* ============================================================
   COMPONENT API
   ============================================================ */

/* ---- Checkbox ---- */

/* Emit a checkbox widget; returns the created Ca_Checkbox handle. */
CA_API Ca_Checkbox *ca_checkbox(const Ca_CheckboxDesc *desc);

/* Programmatically set the checked state of a checkbox. */
CA_API void         ca_checkbox_set(Ca_Checkbox *cb, bool checked);

/* Returns the current checked state of a checkbox. */
CA_API bool         ca_checkbox_get(const Ca_Checkbox *cb);

/* ---- Radio button ---- */

/* Emit a radio button widget; returns the created Ca_Radio handle. */
CA_API Ca_Radio    *ca_radio(const Ca_RadioDesc *desc);

/*
 * Return the value of the currently selected radio button in a group.
 *
 * win    Window owning the radio group.
 * group  Group identifier passed in Ca_RadioDesc.group.
 * Returns  Selected value, or -1 if nothing is selected.
 */
CA_API int          ca_radio_group_get(Ca_Window *win, int group);

/* ---- Slider ---- */

/* Emit a slider widget; returns the created Ca_Slider handle. */
CA_API Ca_Slider   *ca_slider(const Ca_SliderDesc *desc);

/* Programmatically set the slider value (clamped to [min, max]). */
CA_API void         ca_slider_set(Ca_Slider *s, float value);

/* Returns the current value of the slider. */
CA_API float        ca_slider_get(const Ca_Slider *s);

/* ---- Toggle switch ---- */

/* Emit a toggle switch widget; returns the created Ca_Toggle handle. */
CA_API Ca_Toggle   *ca_toggle(const Ca_ToggleDesc *desc);

/* Programmatically set the toggle on/off state. */
CA_API void         ca_toggle_set(Ca_Toggle *t, bool on);

/* Returns the current on/off state of the toggle. */
CA_API bool         ca_toggle_get(const Ca_Toggle *t);

/* ---- Progress bar ---- */

/* Emit a progress bar widget; returns the created Ca_Progress handle. */
CA_API Ca_Progress *ca_progress(const Ca_ProgressDesc *desc);

/* Update the progress bar fill value (clamped to [0.0, 1.0]). */
CA_API void         ca_progress_set(Ca_Progress *p, float value);

/* ---- Select / dropdown ---- */

/* Emit a select/dropdown widget; returns the created Ca_Select handle. */
CA_API Ca_Select   *ca_select(const Ca_SelectDesc *desc);

/* Programmatically set the selected option by index. */
CA_API void         ca_select_set(Ca_Select *s, int index);

/* Returns the index of the currently selected option. */
CA_API int          ca_select_get(const Ca_Select *s);

/* ---- Tab bar ---- */

/* Emit a tab bar widget; returns the created Ca_TabBar handle. */
CA_API Ca_TabBar   *ca_tabs(const Ca_TabBarDesc *desc);

/* Returns the zero-based index of the currently active tab. */
CA_API int          ca_tabs_active(const Ca_TabBar *t);

/* ---- Tree view ---- */

/* Open a tree view container; must be paired with ca_tree_end(). */
CA_API void         ca_tree_begin(const Ca_DivDesc *desc);

/* Close a tree view container opened by ca_tree_begin(). */
CA_API void         ca_tree_end(void);

/*
 * Open a tree node row inside the current tree view.
 *
 * desc     Node label, expand state, icon, and toggle callback.
 * Returns  Handle to the created Ca_TreeNode.
 */
CA_API Ca_TreeNode *ca_tree_node_begin(const Ca_TreeNodeDesc *desc);

/* Close a tree node opened by ca_tree_node_begin(). */
CA_API void         ca_tree_node_end(void);

/* Returns true if the tree node is currently expanded. */
CA_API bool         ca_tree_node_expanded(const Ca_TreeNode *n);

/* Programmatically expand or collapse a tree node. */
CA_API void         ca_tree_node_set_expanded(Ca_TreeNode *n, bool expanded);

/* ---- Table ---- */

/* Open a table container; must be paired with ca_table_end(). */
CA_API void ca_table_begin(const Ca_TableDesc *desc);

/* Close a table container opened by ca_table_begin(). */
CA_API void ca_table_end(void);

/* Open a table row; must be paired with ca_table_row_end(). */
CA_API void ca_table_row_begin(const Ca_DivDesc *desc);

/* Close a table row opened by ca_table_row_begin(). */
CA_API void ca_table_row_end(void);

/* Emit a single table cell with the given text descriptor. */
CA_API void ca_table_cell(const Ca_TextDesc *desc);

/* ---- Tooltip ---- */

/*
 * Attach a tooltip to the previously created element.
 *
 * desc     Tooltip text and optional id/style.
 * Returns  Handle to the created Ca_Tooltip.
 */
CA_API Ca_Tooltip *ca_tooltip(const Ca_TooltipDesc *desc);

/*
 * Update the text of an existing tooltip without rebuilding its target widget.
 *
 * tooltip  Target Ca_Tooltip.
 * text     New tooltip text.
 */
CA_API void ca_tooltip_set_text(Ca_Tooltip *tooltip, const char *text);

/* ---- Context menu ---- */

/*
 * Attach a context menu to the previously created element.
 *
 * desc  Item list, selection callback, and open callback.
 */
CA_API void ca_context_menu(const Ca_CtxMenuDesc *desc);

/* ---- Menu bar ---- */

/*
 * Emit a horizontal menu bar widget.
 *
 * desc     Menu definitions and appearance overrides.
 * Returns  Handle to the created Ca_MenuBar.
 */
CA_API Ca_MenuBar *ca_menu_bar(const Ca_MenuBarDesc *desc);

/* ---- Modal / dialog ---- */

/*
 * Open a modal overlay container; must be paired with ca_modal_end().
 *
 * desc     Initial visibility and overlay color.
 * Returns  Handle to the created Ca_Modal.
 */
CA_API Ca_Modal *ca_modal_begin(const Ca_ModalDesc *desc);

/* Close a modal container opened by ca_modal_begin(). */
CA_API void      ca_modal_end(void);

/* Show or hide an existing modal overlay. */
CA_API void      ca_modal_set_visible(Ca_Modal *modal, bool visible);

/* Node graph canvas (see ca_node_graph.h for full documentation) */
#include "ca_node_graph.h"

#ifdef __cplusplus
}
#endif
