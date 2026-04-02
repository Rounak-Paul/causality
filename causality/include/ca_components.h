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

typedef void (*Ca_CheckFn)(Ca_Checkbox *cb, void *user_data);
typedef void (*Ca_SliderFn)(Ca_Slider *slider, void *user_data);
typedef void (*Ca_ToggleFn)(Ca_Toggle *toggle, void *user_data);
typedef void (*Ca_SelectFn)(Ca_Select *sel, void *user_data);
typedef void (*Ca_TabFn)(Ca_TabBar *tabs, void *user_data);
typedef void (*Ca_TreeToggleFn)(Ca_TreeNode *tn, void *user_data);
typedef void (*Ca_MenuFn)(int item_index, void *user_data);
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
    const char **options;
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
    const char      *icon;           /* UTF-8 icon string (single glyph) */
    uint32_t         icon_color;     /* packed RGBA for icon (0 = text_color) */
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

/* Checkbox */
CA_API Ca_Checkbox *ca_checkbox(const Ca_CheckboxDesc *desc);
CA_API void         ca_checkbox_set(Ca_Checkbox *cb, bool checked);
CA_API bool         ca_checkbox_get(const Ca_Checkbox *cb);

/* Radio button */
CA_API Ca_Radio    *ca_radio(const Ca_RadioDesc *desc);
CA_API int          ca_radio_group_get(Ca_Window *win, int group);

/* Slider */
CA_API Ca_Slider   *ca_slider(const Ca_SliderDesc *desc);
CA_API void         ca_slider_set(Ca_Slider *s, float value);
CA_API float        ca_slider_get(const Ca_Slider *s);

/* Toggle switch */
CA_API Ca_Toggle   *ca_toggle(const Ca_ToggleDesc *desc);
CA_API void         ca_toggle_set(Ca_Toggle *t, bool on);
CA_API bool         ca_toggle_get(const Ca_Toggle *t);

/* Progress bar */
CA_API Ca_Progress *ca_progress(const Ca_ProgressDesc *desc);
CA_API void         ca_progress_set(Ca_Progress *p, float value);

/* Select / dropdown */
CA_API Ca_Select   *ca_select(const Ca_SelectDesc *desc);
CA_API void         ca_select_set(Ca_Select *s, int index);
CA_API int          ca_select_get(const Ca_Select *s);

/* Tab bar */
CA_API Ca_TabBar   *ca_tabs(const Ca_TabBarDesc *desc);
CA_API int          ca_tabs_active(const Ca_TabBar *t);

/* Tree view */
CA_API void         ca_tree_begin(const Ca_DivDesc *desc);
CA_API void         ca_tree_end(void);
CA_API Ca_TreeNode *ca_tree_node_begin(const Ca_TreeNodeDesc *desc);
CA_API void         ca_tree_node_end(void);
CA_API bool         ca_tree_node_expanded(const Ca_TreeNode *n);

/* Table */
CA_API void ca_table_begin(const Ca_TableDesc *desc);
CA_API void ca_table_end(void);
CA_API void ca_table_row_begin(const Ca_DivDesc *desc);
CA_API void ca_table_row_end(void);
CA_API void ca_table_cell(const Ca_TextDesc *desc);

/* Tooltip — attach to the previously created element */
CA_API void ca_tooltip(const Ca_TooltipDesc *desc);

/* Context menu — attach to the previously created element */
CA_API void ca_context_menu(const Ca_CtxMenuDesc *desc);

/* Menu bar */
CA_API Ca_MenuBar *ca_menu_bar(const Ca_MenuBarDesc *desc);

/* Modal / dialog */
CA_API Ca_Modal *ca_modal_begin(const Ca_ModalDesc *desc);
CA_API void      ca_modal_end(void);
CA_API void      ca_modal_set_visible(Ca_Modal *modal, bool visible);

#ifdef __cplusplus
}
#endif
