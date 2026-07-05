// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_resolved_style.h — Ca_ResolvedStyle: the merged output of CSS cascade
   resolution for one node. Deliberately dependency-free (only stdint/
   stdbool) so it can be included from both style.h (the resolver) and
   ca_internal.h (Ca_Node's per-node resolved-style cache) without a
   circular include — style.h already depends on ca_internal.h for Ca_Node. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

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
    /* Backdrop filter */
    float    backdrop_blur;
} Ca_ResolvedStyle;
