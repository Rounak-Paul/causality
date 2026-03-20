/* layout.c — flexbox-inspired single-axis layout with full flex support */
#include "layout.h"
#include "css.h"
#include "font.h"

/* Measure the height a wrapped text label will occupy given its laid-out width.
   Mirrors the first pass of paint_text_wrapped() in paint.c but returns early
   with just the line count × line height.  Returns 0 if not applicable. */
static float measure_wrapped_text_height(Ca_Node *node)
{
    if (node->widget_type != CA_WIDGET_LABEL || !node->widget)
        return 0.0f;
    if (!node->desc.text_wrap)
        return 0.0f;
    Ca_Label *lbl = (Ca_Label *)node->widget;
    if (!lbl->in_use || lbl->text[0] == '\0')
        return 0.0f;

    Ca_Window *win = node->window;
    if (!win || !win->instance || !win->instance->font)
        return 0.0f;
    Ca_Font *font = win->instance->font;

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float baked_logical = font->baked_px / font->content_scale;
    float desired_size  = node->desc.font_size > 0.0f ? node->desc.font_size : baked_logical;
    float font_scale    = desired_size / baked_logical;
    float cs_eff        = cs / font_scale;

    float line_height = (font->ascent - font->descent + font->line_gap) * font_scale;
    if (line_height < 1.0f) line_height = desired_size * 1.3f;

    float max_w = node->w - node->desc.padding_left - node->desc.padding_right;
    if (max_w < 1.0f) return 0.0f;

    #define MADV(c) \
        (((int)(c) >= CA_FONT_GLYPH_FIRST && (int)(c) < CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT) \
         ? font->glyphs[(int)(c) - CA_FONT_GLYPH_FIRST].xadvance / cs_eff : 0.0f)

    float cur_line_w = 0.0f;
    int line_count = 1;
    const char *p = lbl->text;
    while (*p) {
        float word_w = 0.0f;
        while (*p && *p != ' ' && *p != '\n') {
            word_w += MADV((unsigned char)*p);
            p++;
        }
        float with_space = (cur_line_w > 0.0f) ? cur_line_w + MADV(' ') + word_w : word_w;
        if (cur_line_w > 0.0f && with_space > max_w) {
            line_count++;
            cur_line_w = word_w;
        } else {
            cur_line_w = with_space;
        }
        if (*p == '\n') { line_count++; cur_line_w = 0; p++; }
        else if (*p == ' ') { p++; }
    }
    #undef MADV

    return node->desc.padding_top + line_height * line_count + node->desc.padding_bottom;
}

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

    /* Pre-measure wrapped text labels so their height is known before
       the parent container's auto-sizing pass.  Must happen after
       node->w is set (needed for word-wrap line breaking). */
    if (auto_h && node->child_count == 0) {
        float mh = measure_wrapped_text_height(node);
        if (mh > 0.0f) {
            node->h = mh;
            node->content_h = mh;
            h = mh;
        }
    }

    /* --- Splitter custom layout: size two panes based on ratio --- */
    if (node->widget_type == CA_WIDGET_SPLITTER && node->widget) {
        Ca_Splitter *sp = (Ca_Splitter *)node->widget;
        bool is_h = (sp->direction == CA_HORIZONTAL);
        float total = is_h ? w : h;
        float pane_space = total - sp->bar_size;
        if (pane_space < 0.0f) pane_space = 0.0f;
        float pane1 = pane_space * sp->ratio;
        float pane2 = pane_space - pane1;

        /* Lay out visible children: first two non-hidden become pane1 and pane2 */
        uint32_t pane_idx = 0;
        for (uint32_t i = 0; i < node->child_count; ++i) {
            Ca_Node *child = node->children[i];
            if (child->desc.hidden) {
                layout_node(child, 0, 0, 0, 0);
                continue;
            }
            if (pane_idx == 0) {
                if (is_h)
                    layout_node(child, x, y, pane1, h);
                else
                    layout_node(child, x, y, w, pane1);
            } else if (pane_idx == 1) {
                if (is_h)
                    layout_node(child, x + pane1 + sp->bar_size, y, pane2, h);
                else
                    layout_node(child, x, y + pane1 + sp->bar_size, w, pane2);
            } else {
                /* Extra children beyond two: hide them */
                layout_node(child, 0, 0, 0, 0);
            }
            pane_idx++;
        }
        node->content_w = w;
        node->content_h = h;
        return;
    }

    /* Count visible children that participate in flex flow.
       Children with position: absolute/fixed are out of flow. */
    uint32_t visible_count = 0;
    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *c = node->children[i];
        if (!c->desc.hidden && c->desc.position == CA_POSITION_RELATIVE)
            visible_count++;
    }
    if (visible_count == 0) goto out_of_flow;

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

    /* Pre-compute each child's hypothetical main-axis size (including margins) */
    float child_hypo_main[CA_MAX_NODE_CHILDREN];
    float child_margin_before[CA_MAX_NODE_CHILDREN];  /* main-axis leading margin */
    float child_margin_after[CA_MAX_NODE_CHILDREN];   /* main-axis trailing margin */
    float child_margin_cross0[CA_MAX_NODE_CHILDREN];  /* cross-axis start margin */
    float child_margin_cross1[CA_MAX_NODE_CHILDREN];  /* cross-axis end margin */
    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (child->desc.hidden || child->desc.position != CA_POSITION_RELATIVE) {
            child_hypo_main[i] = 0;
            child_margin_before[i] = child_margin_after[i] = 0;
            child_margin_cross0[i] = child_margin_cross1[i] = 0;
            continue;
        }
        if (is_row) {
            child_margin_before[i] = child->desc.margin_left;
            child_margin_after[i]  = child->desc.margin_right;
            child_margin_cross0[i] = child->desc.margin_top;
            child_margin_cross1[i] = child->desc.margin_bottom;
        } else {
            child_margin_before[i] = child->desc.margin_top;
            child_margin_after[i]  = child->desc.margin_bottom;
            child_margin_cross0[i] = child->desc.margin_left;
            child_margin_cross1[i] = child->desc.margin_right;
        }
        float ms = is_row ? child->desc.width : child->desc.height;
        if (ms > 0.0f) {
            child_hypo_main[i] = ms;
        } else {
            float nat = content_size(child, !is_row);
            child_hypo_main[i] = nat > 0.0f ? nat : 0.0f;
        }
        /* Include margins in the hypothetical space consumption */
        child_hypo_main[i] += child_margin_before[i] + child_margin_after[i];
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
            if (child->desc.hidden || child->desc.position != CA_POSITION_RELATIVE) continue;
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
            if (child->desc.hidden || child->desc.position != CA_POSITION_RELATIVE) continue;

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
            if (child->desc.hidden || child->desc.position != CA_POSITION_RELATIVE) { cm_arr[i] = 0; cc_arr[i] = 0; continue; }

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
            if (child->desc.hidden || child->desc.position != CA_POSITION_RELATIVE) {
                if (child->desc.hidden) layout_node(child, 0, 0, 0, 0);
                continue;
            }

            float cm = cm_arr[i];
            float cc = cc_arr[i];

            /* Subtract margins from available child content size */
            float mb = child_margin_before[i];
            float ma = child_margin_after[i];
            float mc0 = child_margin_cross0[i];
            float mc1 = child_margin_cross1[i];
            float cm_content = cm - mb - ma;
            float cc_content = cc - mc0 - mc1;
            if (cm_content < 0) cm_content = 0;
            if (cc_content < 0) cc_content = 0;

            /* Align-items: cross-axis positioning within the line */
            float cross_offset = mc0;
            Ca_Align align = node->desc.align_items;
            if (align == CA_ALIGN_CENTER)
                cross_offset = (line_avail_cross - cc_content - mc0 - mc1) * 0.5f + mc0;
            else if (align == CA_ALIGN_END)
                cross_offset = line_avail_cross - cc_content - mc1;

            float cx, cy, cw, ch;
            if (is_row) {
                cx = inner_x + main_cursor + mb - scroll_off_x;
                cy = inner_y + cross_cursor + cross_offset - scroll_off_y;
                cw = cm_content;  ch = cc_content;
            } else {
                cx = inner_x + cross_cursor + cross_offset - scroll_off_x;
                cy = inner_y + main_cursor + mb - scroll_off_y;
                cw = cc_content;  ch = cm_content;
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

out_of_flow:
    /* Position absolute/fixed children (they are out of the flex flow) */
    for (uint32_t i = 0; i < node->child_count; ++i) {
        Ca_Node *child = node->children[i];
        if (child->desc.hidden || child->desc.position == CA_POSITION_RELATIVE)
            continue;
        float cw = child->desc.width  > 0.0f ? child->desc.width  : node->w;
        float ch = child->desc.height > 0.0f ? child->desc.height : node->h;
        float cx, cy;
        if (child->desc.position == CA_POSITION_ABSOLUTE) {
            /* Relative to parent's content box */
            cx = node->x + node->desc.padding_left + child->desc.pos_x;
            cy = node->y + node->desc.padding_top  + child->desc.pos_y;
        } else { /* CA_POSITION_FIXED — relative to window origin */
            cx = child->desc.pos_x;
            cy = child->desc.pos_y;
        }
        layout_node(child, cx, cy, cw, ch);
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
