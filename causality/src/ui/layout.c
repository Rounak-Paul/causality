/* layout.c — flexbox-inspired single-axis layout with full flex support */
#include "layout.h"
#include "css.h"

/* Estimate a node's natural size along a given axis (true=height, false=width).
   Used for auto-sizing children that have no explicit size and no flex-grow. */
static float content_size(Ca_Node *node, bool want_height)
{
    if (node->desc.hidden) return 0.0f;

    float sz = want_height ? node->desc.height : node->desc.width;
    if (sz > 0.0f) return sz;

    /* Leaf: height ≈ line-height, width = 0 (unknown, distribute remaining) */
    if (node->child_count == 0)
        return want_height ? 20.0f : 0.0f;

    /* Container: compute from children */
    bool is_row = (node->desc.direction == CA_DIR_ROW);
    float pad = want_height
        ? (node->desc.padding_top + node->desc.padding_bottom)
        : (node->desc.padding_left + node->desc.padding_right);

    /*  Column + want_height  → sum child heights
        Row    + want_height  → max child heights
        Row    + want_width   → sum child widths
        Column + want_width   → max child widths */
    bool summing = (want_height && !is_row) || (!want_height && is_row);

    float total = 0.0f, max_val = 0.0f;
    uint32_t vis = 0;
    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *c = node->children[i];
        if (c->desc.hidden) continue;
        float csz = content_size(c, want_height);
        if (summing) total += csz; else if (csz > max_val) max_val = csz;
        vis++;
    }

    if (summing) {
        float gaps = (vis > 1) ? node->desc.gap * (float)(vis - 1) : 0.0f;
        return pad + total + gaps;
    }
    return pad + max_val;
}

/* Compute the laid-out rect of a node given the available space.
   Recurses into children.  Supports:
     - flex-grow / flex-shrink
     - align-items (start, center, end, stretch)
     - justify-content (start, center, end, space-between, space-around, space-evenly)
     - display: none  (hidden flag)
     - overflow: scroll (stores content_w/content_h for scroll range)     */
static void layout_node(Ca_Node *node, float x, float y, float avail_w, float avail_h)
{
    /* display: none — zero size, skip children */
    if (node->desc.hidden) {
        node->x = x; node->y = y;
        node->w = 0; node->h = 0;
        node->dirty &= ~(CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN);
        return;
    }

    /* Determine this node's own size */
    float w = (node->desc.width  > 0.0f) ? node->desc.width  : avail_w;
    float h = (node->desc.height > 0.0f) ? node->desc.height : avail_h;

    if (node->desc.min_w > 0.0f && w < node->desc.min_w) w = node->desc.min_w;
    if (node->desc.max_w > 0.0f && w > node->desc.max_w) w = node->desc.max_w;
    if (node->desc.min_h > 0.0f && h < node->desc.min_h) h = node->desc.min_h;
    if (node->desc.max_h > 0.0f && h > node->desc.max_h) h = node->desc.max_h;

    node->x = x;
    node->y = y;
    node->w = w;
    node->h = h;
    node->dirty &= ~(CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN);

    /* Count visible children */
    uint32_t visible_count = 0;
    for (uint32_t i = 0; i < node->child_count; ++i) {
        if (!node->children[i]->desc.hidden)
            visible_count++;
    }
    if (visible_count == 0) return;

    /* Inner area after padding */
    float inner_x = x + node->desc.padding_left;
    float inner_y = y + node->desc.padding_top;
    float inner_w = w - node->desc.padding_left - node->desc.padding_right;
    float inner_h = h - node->desc.padding_top  - node->desc.padding_bottom;
    if (inner_w < 0.0f) inner_w = 0.0f;
    if (inner_h < 0.0f) inner_h = 0.0f;

    bool  is_row      = (node->desc.direction == CA_DIR_ROW);
    float gap         = node->desc.gap;
    float avail_main  = is_row ? inner_w : inner_h;
    float avail_cross = is_row ? inner_h : inner_w;

    /* Pass 1: measure children — compute total fixed main-axis size,
       total flex-grow, and total flex-shrink */
    float total_gaps = (visible_count > 1) ? gap * (float)(visible_count - 1) : 0.0f;
    float total_fixed = total_gaps;
    float total_grow  = 0.0f;
    float total_shrink = 0.0f;

    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (child->desc.hidden) continue;

        float main_size = is_row ? child->desc.width : child->desc.height;
        if (main_size > 0.0f) {
            total_fixed += main_size;
        } else if (child->desc.flex_grow > 0.0f) {
            /* Explicit flex-grow: will absorb remaining space */
            total_grow += child->desc.flex_grow;
        } else {
            /* Auto-sized without flex-grow: use content-based natural size.
               If content_size returns 0 (unknown), treat as implicit grow. */
            float natural = content_size(child, !is_row);
            if (natural > 0.0f)
                total_fixed += natural;
            else
                total_grow += 1.0f;
        }
        total_shrink += (child->desc.flex_shrink > 0.0f) ? child->desc.flex_shrink : 1.0f;
    }

    float remaining = avail_main - total_fixed;
    if (remaining < 0.0f) remaining = 0.0f;

    /* Pass 2: compute each child's main and cross sizes */
    float cursor = 0.0f;
    float total_main_used = 0.0f;  /* actual total main size for content bounds */

    /* Temporary child sizes for justify-content */
    float child_main[CA_MAX_NODE_CHILDREN];
    float child_cross[CA_MAX_NODE_CHILDREN];
    int   vis_idx = 0;

    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (child->desc.hidden) {
            child_main[i] = 0;
            child_cross[i] = 0;
            continue;
        }

        float cm = is_row ? child->desc.width  : child->desc.height;
        float cc = is_row ? child->desc.height : child->desc.width;

        if (cm <= 0.0f) {
            if (child->desc.flex_grow > 0.0f) {
                /* Flex-grow: distribute remaining space proportionally */
                cm = (total_grow > 0.0f) ? remaining * child->desc.flex_grow / total_grow : 0.0f;
            } else {
                /* Auto: use content size, or distribute remaining if unknown */
                float natural = content_size(child, !is_row);
                if (natural > 0.0f)
                    cm = natural;
                else
                    cm = (total_grow > 0.0f) ? remaining * 1.0f / total_grow : 0.0f;
            }
        }

        /* Cross-axis: stretch fills available, otherwise use child's own size */
        if (cc <= 0.0f) {
            cc = avail_cross;
        }

        child_main[i]  = cm;
        child_cross[i] = cc;
        total_main_used += cm;
        vis_idx++;
    }

    /* justify-content offset and spacing */
    float jc_offset  = 0.0f;
    float jc_spacing = gap;

    float total_children_main = total_main_used + total_gaps;
    float free_space = avail_main - total_children_main;
    if (free_space < 0.0f) free_space = 0.0f;

    Ca_Align jc = node->desc.justify_content;
    if (jc == CA_ALIGN_CENTER) {
        jc_offset = free_space * 0.5f;
    } else if (jc == CA_ALIGN_END) {
        jc_offset = free_space;
    } else if ((int)jc == CA_CSS_ALIGN_SPACE_BETWEEN && visible_count > 1) {
        jc_spacing = gap + free_space / (float)(visible_count - 1);
    } else if ((int)jc == CA_CSS_ALIGN_SPACE_AROUND && visible_count > 0) {
        float s = free_space / (float)(visible_count);
        jc_offset = s * 0.5f;
        jc_spacing = gap + s;
    } else if ((int)jc == CA_CSS_ALIGN_SPACE_EVENLY && visible_count > 0) {
        float s = free_space / (float)(visible_count + 1);
        jc_offset = s;
        jc_spacing = gap + s;
    }

    /* Apply scroll offset */
    float scroll_off_x = node->scroll_x;
    float scroll_off_y = node->scroll_y;

    /* Pass 3: position children */
    cursor = jc_offset;
    float content_extent = 0.0f;

    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (child->desc.hidden) {
            layout_node(child, 0, 0, 0, 0);
            continue;
        }

        float cm = child_main[i];
        float cc = child_cross[i];

        /* Align-items: cross-axis positioning */
        float cross_offset = 0.0f;
        Ca_Align align = node->desc.align_items;
        if (align == CA_ALIGN_CENTER) {
            cross_offset = (avail_cross - cc) * 0.5f;
        } else if (align == CA_ALIGN_END) {
            cross_offset = avail_cross - cc;
        }
        /* START and STRETCH: cross_offset = 0 */

        float cx, cy, cw, ch;
        if (is_row) {
            cx = inner_x + cursor - scroll_off_x;
            cy = inner_y + cross_offset - scroll_off_y;
            cw = cm;  ch = cc;
        } else {
            cx = inner_x + cross_offset - scroll_off_x;
            cy = inner_y + cursor - scroll_off_y;
            cw = cc;  ch = cm;
        }

        layout_node(child, cx, cy, cw, ch);

        float end = cursor + cm;
        if (end > content_extent) content_extent = end;
        cursor += cm + jc_spacing;
    }

    /* Store content size for scroll range computation */
    if (is_row) {
        node->content_w = content_extent;
        node->content_h = avail_cross;
    } else {
        node->content_w = avail_cross;
        node->content_h = content_extent;
    }
}

void ca_layout_pass(Ca_Window *win)
{
    if (!win->root) return;

    /* Use logical (window) size so widget coordinates stay in the same
       pixel space as user-specified sizes (padding, width, height).
       On Retina / HiDPI, glfwGetFramebufferSize returns 2× logical size
       which would make every widget appear half-sized. */
    int lw, lh;
    glfwGetWindowSize(win->glfw, &lw, &lh);
    layout_node(win->root, 0.0f, 0.0f, (float)lw, (float)lh);
}
