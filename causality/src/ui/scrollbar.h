// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

#include "../core/ca_internal.h"

/** Returns whether an overflow axis can display a scrollbar. */
static inline bool ca_scrollbar_axis_enabled(const Ca_Node *node, bool vertical)
{
    if (!node) return false;
    uint8_t overflow = vertical ? node->desc.overflow_y : node->desc.overflow_x;
    return overflow >= 2 &&
           (!node->desc.scrollbar_width_set || node->desc.scrollbar_width > 0.0f);
}

/** Returns the resolved vertical scrollbar width in logical pixels. */
static inline float ca_scrollbar_vertical_width(const Ca_Node *node)
{
    if (!node) return 0.0f;
    if (node->desc.scrollbar_width_set) return node->desc.scrollbar_width;
    float scale = node->window && node->window->ui_scale > 0.0f
        ? node->window->ui_scale : 1.0f;
    return 14.0f * scale;
}

/** Returns the resolved horizontal scrollbar height in logical pixels. */
static inline float ca_scrollbar_horizontal_height(const Ca_Node *node)
{
    if (!node) return 0.0f;
    if (node->desc.scrollbar_width_set) return node->desc.scrollbar_width;
    float scale = node->window && node->window->ui_scale > 0.0f
        ? node->window->ui_scale : 1.0f;
    return 6.0f * scale;
}

/** Returns the content viewport width after reserving the vertical gutter. */
static inline float ca_scrollbar_viewport_width(const Ca_Node *node)
{
    if (!node) return 0.0f;
    float width = node->w;
    if (node->scrollbar_y_visible)
        width -= ca_scrollbar_vertical_width(node);
    return width > 0.0f ? width : 0.0f;
}

/** Returns the content viewport height after reserving the horizontal gutter. */
static inline float ca_scrollbar_viewport_height(const Ca_Node *node)
{
    if (!node) return 0.0f;
    float height = node->h;
    if (node->scrollbar_x_visible)
        height -= ca_scrollbar_horizontal_height(node);
    return height > 0.0f ? height : 0.0f;
}

/** Returns the maximum horizontal scroll offset for the resolved viewport. */
static inline float ca_scrollbar_max_x(const Ca_Node *node)
{
    if (!node) return 0.0f;
    float maximum = node->content_w - ca_scrollbar_viewport_width(node);
    return maximum > 0.0f ? maximum : 0.0f;
}

/** Returns the maximum vertical scroll offset for the resolved viewport. */
static inline float ca_scrollbar_max_y(const Ca_Node *node)
{
    if (!node) return 0.0f;
    float maximum = node->content_h - ca_scrollbar_viewport_height(node);
    return maximum > 0.0f ? maximum : 0.0f;
}
