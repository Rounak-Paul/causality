// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

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
    CA_ELEM_SPLITTER,
    CA_ELEM_IMAGE,
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
    uint64_t set_mask;    /* bitmask of which CA_CSS_PROP_* (low 64) were set by CSS */
    uint64_t set_mask2;   /* bitmask for props >= 64 */

    /* Sizing */
    float    width, height;
    bool     width_pct, height_pct;   /* true when value is a percentage */
    float    min_width, max_width, min_height, max_height;
    /* Spacing */
    float    padding[4];     /* top, right, bottom, left */
    float    margin[4];
    float    gap, row_gap, column_gap;
    /* Visual */
    float    border_radius;
    float    border_radius_tl, border_radius_tr, border_radius_br, border_radius_bl;
    float    opacity;
    /* Typography */
    float    font_size;
    float    line_height;
    float    letter_spacing;
    float    word_spacing;
    bool     font_bold;
    /* Flex */
    float    flex_grow, flex_shrink;
    float    flex_basis;
    int      flex_order;
    /* Colors */
    uint32_t background_color;
    uint32_t color;
    /* Layout keywords */
    int      display;
    int      flex_direction;
    int      flex_wrap;
    int      align_items;
    int      align_self;
    int      align_content;
    int      justify_content;
    int      justify_self;
    int      overflow_x, overflow_y;
    int      text_align;
    int      text_decoration;
    int      text_transform;
    int      white_space;
    int      font_weight;
    int      font_style;
    int      visibility;
    int      cursor;
    int      pointer_events;
    int      user_select;
    int      scroll_behavior;
    int      box_sizing;
    /* Transition */
    float    transition_duration;
    uint64_t transition_props;
    int      transition_easing;
    /* Border — uniform */
    float    border_width;
    uint32_t border_color;
    int      border_style;
    /* Border — per-side (using submodule short-name convention) */
    float    border_top_w, border_right_w, border_bottom_w, border_left_w;
    uint32_t border_top_c, border_right_c, border_bottom_c, border_left_c;
    int      border_top_style, border_right_style, border_bottom_style, border_left_style;
    /* Outline */
    float    outline_width;
    uint32_t outline_color;
    int      outline_style;
    float    outline_offset;
    /* Box shadow */
    float    shadow_offset_x, shadow_offset_y;
    float    shadow_blur;
    uint32_t shadow_color;
    /* Z-index */
    int      z_index;
    /* Text wrapping */
    int      text_wrap;
    /* Aspect ratio */
    float    aspect_ratio;
    /* Gradient background */
    uint8_t  gradient_type;    /* CA_DRAW_MODE_LINEAR_GRAD / CA_DRAW_MODE_RADIAL_GRAD, 0=none */
    uint32_t gradient_color2;  /* end color stop (RRGGBBAA) */
    float    gradient_angle;   /* degrees for linear-gradient */
    float    gradient_cx, gradient_cy; /* radial center 0..1 */
    float    scrollbar_width;
    uint32_t scrollbar_track_color;
    uint32_t scrollbar_thumb_color;
    uint32_t scrollbar_thumb_active_color;
    float    scrollbar_radius;
} Ca_ResolvedStyle;

/* ============================================================
   API
   ============================================================ */

/** Get element type name string for CSS selector matching. */
const char *ca_elem_type_name(Ca_ElementType type);

/** Resolve all matching CSS rules for a node, producing a merged style.
    Walks the parent chain for descendant/child selector matching.
    @param ss        Parsed stylesheet (may be NULL — returns zero style).
    @param node      Target UI node.
    @param elem_type Element type enum for element-selector matching.
    @param classes   Space-separated class string for class-selector matching.
    @param out       Output resolved style (zeroed on entry). */
void ca_style_resolve(Ca_Stylesheet *ss,
                      Ca_Node *node,
                      Ca_ElementType elem_type,
                      const char *classes,
                      Ca_ResolvedStyle *out);

/** Apply resolved style to a Ca_NodeDesc.
    Only fills in properties where the NodeDesc value is still at default (0).
    Non-zero NodeDesc values are treated as inline styles and take precedence.
    @param style     Resolved style (from ca_style_resolve).
    @param nd        Target node descriptor to update in-place.
    @param out_color Optional output for the text/foreground color. */
void ca_style_apply_to_node(const Ca_ResolvedStyle *style,
                            Ca_NodeDesc *nd,
                            uint32_t *out_color);
