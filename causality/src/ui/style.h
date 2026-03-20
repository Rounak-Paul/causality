/* style.h — CSS style resolution and cascade */
#pragma once

#include "css.h"
#include "ca_internal.h"

/* ============================================================
   ELEMENT TYPES — for CSS selector matching
   ============================================================ */

typedef enum {
    CA_ELEM_DIV = 0,
    CA_ELEM_TEXT,
    CA_ELEM_BUTTON,
    CA_ELEM_INPUT,
    CA_ELEM_H1, CA_ELEM_H2, CA_ELEM_H3,
    CA_ELEM_H4, CA_ELEM_H5, CA_ELEM_H6,
    CA_ELEM_HR,
    CA_ELEM_SPACER,
    CA_ELEM_LIST,
    CA_ELEM_LI,
    CA_ELEM_CHECKBOX,
    CA_ELEM_RADIO,
    CA_ELEM_SLIDER,
    CA_ELEM_TOGGLE,
    CA_ELEM_PROGRESS,
    CA_ELEM_SELECT,
    CA_ELEM_TABBAR,
    CA_ELEM_TAB,
    CA_ELEM_TREE,
    CA_ELEM_TREENODE,
    CA_ELEM_TABLE,
    CA_ELEM_TABLE_ROW,
    CA_ELEM_TABLE_CELL,
    CA_ELEM_TOOLTIP,
    CA_ELEM_CTXMENU,
    CA_ELEM_MODAL,
    CA_ELEM_COUNT
} Ca_ElementType;

/* ============================================================
   OVERFLOW
   ============================================================ */

typedef enum {
    CA_OVERFLOW_VISIBLE = 0,
    CA_OVERFLOW_HIDDEN,
    CA_OVERFLOW_SCROLL,
    CA_OVERFLOW_AUTO,
} Ca_Overflow;

/* ============================================================
   RESOLVED STYLE
   ============================================================ */

typedef struct {
    uint64_t set_mask;   /* bitmask of which CA_CSS_PROP_* were set by CSS */

    float    width, height;
    float    min_width, max_width, min_height, max_height;
    float    padding[4];     /* top, right, bottom, left */
    float    margin[4];
    float    gap;
    float    border_radius;
    float    opacity;
    float    font_size;
    float    flex_grow, flex_shrink;

    uint32_t background_color;
    uint32_t color;

    int      display;         /* Ca_CssKeyword display value */
    int      flex_direction;  /* Ca_CssKeyword flex direction */
    int      flex_wrap;
    int      align_items;
    int      justify_content;
    int      overflow_x, overflow_y;  /* Ca_Overflow */
    int      text_align;              /* Ca_CssKeyword text-align */

    /* Transition */
    float    transition_duration;     /* seconds */
    uint64_t transition_props;        /* bitmask of Ca_CssPropId to animate */
} Ca_ResolvedStyle;

/* ============================================================
   API
   ============================================================ */

/* Get element type name string for CSS selector matching */
const char *ca_elem_type_name(Ca_ElementType type);

/* Resolve all matching CSS rules for a node, producing a merged style.
   walks the parent chain for descendant/child selector matching.  */
void ca_style_resolve(Ca_Stylesheet *ss,
                      Ca_Node *node,
                      Ca_ElementType elem_type,
                      const char *classes,
                      Ca_ResolvedStyle *out);

/* Apply resolved style to a Ca_NodeDesc.
   Only fills in properties where the NodeDesc value is still at default (0).
   Non-zero NodeDesc values are treated as inline styles and take precedence. */
void ca_style_apply_to_node(const Ca_ResolvedStyle *style,
                            Ca_NodeDesc *nd,
                            uint32_t *out_color);
