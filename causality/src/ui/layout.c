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
    bool is_row  = (node->desc.direction == CA_DIR_ROW);
    bool do_wrap = (node->desc.flex_wrap == 1);
    float pad = want_height
        ? (node->desc.padding_top + node->desc.padding_bottom)
        : (node->desc.padding_left + node->desc.padding_right);
    float gap = node->desc.gap;

    /* For a wrapping row container, asking for height requires simulating
       line breaks to find how many lines there are. */
    if (do_wrap && is_row && want_height) {
        float avail_w = node->desc.width;
        if (avail_w <= 0.0f) avail_w = 9999.0f; /* will be constrained by parent later */
        float inner_w = avail_w - node->desc.padding_left - node->desc.padding_right;
        if (inner_w < 0.0f) inner_w = 0.0f;

        float line_used = 0;
        uint32_t line_vis = 0;
        float line_cross = 0, total_cross = 0;
        uint32_t line_count = 0;

        for (uint32_t i = 0; i < node->child_count; ++i) {
            Ca_Node *c = node->children[i];
            if (c->desc.hidden) continue;
            float cw = c->desc.width > 0.0f ? c->desc.width : content_size(c, false);
            if (cw <= 0.0f) cw = 20.0f;
            float ch = c->desc.height > 0.0f ? c->desc.height : content_size(c, true);
            if (ch <= 0.0f) ch = 20.0f;

            float added = cw + (line_vis > 0 ? gap : 0);
            if (line_vis > 0 && line_used + added > inner_w) {
                total_cross += line_cross + (line_count > 0 ? gap : 0);
                line_count++;
                line_used = 0; line_vis = 0; line_cross = 0;
            }
            line_used += cw + (line_vis > 0 ? gap : 0);
            if (ch > line_cross) line_cross = ch;
            line_vis++;
        }
        if (line_vis > 0)
            total_cross += line_cross + (line_count > 0 ? gap : 0);

        return pad + total_cross;
    }

    /* For a wrapping column container, asking for width: similar logic */
    if (do_wrap && !is_row && !want_height) {
        float avail_h = node->desc.height;
        if (avail_h <= 0.0f) avail_h = 9999.0f;
        float inner_h = avail_h - node->desc.padding_top - node->desc.padding_bottom;
        if (inner_h < 0.0f) inner_h = 0.0f;

        float line_used = 0;
        uint32_t line_vis = 0;
        float line_cross = 0, total_cross = 0;
        uint32_t line_count = 0;

        for (uint32_t i = 0; i < node->child_count; ++i) {
            Ca_Node *c = node->children[i];
            if (c->desc.hidden) continue;
            float ch = c->desc.height > 0.0f ? c->desc.height : content_size(c, true);
            if (ch <= 0.0f) ch = 20.0f;
            float cw = c->desc.width > 0.0f ? c->desc.width : content_size(c, false);
            if (cw <= 0.0f) cw = 20.0f;

            float added = ch + (line_vis > 0 ? gap : 0);
            if (line_vis > 0 && line_used + added > inner_h) {
                total_cross += line_cross + (line_count > 0 ? gap : 0);
                line_count++;
                line_used = 0; line_vis = 0; line_cross = 0;
            }
            line_used += ch + (line_vis > 0 ? gap : 0);
            if (cw > line_cross) line_cross = cw;
            line_vis++;
        }
        if (line_vis > 0)
            total_cross += line_cross + (line_count > 0 ? gap : 0);

        return pad + total_cross;
    }

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
        float gaps = (vis > 1) ? gap * (float)(vis - 1) : 0.0f;
        return pad + total + gaps;
    }
    return pad + max_val;
}

/* Compute the laid-out rect of a node given the available space.
   Recurses into children.  Supports:
     - flex-grow / flex-shrink
     - flex-wrap (wrap children to next line when main axis overflows)
     - align-items (start, center, end, stretch)
     - justify-content (start, center, end, space-between, space-around, space-evenly)
     - display: none  (hidden flag)
     - overflow: scroll (stores content_w/content_h for scroll range)     */

/* A flex line is a group of children that fit on one row/column in wrap mode. */
typedef struct {
    uint32_t start;          /* first child index in this line */
    uint32_t count;          /* number of visible children */
    float    total_fixed;    /* total fixed main-axis size + gaps */
    float    total_grow;     /* sum of flex-grow values */
    float    cross_size;     /* max cross-axis size among children in this line */
} FlexLine;

#define MAX_FLEX_LINES 64

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
    bool auto_h = (node->desc.height <= 0.0f);
    bool auto_w = (node->desc.width  <= 0.0f);

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
    bool  do_wrap     = (node->desc.flex_wrap == 1);
    float gap         = node->desc.gap;
    float avail_main  = is_row ? inner_w : inner_h;
    float avail_cross = is_row ? inner_h : inner_w;

    /* Pre-compute each child's hypothetical main-axis size */
    float child_hypo_main[CA_MAX_NODE_CHILDREN];
    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (child->desc.hidden) { child_hypo_main[i] = 0; continue; }
        float ms = is_row ? child->desc.width : child->desc.height;
        if (ms > 0.0f) {
            child_hypo_main[i] = ms;
        } else {
            float nat = content_size(child, !is_row);
            child_hypo_main[i] = nat > 0.0f ? nat : 0.0f;
        }
    }

    /* Build flex lines */
    FlexLine lines[MAX_FLEX_LINES];
    uint32_t line_count = 0;

    if (!do_wrap) {
        /* Single line — all children */
        FlexLine *ln = &lines[0];
        ln->start = 0;
        ln->count = 0;
        ln->total_fixed = 0;
        ln->total_grow = 0;
        ln->cross_size = avail_cross;

        uint32_t vis = 0;
        for (uint32_t i = 0; i < node->child_count; ++i) {
            Ca_Node *child = node->children[i];
            if (child->desc.hidden) continue;
            vis++;
            float ms = child_hypo_main[i];
            if (ms > 0.0f)
                ln->total_fixed += ms;
            else if (child->desc.flex_grow > 0.0f)
                ln->total_grow += child->desc.flex_grow;
            else
                ln->total_grow += 1.0f; /* implicit grow */
        }
        ln->count = vis;
        ln->total_fixed += (vis > 1) ? gap * (float)(vis - 1) : 0;
        line_count = 1;
    } else {
        /* Wrap mode — break into lines when main-axis overflows */
        float line_used = 0;
        uint32_t line_vis = 0;
        FlexLine *ln = &lines[0];
        ln->start = 0; ln->count = 0;
        ln->total_fixed = 0; ln->total_grow = 0; ln->cross_size = 0;

        for (uint32_t i = 0; i < node->child_count; ++i) {
            Ca_Node *child = node->children[i];
            if (child->desc.hidden) continue;

            float ms = child_hypo_main[i];
            if (ms <= 0.0f) ms = 20.0f; /* minimum for wrapping purposes */

            float added = ms + (line_vis > 0 ? gap : 0);
            if (line_vis > 0 && line_used + added > avail_main && line_count + 1 < MAX_FLEX_LINES) {
                /* Finish current line, start a new one */
                ln->count = line_vis;
                ln->total_fixed += (line_vis > 1) ? gap * (float)(line_vis - 1) : 0;
                line_count++;
                ln = &lines[line_count];
                ln->start = i; ln->count = 0;
                ln->total_fixed = 0; ln->total_grow = 0; ln->cross_size = 0;
                line_used = 0; line_vis = 0;
            }

            line_used += ms + (line_vis > 0 ? gap : 0);
            line_vis++;

            float cs_val = is_row ? child->desc.height : child->desc.width;
            if (cs_val <= 0) cs_val = content_size(child, is_row);
            if (cs_val <= 0) cs_val = 20.0f;
            if (cs_val > ln->cross_size) ln->cross_size = cs_val;

            float hmain = child_hypo_main[i];
            if (hmain > 0.0f)
                ln->total_fixed += hmain;
            else if (child->desc.flex_grow > 0.0f)
                ln->total_grow += child->desc.flex_grow;
            else
                ln->total_grow += 1.0f;
        }
        /* Finish last line */
        if (line_vis > 0) {
            ln->count = line_vis;
            ln->total_fixed += (line_vis > 1) ? gap * (float)(line_vis - 1) : 0;
            line_count++;
        }
    }

    /* Apply scroll offset */
    float scroll_off_x = node->scroll_x;
    float scroll_off_y = node->scroll_y;

    /* Now lay out each line */
    float cross_cursor = 0;
    float max_main_extent = 0;
    float max_cross_extent = 0;
    uint32_t child_idx = 0;

    for (uint32_t li = 0; li < line_count; ++li) {
        FlexLine *ln = &lines[li];
        float line_avail_main = avail_main;
        float line_avail_cross = do_wrap ? ln->cross_size : avail_cross;
        float remaining = line_avail_main - ln->total_fixed;
        if (remaining < 0) remaining = 0;

        /* Compute child main/cross sizes for this line */
        float cm_arr[CA_MAX_NODE_CHILDREN];
        float cc_arr[CA_MAX_NODE_CHILDREN];
        float total_main_used = 0;
        uint32_t vis_in_line = 0;
        uint32_t line_child_start = child_idx;

        for (uint32_t i = child_idx; i < node->child_count && vis_in_line < ln->count; ++i) {
            Ca_Node *child = node->children[i];
            if (child->desc.hidden) { cm_arr[i] = 0; cc_arr[i] = 0; continue; }

            float cm = child_hypo_main[i];
            float cc = is_row ? child->desc.height : child->desc.width;

            if (cm <= 0.0f) {
                if (child->desc.flex_grow > 0.0f)
                    cm = (ln->total_grow > 0) ? remaining * child->desc.flex_grow / ln->total_grow : 0;
                else
                    cm = (ln->total_grow > 0) ? remaining * 1.0f / ln->total_grow : 0;
            }
            if (cc <= 0.0f) cc = line_avail_cross;

            cm_arr[i] = cm;
            cc_arr[i] = cc;
            total_main_used += cm;
            vis_in_line++;
            child_idx = i + 1;
        }

        /* justify-content for this line */
        float jc_offset = 0;
        float jc_spacing = gap;
        float total_gaps = (vis_in_line > 1) ? gap * (float)(vis_in_line - 1) : 0;
        float total_children_main = total_main_used + total_gaps;
        float free_space = line_avail_main - total_children_main;
        if (free_space < 0) free_space = 0;

        Ca_Align jc = node->desc.justify_content;
        if (jc == CA_ALIGN_CENTER) {
            jc_offset = free_space * 0.5f;
        } else if (jc == CA_ALIGN_END) {
            jc_offset = free_space;
        } else if ((int)jc == CA_CSS_ALIGN_SPACE_BETWEEN && vis_in_line > 1) {
            jc_spacing = gap + free_space / (float)(vis_in_line - 1);
        } else if ((int)jc == CA_CSS_ALIGN_SPACE_AROUND && vis_in_line > 0) {
            float s = free_space / (float)(vis_in_line);
            jc_offset = s * 0.5f;
            jc_spacing = gap + s;
        } else if ((int)jc == CA_CSS_ALIGN_SPACE_EVENLY && vis_in_line > 0) {
            float s = free_space / (float)(vis_in_line + 1);
            jc_offset = s;
            jc_spacing = gap + s;
        }

        /* Position children in this line */
        float main_cursor = jc_offset;

        vis_in_line = 0;
        for (uint32_t i = line_child_start; i < node->child_count && vis_in_line < ln->count; ++i) {
            Ca_Node *child = node->children[i];
            if (child->desc.hidden) {
                layout_node(child, 0, 0, 0, 0);
                continue;
            }

            float cm = cm_arr[i];
            float cc = cc_arr[i];

            /* Align-items: cross-axis positioning within the line */
            float cross_offset = 0;
            Ca_Align align = node->desc.align_items;
            if (align == CA_ALIGN_CENTER)
                cross_offset = (line_avail_cross - cc) * 0.5f;
            else if (align == CA_ALIGN_END)
                cross_offset = line_avail_cross - cc;

            float cx, cy, cw, ch;
            if (is_row) {
                cx = inner_x + main_cursor - scroll_off_x;
                cy = inner_y + cross_cursor + cross_offset - scroll_off_y;
                cw = cm;  ch = cc;
            } else {
                cx = inner_x + cross_cursor + cross_offset - scroll_off_x;
                cy = inner_y + main_cursor - scroll_off_y;
                cw = cc;  ch = cm;
            }

            layout_node(child, cx, cy, cw, ch);

            /* Use the child's actual laid-out main-axis size for positioning.
               This is critical: if the child grew (e.g. flex-wrap creating
               multiple lines), we must respect its real size, not the
               hypothetical pre-computed size. */
            float actual_main = is_row ? child->w : child->h;
            float end = main_cursor + actual_main;
            if (end > max_main_extent) max_main_extent = end;
            main_cursor += actual_main + jc_spacing;
            vis_in_line++;
        }

        cross_cursor += line_avail_cross + gap;
        float cross_end = cross_cursor - gap;
        if (cross_end > max_cross_extent) max_cross_extent = cross_end;
    }

    /* Handle hidden children that were skipped entirely */
    for (uint32_t i = child_idx; i < node->child_count; ++i) {
        if (node->children[i]->desc.hidden)
            layout_node(node->children[i], 0, 0, 0, 0);
    }

    /* Store content size for scroll range computation.
       Include padding so content_h is comparable to node->h (which also includes padding). */
    float pad_main  = is_row ? (node->desc.padding_left + node->desc.padding_right)
                             : (node->desc.padding_top  + node->desc.padding_bottom);
    float pad_cross = is_row ? (node->desc.padding_top  + node->desc.padding_bottom)
                             : (node->desc.padding_left + node->desc.padding_right);
    if (is_row) {
        node->content_w = max_main_extent + pad_main;
        node->content_h = (do_wrap ? max_cross_extent : avail_cross) + pad_cross;
    } else {
        node->content_w = (do_wrap ? max_cross_extent : avail_cross) + pad_cross;
        node->content_h = max_main_extent + pad_main;
    }

    /* Post-layout auto-sizing: shrink the node to fit its actual content
       when no explicit size was set.  Scroll containers keep their size
       so overflow can scroll.  This is the key to making flex-wrap and
       other content-driven containers report correct sizes to their parent. */
    if (auto_h && node->child_count > 0 && node->desc.overflow_y < 2) {
        if (node->content_h > 0.0f)
            node->h = node->content_h;
    }
    if (auto_w && node->child_count > 0 && node->desc.overflow_x < 2) {
        if (node->content_w > 0.0f)
            node->w = node->content_w;
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
