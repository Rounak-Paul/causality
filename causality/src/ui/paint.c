// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* paint.c — CPU-side draw command generation */
#include "paint.h"
#include "node.h"
#include "font.h"
#include "ca_theme.h"
#include "style.h"
#include "scrollbar.h"
#include "../../include/ca_icons.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>

/* Process memory (RSS) for debug overlay */
#ifdef __APPLE__
  #include <mach/mach.h>
#elif defined(__linux__)
  #include <stdio.h>
#elif defined(_WIN32)
  #include <windows.h>
  #include <psapi.h>
#endif

static void unpack_color(uint32_t packed, float *r, float *g, float *b, float *a)
{
    *r = (float)((packed >> 24) & 0xFF) / 255.0f;
    *g = (float)((packed >> 16) & 0xFF) / 255.0f;
    *b = (float)((packed >>  8) & 0xFF) / 255.0f;
    *a = (float)((packed)       & 0xFF) / 255.0f;
}

typedef struct OverlayCssStyle {
    uint32_t background;
    uint32_t color;
    float radius;
} OverlayCssStyle;

/* Resolve a synthetic overlay class while retaining neutral fallback values. */
static OverlayCssStyle overlay_css_style(Ca_Window *window, Ca_Node *owner,
                                         const char *classes,
                                         uint32_t background, uint32_t color,
                                         float radius)
{
    OverlayCssStyle result = { background, color, radius };
    if (!window || !window->instance || !owner)
        return result;
    Ca_Instance *instance = window->instance;
    if (!instance->system_stylesheet && !instance->stylesheet) return result;
    Ca_ResolvedStyle resolved;
    ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                            owner, CA_ELEM_DIV, classes, &resolved);
    if (resolved.background_color != 0u) result.background = resolved.background_color;
    if (resolved.color != 0u) result.color = resolved.color;
    result.radius = resolved.border_radius * window->ui_scale;
    return result;
}

/* Clip rect state passed through the tree */
typedef struct {
    bool  active;
    float x, y, w, h;
} ClipRect;

static ClipRect clip_intersect(ClipRect parent, float cx, float cy, float cw, float ch)
{
    ClipRect r;
    r.active = true;
    float x0 = parent.active ? (cx > parent.x ? cx : parent.x) : cx;
    float y0 = parent.active ? (cy > parent.y ? cy : parent.y) : cy;
    float x1_a = cx + cw;
    float y1_a = cy + ch;
    float x1_b = parent.active ? parent.x + parent.w : x1_a;
    float y1_b = parent.active ? parent.y + parent.h : y1_a;
    float x1 = x1_a < x1_b ? x1_a : x1_b;
    float y1 = y1_a < y1_b ? y1_a : y1_b;
    r.x = x0; r.y = y0;
    r.w = (x1 > x0) ? x1 - x0 : 0.0f;
    r.h = (y1 > y0) ? y1 - y0 : 0.0f;
    return r;
}

static void set_clip(Ca_DrawCmd *cmd, ClipRect clip)
{
    cmd->has_clip = clip.active;
    if (clip.active) {
        cmd->clip_x = clip.x;
        cmd->clip_y = clip.y;
        cmd->clip_w = clip.w;
        cmd->clip_h = clip.h;
    }
}

static void touch_font_pages_for_cmds(Ca_Font *font,
                                      const Ca_DrawCmd *cmds,
                                      uint32_t count)
{
    if (!font || !cmds) return;
    for (uint32_t i = 0; i < count; i++) {
        const Ca_DrawCmd *cmd = &cmds[i];
        if (cmd->in_use && cmd->type == CA_DRAW_GLYPH)
            ca_font_touch_page(font, cmd->font_page_index);
    }
}

/* Forward declarations — used by paint_node_content before definition */
static void paint_text(Ca_Window *win, Ca_Font *font,
                       Ca_Node *node,
                       const char *text, uint32_t packed_color);
static void paint_text_wrapped(Ca_Window *win, Ca_Font *font,
                               Ca_Node *node,
                               const char *text, uint32_t packed_color);
static void paint_text_left(Ca_Window *win, Ca_Font *font,
                            Ca_Node *node,
                            const char *text, uint32_t packed_color);
static void paint_cursor(Ca_Window *win, Ca_Font *font,
                         Ca_Node *node, const char *text, int cursor_pos);
static void paint_focus_ring(Ca_Window *win, Ca_Node *node);

/* Walk up from a node and compute its effective clip rect from ancestors
   with overflow != visible. */
static ClipRect find_clip_for_node(Ca_Node *node)
{
    ClipRect clip = { .active = false };
    Ca_Node *cur = node->parent;
    while (cur) {
        if (cur->desc.overflow_x >= 1 || cur->desc.overflow_y >= 1) {
            clip = clip_intersect(clip, cur->x, cur->y,
                                  ca_scrollbar_viewport_width(cur),
                                  ca_scrollbar_viewport_height(cur));
        }
        cur = cur->parent;
    }
    return clip;
}

/* Compute the clip rect for text glyphs of a node.
 *
 * Single-line glyphs are positioned around a baseline derived from
 * (ascent + descent) of the font tier; with tight rows (e.g. a 16 px
 * label rendering a 12 px font) the descender of letters like 'g' lands
 * a fraction of a pixel below node->y + node->h.  Intersecting the clip
 * with the node's own bounds would slice that descender off, even though
 * the parent container has plenty of room.  This mirrors the CSS rule
 * that a line-box does not vertically clip its own glyphs — only an
 * ancestor `overflow:hidden` should do so.
 *
 * Horizontally we still clip to the node so overflowing text does not
 * bleed into sibling widgets.  Vertically we honour the node's bounds
 * only if it explicitly opts in via overflow_y >= 1; otherwise we
 * inherit from the closest scrolling/clipping ancestor (if any). */
static ClipRect text_clip_for_node(Ca_Node *node)
{
    ClipRect ancestor = find_clip_for_node(node);
    const float vertical_pad = 2.0f;

    ClipRect r;
    r.active = true;

    /* Horizontal: clamp to node width, then intersect with ancestor. */
    float x0 = node->x;
    float x1 = node->x + node->w;
    if (ancestor.active) {
        if (ancestor.x > x0)            x0 = ancestor.x;
        if (ancestor.x + ancestor.w < x1) x1 = ancestor.x + ancestor.w;
    }
    r.x = x0;
    r.w = (x1 > x0) ? x1 - x0 : 0.0f;

    /* Vertical: inherit ancestor unless the node itself requests clipping. */
    bool clip_self_v = node->desc.overflow_y >= 1;
    if (clip_self_v && ancestor.active) {
        float y0_a = node->y - vertical_pad;
        float y1_a = node->y + node->h + vertical_pad;
        float y0 = y0_a > ancestor.y ? y0_a : ancestor.y;
        float y1_b = ancestor.y + ancestor.h;
        float y1 = y1_a < y1_b ? y1_a : y1_b;
        r.y = y0;
        r.h = (y1 > y0) ? y1 - y0 : 0.0f;
    } else if (clip_self_v) {
        r.y = node->y - vertical_pad;
        r.h = node->h + vertical_pad * 2.0f;
    } else if (ancestor.active) {
        r.y = ancestor.y - vertical_pad;
        r.h = ancestor.h + vertical_pad * 2.0f;
    } else {
        r.active = (r.w > 0.0f); /* no vertical clip needed */
        r.y = 0.0f;
        r.h = 0.0f;
        if (!r.active) return r;
        /* Width-only clip: encode with effectively unbounded y range so
           the GPU scissor test never rejects on vertical bounds. */
        r.y = -1.0e6f;
        r.h =  2.0e6f;
    }
    return r;
}

/* Walk ancestors to check if any node in the chain is disabled. */
static bool is_node_effectively_disabled(Ca_Node *n)
{
    for (Ca_Node *cur = n; cur; cur = cur->parent)
        if (cur->desc.disabled) return true;
    return false;
}

/* Paint a single node's OWN visual content (background rect + widget-specific).
   Does NOT recurse into children.  Does NOT paint scrollbars (post-children). */
static void paint_node_content(Ca_Window *win, Ca_Font *font, Ca_Node *node, ClipRect clip)
{
    if (!node->in_use) return;
    if (node->desc.hidden) return;
    if (node->desc.visibility_hidden) return;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;

    /* Record starting draw cmd index so we can apply disabled dim after. */
    uint32_t cmd_start = win->draw_cmd_count;

    /* ---- Backdrop blur — frosted glass effect ---- */
    if (node->desc.backdrop_blur > 0.0f &&
        ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type                 = CA_DRAW_BACKDROP_BLUR;
        cmd->x                    = node->x;
        cmd->y                    = node->y;
        cmd->w                    = node->w;
        cmd->h                    = node->h;
        cmd->backdrop_blur_radius = node->desc.backdrop_blur;
        cmd->corner_radius        = node->desc.corner_radius;
        cmd->corner_tl = node->desc.border_radius_tl > 0.0f ? node->desc.border_radius_tl : node->desc.corner_radius;
        cmd->corner_tr = node->desc.border_radius_tr > 0.0f ? node->desc.border_radius_tr : node->desc.corner_radius;
        cmd->corner_br = node->desc.border_radius_br > 0.0f ? node->desc.border_radius_br : node->desc.corner_radius;
        cmd->corner_bl = node->desc.border_radius_bl > 0.0f ? node->desc.border_radius_bl : node->desc.corner_radius;
        cmd->z_index   = node->desc.z_index;
        cmd->in_use    = true;
        set_clip(cmd, clip);
    }

    /* ---- Box shadow — GPU SDF Gaussian blur ---- */
    if (node->desc.shadow_color != 0 &&
        ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
        float sr, sg, sb, sa;
        unpack_color(node->desc.shadow_color, &sr, &sg, &sb, &sa);
        float blur   = node->desc.shadow_blur;
        float expand = blur; /* expand to cover the blur falloff extent */
        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type        = CA_DRAW_RECT;
        cmd->draw_mode   = CA_DRAW_MODE_SHADOW;
        cmd->x           = node->x + node->desc.shadow_offset_x - expand;
        cmd->y           = node->y + node->desc.shadow_offset_y - expand;
        cmd->w           = node->w + expand * 2.0f;
        cmd->h           = node->h + expand * 2.0f;
        cmd->r = sr; cmd->g = sg; cmd->b = sb; cmd->a = sa;
        /* Pass the node's corner radii so shadow follows the shape */
        cmd->corner_tl   = node->desc.border_radius_tl > 0.0f ? node->desc.border_radius_tl : node->desc.corner_radius;
        cmd->corner_tr   = node->desc.border_radius_tr > 0.0f ? node->desc.border_radius_tr : node->desc.corner_radius;
        cmd->corner_br   = node->desc.border_radius_br > 0.0f ? node->desc.border_radius_br : node->desc.corner_radius;
        cmd->corner_bl   = node->desc.border_radius_bl > 0.0f ? node->desc.border_radius_bl : node->desc.corner_radius;
        cmd->blur_radius = blur;
        cmd->z_index     = node->desc.z_index;
        cmd->in_use      = true;
        set_clip(cmd, clip);
    }

    /* ---- Background rect (or gradient) ---- */
    if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
        node->draw_cmd_idx = (int32_t)win->draw_cmd_count;
        Ca_DrawCmd *cmd    = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type = CA_DRAW_RECT;
        cmd->x    = node->x;   cmd->y = node->y;
        cmd->w    = node->w;   cmd->h = node->h;

        /* Per-corner radii — GPU handles asymmetric corners natively now */
        cmd->corner_radius = node->desc.corner_radius;
        cmd->corner_tl = node->desc.border_radius_tl > 0.0f ? node->desc.border_radius_tl : node->desc.corner_radius;
        cmd->corner_tr = node->desc.border_radius_tr > 0.0f ? node->desc.border_radius_tr : node->desc.corner_radius;
        cmd->corner_br = node->desc.border_radius_br > 0.0f ? node->desc.border_radius_br : node->desc.corner_radius;
        cmd->corner_bl = node->desc.border_radius_bl > 0.0f ? node->desc.border_radius_bl : node->desc.corner_radius;

        /* Gradient or solid fill */
        if (node->desc.gradient_type != 0) {
            cmd->draw_mode     = (Ca_DrawMode)node->desc.gradient_type;
            unpack_color(node->desc.background,  &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            unpack_color(node->desc.gradient_color2, &cmd->color2_r, &cmd->color2_g, &cmd->color2_b, &cmd->color2_a);
            cmd->gradient_angle = node->desc.gradient_angle;
            cmd->gradient_cx    = node->desc.gradient_cx;
            cmd->gradient_cy    = node->desc.gradient_cy;
        } else {
            cmd->draw_mode = CA_DRAW_MODE_NORMAL;
            unpack_color(node->desc.background, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
        }

        float op = (node->desc.opacity > 0.0f) ? node->desc.opacity : 1.0f;
        cmd->a       *= op;
        cmd->color2_a *= op;

        /* Uniform border.

           Suppressed when the four sides do not share one colour: the
           per-side rects emitted below would otherwise be drawn on top of
           a full uniform border, which still shows through at the corners
           (where the per-side rects meet diagonally) and wins wherever a
           side resolved to no colour of its own. A shaded/bevelled border
           is exactly that case, so it has to own the whole edge. */
        bool uniform_sides =
            node->desc.border_top_c == node->desc.border_right_c &&
            node->desc.border_top_c == node->desc.border_bottom_c &&
            node->desc.border_top_c == node->desc.border_left_c;
        if (uniform_sides) {
            cmd->border_width = node->desc.border_width;
            if (node->desc.border_color != 0) {
                unpack_color(node->desc.border_color,
                             &cmd->border_r, &cmd->border_g,
                             &cmd->border_b, &cmd->border_a);
            }
        }
        cmd->z_index = node->desc.z_index;
        cmd->in_use  = true;
        set_clip(cmd, clip);
    }

    /* ---- Per-side border rects (top / right / bottom / left) ---- */
    {
        struct { float w; uint32_t c; int side; } edges[4] = {
            { node->desc.border_top_w,    node->desc.border_top_c,    0 },
            { node->desc.border_right_w,  node->desc.border_right_c,  1 },
            { node->desc.border_bottom_w, node->desc.border_bottom_c, 2 },
            { node->desc.border_left_w,   node->desc.border_left_c,   3 },
        };
        for (int ei = 0; ei < 4; ei++) {
            float ew = edges[ei].w;
            if (ew <= 0.0f || edges[ei].c == 0) continue;
            if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) break;
            float er, eg, eb, ea;
            unpack_color(edges[ei].c, &er, &eg, &eb, &ea);
            Ca_DrawCmd *ec = &win->draw_cmds[win->draw_cmd_count++];
            memset(ec, 0, sizeof(*ec));
            ec->type = CA_DRAW_RECT;
            ec->r = er; ec->g = eg; ec->b = eb; ec->a = ea;
            ec->z_index = node->desc.z_index;
            ec->in_use  = true;
            /* Each side owns a disjoint span: the horizontal edges run the
               full width and the vertical edges are inset between them.
               Overlapping full-span rects would let whichever side is
               painted last win the corner square outright, which on a
               two-tone (bevelled) border shows as a wrongly-shaded block at
               two of the four corners rather than a clean joint. */
            float top_w    = node->desc.border_top_w    > 0.0f ? node->desc.border_top_w    : 0.0f;
            float bottom_w = node->desc.border_bottom_w > 0.0f ? node->desc.border_bottom_w : 0.0f;
            float inner_y  = node->y + top_w;
            float inner_h  = node->h - top_w - bottom_w;
            if (inner_h < 0.0f) inner_h = 0.0f;
            switch (edges[ei].side) {
                case 0: ec->x = node->x;                  ec->y = node->y;                  ec->w = node->w;  ec->h = ew;       break; /* top    */
                case 1: ec->x = node->x + node->w - ew;   ec->y = inner_y;                  ec->w = ew;       ec->h = inner_h;  break; /* right  */
                case 2: ec->x = node->x;                  ec->y = node->y + node->h - ew;   ec->w = node->w;  ec->h = ew;       break; /* bottom */
                case 3: ec->x = node->x;                  ec->y = inner_y;                  ec->w = ew;       ec->h = inner_h;  break; /* left   */
            }
            set_clip(ec, clip);
        }
    }

    /* ---- CSS outline — drawn outside the border, does not affect layout ---- */
    if (node->desc.outline_width > 0.0f && node->desc.outline_color != 0) {
        float ow  = node->desc.outline_width;
        float off = node->desc.outline_offset;
        float ox  = node->x  - ow - off;
        float oy  = node->y  - ow - off;
        float oow = node->w + (ow + off) * 2.0f;
        float ooh = node->h + (ow + off) * 2.0f;

        struct { float x, y, w, h; } osides[4] = {
            { ox,           oy,            oow, ow  }, /* top    */
            { ox + oow - ow, oy,           ow,  ooh }, /* right  */
            { ox,           oy + ooh - ow, oow, ow  }, /* bottom */
            { ox,           oy,            ow,  ooh }, /* left   */
        };
        float or_, og, ob, oa;
        unpack_color(node->desc.outline_color, &or_, &og, &ob, &oa);
        for (int oi = 0; oi < 4; oi++) {
            if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) break;
            Ca_DrawCmd *oc = &win->draw_cmds[win->draw_cmd_count++];
            memset(oc, 0, sizeof(*oc));
            oc->type    = CA_DRAW_RECT;
            oc->x = osides[oi].x; oc->y = osides[oi].y;
            oc->w = osides[oi].w; oc->h = osides[oi].h;
            oc->r = or_; oc->g = og; oc->b = ob; oc->a = oa;
            oc->z_index = node->desc.z_index;
            oc->in_use  = true;
            set_clip(oc, clip);
        }
    }

    /* ---- Widget-specific content ---- */
    if (!font) return;

    switch (node->widget_type) {
    case CA_WIDGET_LABEL: {
        Ca_Label *lbl = (Ca_Label *)node->widget;
        if (lbl && lbl->in_use) {
            const char *txt = ca_label_get_text(lbl);
            if (txt[0]) {
                if (node->desc.text_wrap)
                    paint_text_wrapped(win, font, node, txt, lbl->color);
                else
                    paint_text(win, font, node, txt, lbl->color);
            }
        }
        break;
    }
    case CA_WIDGET_BUTTON: {
        Ca_Button *btn = (Ca_Button *)node->widget;
        if (btn && btn->in_use && btn->text[0])
            paint_text(win, font, node, btn->text, btn->text_color);
        break;
    }
    case CA_WIDGET_TEXT_INPUT: {
        Ca_TextInput *inp = (Ca_TextInput *)node->widget;
        if (inp && inp->in_use) {
            if (inp->text[0] != '\0')
                paint_text_left(win, font, node, inp->text, inp->text_color);
            else if (inp->placeholder[0] != '\0')
                paint_text_left(win, font, node, inp->placeholder,
                                inp->placeholder_color);
            /* Cursor is painted in the decoration pass, not cached */
        }
        break;
    }
    case CA_WIDGET_CHECKBOX: {
        Ca_Checkbox *cb = (Ca_Checkbox *)node->widget;
        if (!cb || !cb->in_use) break;
        float bs = node->h * 0.8f;
        float bx = node->x + 1.0f * ui_s;
        float by = node->y + (node->h - bs) * 0.5f;
        /* Box background */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = bx; c->y = by; c->w = bs; c->h = bs;
            c->corner_radius = 3.0f * ui_s;
            if (cb->checked) { float _r, _g, _b, _a; unpack_color(CA_THEME_ACCENT,     &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            else             { float _r, _g, _b, _a; unpack_color(CA_THEME_BG_OVERLAY, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Checkmark */
        if (cb->checked && ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 2u)) {
            float cx = bx + bs * 0.25f, cy = by + bs * 0.5f;
            Ca_DrawCmd *c1 = &win->draw_cmds[win->draw_cmd_count++];
            memset(c1, 0, sizeof(*c1));
            c1->type = CA_DRAW_RECT;
            c1->x = cx; c1->y = cy; c1->w = bs * 0.2f; c1->h = bs * 0.35f;
            c1->r = 1; c1->g = 1; c1->b = 1; c1->a = 1;
            c1->in_use = true;
            Ca_DrawCmd *c2 = &win->draw_cmds[win->draw_cmd_count++];
            memset(c2, 0, sizeof(*c2));
            c2->type = CA_DRAW_RECT;
            c2->x = cx + bs * 0.15f; c2->y = by + bs * 0.25f;
            c2->w = bs * 0.4f; c2->h = bs * 0.2f;
            c2->r = 1; c2->g = 1; c2->b = 1; c2->a = 1;
            c2->in_use = true;
        }
        /* Label text */
        if (cb->text[0]) {
            Ca_Node tn = *node;
            tn.x = bx + bs + 6.0f * ui_s;
            tn.w = node->w - bs - 6.0f * ui_s;
            paint_text(win, font, &tn, cb->text, cb->text_color);
        }
        break;
    }
    case CA_WIDGET_RADIO: {
        Ca_Radio *r = (Ca_Radio *)node->widget;
        if (!r || !r->in_use) break;
        float bs = node->h * 0.8f;
        float bx = node->x + 1.0f * ui_s;
        float by = node->y + (node->h - bs) * 0.5f;
        /* Outer circle */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = bx; c->y = by; c->w = bs; c->h = bs;
            c->corner_radius = bs * 0.5f;
            { float _r, _g, _b, _a; unpack_color(CA_THEME_BG_OVERLAY, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Inner dot when selected */
        if (r->value && ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            float ds = bs * 0.5f;
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = bx + (bs - ds) * 0.5f;
            c->y = by + (bs - ds) * 0.5f;
            c->w = ds; c->h = ds;
            c->corner_radius = ds * 0.5f;
            { float _r, _g, _b, _a; unpack_color(CA_THEME_ACCENT, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Label text */
        if (r->text[0]) {
            Ca_Node tn = *node;
            tn.x = bx + bs + 6.0f * ui_s;
            tn.w = node->w - bs - 6.0f * ui_s;
            paint_text(win, font, &tn, r->text, r->text_color);
        }
        break;
    }
    case CA_WIDGET_SLIDER: {
        Ca_Slider *sl = (Ca_Slider *)node->widget;
        if (!sl || !sl->in_use) break;
        float track_h = 4.0f * ui_s;
        float track_y = node->y + (node->h - track_h) * 0.5f;
        float pct = (sl->max_val > sl->min_val)
            ? (sl->value - sl->min_val) / (sl->max_val - sl->min_val) : 0;
        /* Track background */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = track_y; c->w = node->w; c->h = track_h;
            c->corner_radius = 2.0f * ui_s;
            { float _r, _g, _b, _a; unpack_color(CA_THEME_BG_SURFACE, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Fill */
        float fill_w = node->w * pct;
        if (fill_w > 0 && ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = track_y; c->w = fill_w; c->h = track_h;
            c->corner_radius = 2.0f * ui_s;
            { float _r, _g, _b, _a; unpack_color(CA_THEME_ACCENT, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Thumb */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            float thumb_sz = 14.0f * ui_s;
            float tx = node->x + fill_w - thumb_sz * 0.5f;
            float ty = node->y + (node->h - thumb_sz) * 0.5f;
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = tx; c->y = ty; c->w = thumb_sz; c->h = thumb_sz;
            c->corner_radius = thumb_sz * 0.5f;
            c->r = 1.0f; c->g = 1.0f; c->b = 1.0f; c->a = 1.0f;
            c->in_use = true;
        }
        break;
    }
    case CA_WIDGET_TOGGLE: {
        Ca_Toggle *t = (Ca_Toggle *)node->widget;
        if (!t || !t->in_use) break;
        /* Track */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = node->y; c->w = node->w; c->h = node->h;
            c->corner_radius = node->h * 0.5f;
            if (t->on) { float _r, _g, _b, _a; unpack_color(CA_THEME_SUCCESS,    &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            else       { float _r, _g, _b, _a; unpack_color(CA_THEME_BG_OVERLAY, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Thumb */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            float inset = 2.0f * ui_s;
            float thumb_d = node->h - inset * 2;
            float tx = t->on ? (node->x + node->w - thumb_d - inset) : (node->x + inset);
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = tx; c->y = node->y + inset;
            c->w = thumb_d; c->h = thumb_d;
            c->corner_radius = thumb_d * 0.5f;
            c->r = 1.0f; c->g = 1.0f; c->b = 1.0f; c->a = 1.0f;
            c->in_use = true;
        }
        break;
    }
    case CA_WIDGET_PROGRESS: {
        Ca_Progress *p = (Ca_Progress *)node->widget;
        if (!p || !p->in_use) break;
        float rad = node->h * 0.5f;
        /* Track */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = node->y; c->w = node->w; c->h = node->h;
            c->corner_radius = rad;
            { float _r, _g, _b, _a; unpack_color(CA_THEME_BG_SURFACE, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
            c->in_use = true;
        }
        /* Fill */
        float fw = node->w * p->value;
        if (fw > 0 && ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            float fr, fg, fb, fa;
            unpack_color(p->bar_color, &fr, &fg, &fb, &fa);
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = node->y; c->w = fw; c->h = node->h;
            c->corner_radius = rad;
            c->r = fr; c->g = fg; c->b = fb; c->a = fa;
            c->in_use = true;
        }
        break;
    }
    case CA_WIDGET_SELECT: {
        Ca_Select *sel = (Ca_Select *)node->widget;
        if (!sel || !sel->in_use) break;
        /* Current selection text */
        if (sel->selected >= 0 && sel->selected < sel->option_count)
            paint_text(win, font, node, sel->options[sel->selected], 0);
        /* Down arrow indicator */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            float asz = 6.0f;
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x + node->w - asz - 6.0f;
            c->y = node->y + (node->h - asz * 0.5f) * 0.5f;
            c->w = asz; c->h = asz * 0.5f;
            c->r = 0.7f; c->g = 0.7f; c->b = 0.7f; c->a = 1.0f;
            c->in_use = true;
        }
        /* Open dropdown overlay is painted in the overlay pass, not cached */
        break;
    }
    case CA_WIDGET_TABBAR: {
        /* Called for each tab_node (child of tabbar's main node) */
        Ca_TabBar *tb = (Ca_TabBar *)node->widget;
        if (!tb || !tb->in_use) break;
        for (int ti = 0; ti < tb->count; ++ti) {
            if (tb->tab_nodes[ti] == node) {
                uint32_t tc = (ti == tb->active) ? tb->active_text : tb->inactive_text;
                paint_text(win, font, node, tb->labels[ti], tc);
                break;
            }
        }
        break;
    }
    case CA_WIDGET_TREENODE: {
        Ca_TreeNode *tn = (Ca_TreeNode *)node->widget;
        if (!tn || !tn->in_use) break;
        if (node->child_count == 0) break;
        Ca_Node *hdr = node->children[0];
        if (!hdr || hdr->desc.hidden) break;

        float fs = hdr->desc.font_size > 0 ? hdr->desc.font_size : 12.0f;
        float glyph_w = fs;  /* column width for chevron / icon */

        /* Note: hover/active highlight on the header row is painted by the
           header node's own bg rect via paint_node_content(hdr). Pseudo-state
           CSS (.tree-row:hover, :active) sets hdr_node->desc.background which
           is then drawn at hdr's full bounds. We intentionally do NOT emit a
           manual hover rect here — that historical code path produced a
           draw command with in_use=false (renderer skips it) and additionally
           polluted the cache slot count, causing intermittent partial-row
           hover artifacts on tree-node containers that were re-painted while
           the mouse was over them. */

        /* Dim the text color for the chevron indicator */
        uint32_t chevron_color = tn->text_color;
        { /* halve the alpha */
            uint8_t a = chevron_color & 0xFF;
            a = (uint8_t)(a >> 1);
            chevron_color = (chevron_color & 0xFFFFFF00u) | a;
        }

        float x_off = hdr->desc.padding_left;

        /* Disclosure triangle — suppress for leaf nodes */
        if (!tn->is_leaf) {
            const char *indicator = tn->expanded
                ? CA_ICON_FA_CARET_DOWN
                : CA_ICON_FA_CARET_RIGHT;
            Ca_Node ind_n = *hdr;
            ind_n.x = hdr->x + x_off;
            ind_n.desc.padding_left = 0;
            ind_n.w = glyph_w;
            paint_text(win, font, &ind_n, indicator, chevron_color);
            x_off += glyph_w;
        } else {
            x_off += glyph_w;
        }

        /* Icon glyph */
        if (tn->icon[0]) {
            Ca_Node ico_n = *hdr;
            ico_n.x = hdr->x + x_off;
            ico_n.desc.padding_left = 0;
            ico_n.w = glyph_w;
            uint32_t ic = tn->icon_color ? tn->icon_color : tn->text_color;
            paint_text(win, font, &ico_n, tn->icon, ic);
            x_off += glyph_w;
        }

        /* Text label */
        Ca_Node txt_n = *hdr;
        txt_n.x = hdr->x + x_off;
        txt_n.desc.padding_left = 0;
        txt_n.w = hdr->w - x_off;
        paint_text(win, font, &txt_n, tn->text, tn->text_color);
        break;
    }
    case CA_WIDGET_SPLITTER: {
        Ca_Splitter *sp = (Ca_Splitter *)node->widget;
        if (!sp || !sp->in_use) break;
        /* Draw the divider bar between the two panes */
        bool is_h = (sp->direction == CA_HORIZONTAL);
        float bar_x, bar_y, bar_w, bar_h;
        if (is_h) {
            float pane_space = node->w - sp->bar_size;
            if (pane_space < 0) pane_space = 0;
            bar_x = node->x + pane_space * sp->ratio;
            bar_y = node->y;
            bar_w = sp->bar_size;
            bar_h = node->h;
        } else {
            float pane_space = node->h - sp->bar_size;
            if (pane_space < 0) pane_space = 0;
            bar_x = node->x;
            bar_y = node->y + pane_space * sp->ratio;
            bar_w = node->w;
            bar_h = sp->bar_size;
        }
        bool active = win->hovered_node == node || sp->dragging;
        uint32_t color = active ? sp->bar_hover_color : sp->bar_color;
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type   = CA_DRAW_RECT;
            cmd->x      = bar_x;
            cmd->y      = bar_y;
            cmd->w      = bar_w;
            cmd->h      = bar_h;
            unpack_color(color, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            cmd->in_use = true;
            set_clip(cmd, clip);
        }
        break;
    }
    case CA_WIDGET_IMAGE: {
        Ca_Image *img = (Ca_Image *)node->widget;
        if (!img || !img->in_use) break;
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_Instance *inst = win->instance;
            size_t image_index = 0;
            if (!ca_pool_index(&inst->images, img, &image_index) ||
                image_index > UINT32_MAX)
                break;
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type        = CA_DRAW_IMAGE;
            cmd->x           = node->x;
            cmd->y           = node->y;
            cmd->w           = node->w;
            cmd->h           = node->h;
            cmd->r = 1; cmd->g = 1; cmd->b = 1; cmd->a = 1;
            cmd->u0 = 0; cmd->v0 = 0; cmd->u1 = 1; cmd->v1 = 1;
            cmd->image_index = (uint32_t)image_index;
            cmd->z_index     = node->desc.z_index;
            cmd->in_use      = true;
            set_clip(cmd, clip);
        }
        break;
    }
    case CA_WIDGET_VIEWPORT: {
        Ca_Viewport *vp = (Ca_Viewport *)node->widget;
        if (!vp || !vp->in_use) break;
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            size_t pool_index = 0;
            if (!ca_pool_index(&win->viewport_pool, vp, &pool_index) ||
                pool_index > UINT32_MAX)
                break;
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type           = CA_DRAW_VIEWPORT;
            cmd->x              = node->x;
            cmd->y              = node->y;
            cmd->w              = node->w;
            cmd->h              = node->h;
            cmd->r = 1; cmd->g = 1; cmd->b = 1; cmd->a = 1;
            cmd->u0 = 0; cmd->v0 = 0; cmd->u1 = 1; cmd->v1 = 1;
            cmd->viewport_index = (uint32_t)pool_index;
            cmd->z_index        = node->desc.z_index;
            cmd->in_use         = true;
            set_clip(cmd, clip);
        }
        break;
    }
    default: break;
    }

    if (node->tree_drop_indicator != CA_TREE_DROP_NONE &&
        node->tree_drop_color != 0 &&
        ca_window_reserve_draw_commands(
            win, (size_t)win->draw_cmd_count + 1u)) {
        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type = CA_DRAW_RECT;
        cmd->x = node->x;
        cmd->y = node->y;
        cmd->w = node->w;
        cmd->h = node->h;
        float thickness = 2.0f * ui_s;
        if (node->tree_drop_indicator == CA_TREE_DROP_BEFORE)
            cmd->h = thickness;
        else if (node->tree_drop_indicator == CA_TREE_DROP_AFTER) {
            cmd->y += node->h - thickness;
            cmd->h = thickness;
        }
        unpack_color(node->tree_drop_color,
                     &cmd->r, &cmd->g, &cmd->b, &cmd->a);
        if (node->tree_drop_indicator == CA_TREE_DROP_SOURCE)
            cmd->a *= 0.16f;
        else if (node->tree_drop_indicator == CA_TREE_DROP_INSIDE) {
            cmd->a *= 0.12f;
            cmd->border_width = thickness;
            cmd->border_r = cmd->r;
            cmd->border_g = cmd->g;
            cmd->border_b = cmd->b;
            cmd->border_a = 1.0f;
        }
        cmd->z_index = node->desc.z_index;
        cmd->overlay = true;
        cmd->in_use = true;
        set_clip(cmd, clip);
    }

    /* Apply disabled visual dimming to all draw commands emitted for this node. */
    if (is_node_effectively_disabled(node)) {
        const float dim = 0.4f;
        for (uint32_t i = cmd_start; i < win->draw_cmd_count; ++i)
            win->draw_cmds[i].a *= dim;
    }
}

/* Paint scrollbar overlays for a node (post-children, so they draw on top). */
static void paint_scrollbars(Ca_Window *win, Ca_Node *node, ClipRect clip)
{
    /* Scrollbars are painted as overlay so they appear on top of child
       text glyphs.  The renderer draws all rects before all glyphs within
       each phase; using phase 0 for the scrollbar rects would let every
       glyph in the scroll container render over them.  Marking them
       overlay = true puts them in phase 1, after all phase-0 glyphs. */
    uint32_t sb_first = win->draw_cmd_count;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    if (node->scrollbar_y_visible) {
        float bar_w   = ca_scrollbar_vertical_width(node);
        float track_h = ca_scrollbar_viewport_height(node);
        float ratio   = track_h / node->content_h;
        float thumb_h = track_h * ratio;
        if (thumb_h < 20.0f * ui_s) thumb_h = 20.0f * ui_s;
        if (thumb_h > track_h) thumb_h = track_h;

        float max_scroll = ca_scrollbar_max_y(node);
        float scroll_pct = (max_scroll > 0.0f) ? node->scroll_y / max_scroll : 0.0f;
        float thumb_y    = node->y + scroll_pct * (track_h - thumb_h);
        float bar_x      = node->x + node->w - bar_w;

        bool dragging_y = (win->scrollbar_drag_node == node && win->scrollbar_drag_y);

        /* Track — dark inset fill */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type = CA_DRAW_RECT;
            cmd->x = bar_x; cmd->y = node->y;
            cmd->w = bar_w; cmd->h = track_h;
            uint32_t track_color = node->desc.scrollbar_track_color_set
                                       ? node->desc.scrollbar_track_color
                                       : CA_THEME_SCROLLBAR_TRACK;
            unpack_color(track_color, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            cmd->corner_radius = node->desc.scrollbar_radius;
            cmd->border_width = 0.0f;
            cmd->in_use = true;
            set_clip(cmd, clip);
        }
        /* Thumb */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type = CA_DRAW_RECT;
            cmd->x = bar_x; cmd->y = thumb_y;
            cmd->w = bar_w; cmd->h = thumb_h;
            uint32_t thumb_color = dragging_y
                ? (node->desc.scrollbar_thumb_active_color_set
                       ? node->desc.scrollbar_thumb_active_color
                       : CA_THEME_SCROLLBAR_THUMB_ACTIVE)
                : (node->desc.scrollbar_thumb_color_set
                       ? node->desc.scrollbar_thumb_color
                       : CA_THEME_SCROLLBAR_THUMB);
            unpack_color(thumb_color,
                         &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            cmd->corner_radius = node->desc.scrollbar_radius;
            cmd->in_use = true;
            set_clip(cmd, clip);
        }
    }
    /* ---- X scrollbar ---- */
    if (node->scrollbar_x_visible) {
        float bar_h   = ca_scrollbar_horizontal_height(node);
        float margin  = 2.0f * ui_s;
        float track_w = ca_scrollbar_viewport_width(node) - margin * 2;
        if (track_w < 0.0f) track_w = 0.0f;
        float ratio   = ca_scrollbar_viewport_width(node) / node->content_w;
        float thumb_w = track_w * ratio;
        if (thumb_w < 16.0f * ui_s) thumb_w = 16.0f * ui_s;
        if (thumb_w > track_w) thumb_w = track_w;

        float max_scroll = ca_scrollbar_max_x(node);
        float scroll_pct = (max_scroll > 0.0f) ? node->scroll_x / max_scroll : 0.0f;
        float thumb_x    = node->x + margin + scroll_pct * (track_w - thumb_w);
        float bar_y      = node->y + node->h - bar_h - margin;

        bool dragging_x = (win->scrollbar_drag_node == node && !win->scrollbar_drag_y);
        uint32_t thumb_col_x = dragging_x
            ? (node->desc.scrollbar_thumb_active_color_set
                   ? node->desc.scrollbar_thumb_active_color
                   : CA_THEME_SCROLLBAR_THUMB_ACTIVE)
            : (node->desc.scrollbar_thumb_color_set
                   ? node->desc.scrollbar_thumb_color
                   : CA_THEME_SCROLLBAR_THUMB);
        uint32_t track_col_x = node->desc.scrollbar_track_color_set
                                   ? node->desc.scrollbar_track_color
                                   : CA_THEME_SCROLLBAR_TRACK;

        /* Track */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = node->x + margin;
            cmd->y             = bar_y;
            cmd->w             = track_w;
            cmd->h             = bar_h;
            unpack_color(track_col_x, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            cmd->corner_radius = node->desc.scrollbar_radius;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
        /* Thumb */
        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = thumb_x;
            cmd->y             = bar_y;
            cmd->w             = thumb_w;
            cmd->h             = bar_h;
            unpack_color(thumb_col_x, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            cmd->corner_radius = node->desc.scrollbar_radius;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
    }

    /* Mark every scrollbar command as overlay so they render in phase 1,
       after all phase-0 text glyphs that belong to the scroll container's
       children.  Without this the renderer draws all rects (incl. scrollbar)
       first and then all glyphs last, so child text paints over the bar. */
    for (uint32_t si = sb_first; si < win->draw_cmd_count; ++si)
        win->draw_cmds[si].overlay = true;
}

/* Helper: glyph advance for a codepoint */
static inline float glyph_adv(Ca_FontTier *tier, uint32_t cp,
                              float cs, float desired_size)
{
    Ca_FontTier *glyph_tier = tier;
    Ca_Glyph *g = ca_font_glyph_from_tier(tier, cp, &glyph_tier);
    if (!g) return 0.0f;
    float cs_eff = ca_font_glyph_cs_eff(glyph_tier, desired_size, cs);
    return g->xadvance / cs_eff;
}

/* Snap low-DPI glyph origins to physical pixels; keep HiDPI fractional. */
static inline float snap_text_position(float value, float raster_scale,
                                       float display_scale)
{
    if (display_scale > 1.25f) return value;
    float logical = value / raster_scale;
    return floorf(logical * display_scale + 0.5f) *
           (raster_scale / display_scale);
}

/* Emit glyph draw commands for a multi-line word-wrapped text string. */
static void paint_text_wrapped(Ca_Window *win, Ca_Font *font,
                               Ca_Node *node,
                               const char *text, uint32_t packed_color)
{
    if (!text || text[0] == '\0') return;
    if (!node || !node->in_use)   return;
    if (node->desc.hidden)        return;

    float r, g, b, a;
    if (packed_color == 0) r = g = b = a = 1.0f;
    else unpack_color(packed_color, &r, &g, &b, &a);

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float desired_size = node->desc.font_size > 0.0f ? node->desc.font_size : font->default_size;
    /* Select the best atlas tier for the visual (scaled) size; font_scale
       uses the CSS desired_size so glyph advances stay in layout space.  */
    Ca_FontTier *tier  = ca_font_select_tier_for_size(font, desired_size * ui_s, node->desc.font_bold);
    float font_scale   = desired_size / tier->logical_px;
    float metric_scale = font_scale * ui_s;

    float line_height = (tier->ascent - tier->descent + tier->line_gap) * metric_scale;
    if (line_height < 1.0f) line_height = desired_size * ui_s * 1.3f;

    float max_w = node->w - node->desc.padding_left - node->desc.padding_right;
    if (max_w < 1.0f) return;

    float space_adv = glyph_adv(tier, ' ', cs, desired_size);

    /* First pass: determine line breaks (word wrap). */
    float cur_line_w = 0.0f;
    int line_count = 1;
    const char *p = text;
    while (*p) {
        const char *word_start = p;
        float word_w = 0.0f;
        while (*p && *p != ' ' && *p != '\n') {
            uint32_t cp = ca_utf8_decode(&p);
            word_w += glyph_adv(tier, cp, cs, desired_size);
        }
        float with_space = (cur_line_w > 0.0f) ? cur_line_w + space_adv + word_w : word_w;
        if (cur_line_w > 0.0f && with_space > max_w) {
            line_count++;
            cur_line_w = word_w;
        } else {
            cur_line_w = with_space;
        }
        if (*p == '\n') { line_count++; cur_line_w = 0; p++; }
        else if (*p == ' ') { p++; }
    }

    /* Second pass: emit glyphs line by line */
    float start_y = node->y + node->desc.padding_top
                    + (tier->ascent * metric_scale + tier->descent * metric_scale) * 0.5f
                    + line_height * 0.5f;
    float left_x  = node->x + node->desc.padding_left;
    cur_line_w = 0.0f;
    int cur_line = 0;

    /* Keep xpos fractional so bilinear coverage sampling preserves smooth
       horizontal positioning. Snap y for a stable pixel-aligned baseline.
       xpos stays in LOGICAL space and drives word-wrap width comparisons
       against max_w — unchanged. glyph_raster_xpos is a parallel RASTER-
       space accumulator (see paint_text()'s comment for why this split
       exists) used only for the actual glyph draw position; it is reset
       in lockstep with xpos at every line break / word-wrap point below.

       Must snap/accumulate using glyph_cs_eff (line_cs_eff), not plain cs —
       cs alone omits the font_scale factor (desired_size/tier->logical_px),
       which is not 1.0 whenever the active tier's quantized logical_px
       differs from the requested desired_size (the normal case at any
       non-default ui_scale). Using cs there shifts every glyph off its
       node by a factor of font_scale — mild at font_scale≈1, but far
       enough off-node to render as missing/clipped content otherwise. */
    float line_cs_eff = ca_font_glyph_cs_eff(tier, desired_size, cs);
    float xpos = left_x;
    float glyph_raster_xpos = snap_text_position(left_x * line_cs_eff, line_cs_eff, font->display_scale);
    float baseline_y = start_y;

    ClipRect node_clip = text_clip_for_node(node);

    p = text;
    while (*p) {
        /* Measure next word */
        const char *wp = p;
        float word_w = 0.0f;
        while (*wp && *wp != ' ' && *wp != '\n') {
            uint32_t cp = ca_utf8_decode(&wp);
            word_w += glyph_adv(tier, cp, cs, desired_size);
        }
        float with_space = (cur_line_w > 0.0f) ? cur_line_w + space_adv + word_w : word_w;
        if (cur_line_w > 0.0f && with_space > max_w) {
            cur_line++;
            cur_line_w = word_w;
            xpos = left_x;
            glyph_raster_xpos = snap_text_position(left_x * line_cs_eff, line_cs_eff, font->display_scale);
            baseline_y = start_y + line_height * cur_line;
        } else {
            if (cur_line_w > 0.0f) {
                xpos += space_adv;
                glyph_raster_xpos += space_adv * line_cs_eff;
                cur_line_w += space_adv;
            }
            cur_line_w += word_w;
        }

        /* Emit glyphs for this word */
        while (p < wp) {
            uint32_t cp = ca_utf8_decode(&p);
            Ca_FontTier *glyph_tier = tier;
            Ca_Glyph *pc = ca_font_glyph_from_tier(tier, cp, &glyph_tier);
            if (!pc) continue;
            if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) goto done;

            Ca_GlyphQuad q;
            float glyph_cs_eff = ca_font_glyph_cs_eff(glyph_tier, desired_size, cs);
            float glyph_xpos = glyph_raster_xpos;
            float glyph_ypos = floorf(baseline_y * glyph_cs_eff + 0.5f);
            ca_font_get_quad(pc, font->atlas_w, font->atlas_h,
                             &glyph_xpos, &glyph_ypos, &q);
            float gw = (q.x1 - q.x0) / glyph_cs_eff;
            float gh = (q.y1 - q.y0) / glyph_cs_eff;
            float adv = pc->xadvance / glyph_cs_eff;
            if (gw < 0.5f || gh < 0.5f) {
                xpos += adv;
                glyph_raster_xpos += pc->xadvance;
                continue;
            }

            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type = CA_DRAW_GLYPH;
            cmd->font_page_index = (int16_t)glyph_tier->page_index;
            cmd->x = q.x0 / glyph_cs_eff; cmd->y = q.y0 / glyph_cs_eff;
            cmd->w = gw; cmd->h = gh;
            cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
            cmd->u0 = q.s0; cmd->v0 = q.t0;
            cmd->u1 = q.s1; cmd->v1 = q.t1;
            cmd->z_index = node->desc.z_index;
            cmd->in_use = true;
            set_clip(cmd, node_clip);
            xpos += adv;
            glyph_raster_xpos += pc->xadvance;
        }
        if (*p == '\n') {
            cur_line++;
            cur_line_w = 0;
            xpos = left_x;
            glyph_raster_xpos = snap_text_position(left_x * line_cs_eff, line_cs_eff, font->display_scale);
            baseline_y = start_y + line_height * cur_line;
            p++;
        } else if (*p == ' ') {
            p++;
        }
    }
done:
    node->content_h = node->desc.padding_top + line_height * (cur_line + 1) + node->desc.padding_bottom;
}

/* Emit glyph draw commands for a text string centred in the given node rect. */
static void paint_text(Ca_Window *win, Ca_Font *font,
                       Ca_Node *node,
                       const char *text, uint32_t packed_color)
{
    if (!text || text[0] == '\0') return;
    if (!node || !node->in_use)   return;
    if (node->desc.hidden)        return;

    /* Unlike unpack_color(), 0 here means "no explicit color" and defaults
       to opaque white rather than transparent black — do not replace this
       with a plain unpack_color() call, it would silently break that
       default for every caller that passes a zero (unset) text color. */
    float r, g, b, a;
    if (packed_color == 0) {
        r = g = b = a = 1.0f;
    } else {
        unpack_color(packed_color, &r, &g, &b, &a);
    }

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float desired_size = node->desc.font_size > 0.0f ? node->desc.font_size : font->default_size;
    Ca_FontTier *tier  = ca_font_select_tier_for_size(font, desired_size * ui_s, node->desc.font_bold);
    float font_scale   = desired_size / tier->logical_px;
    float metric_scale = font_scale * ui_s;

    /* Measure total advance width in logical pixels */
    float text_w = 0.0f;
    {
        const char *p = text;
        while (*p) {
            uint32_t cp = ca_utf8_decode(&p);
            text_w += glyph_adv(tier, cp, cs, desired_size);
        }
    }

    float baseline_logical =
        node->y + node->h * 0.5f
        + (tier->ascent * metric_scale + tier->descent * metric_scale) * 0.5f;
    float left_logical;
    if (text_w > node->w) {
        left_logical = node->x + node->desc.padding_left;
    } else {
        switch (node->desc.text_align) {
        case 1:  left_logical = node->x + (node->w - text_w) * 0.5f; break;
        case 2:  left_logical = node->x + node->w - text_w - node->desc.padding_right; break;
        default: left_logical = node->x + node->desc.padding_left; break;
        }
    }

    /* Glyph positions accumulate in RASTER space (glyph_raster_xpos), using
       each glyph's xadvance as FreeType/hinting produced it directly — this
       is what stays exact (typically a whole raster pixel for a hinted
       monospace face). Re-deriving each glyph's position by repeatedly
       converting an accumulated LOGICAL xpos through cs_eff and re-snapping
       the absolute result independently per glyph discards a fractional
       remainder every time cs_eff isn't an exact integer ratio (any
       ui_scale other than one that divides content_scale cleanly) — that
       discarded remainder does not cancel out, it compounds directionally
       across the line, which is exactly what produced visible cursor/glyph
       drift on non-1.0 ui_scale. Accumulating the already-integral raster
       advance keeps every glyph exactly adjacent to the previous one.

       The snap and the loop must both operate in the SAME raster space:
       glyph_cs_eff (which folds in font_scale = desired_size/tier->logical_px),
       not the coarser 'cs'. All glyphs on this line share one tier (fallback
       lookups render into the same tier/baked_px rather than a differently
       sized page — see ca_font_glyph_from_tier), so it is safe to compute
       glyph_cs_eff once here from the line's primary tier. Using plain 'cs'
       for the initial snap left glyph_raster_xpos off by a factor of
       font_scale — harmless at font_scale==1 but badly wrong (glyphs shifted
       far outside their node, rendering as clipped/invisible) whenever the
       active tier's logical_px differs from desired_size, which is the
       normal case at any non-default ui_scale. */
    float line_cs_eff = ca_font_glyph_cs_eff(tier, desired_size, cs);
    float glyph_raster_xpos = snap_text_position(left_logical * line_cs_eff, line_cs_eff,
                                                 font->display_scale);
    float letter_spacing_raster = node->desc.letter_spacing * line_cs_eff;

    ClipRect node_clip = text_clip_for_node(node);

    const char *p = text;
    while (*p) {
        uint32_t cp = ca_utf8_decode(&p);
        Ca_FontTier *glyph_tier = tier;
        Ca_Glyph *pc = ca_font_glyph_from_tier(tier, cp, &glyph_tier);
        if (!pc) continue;
        if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) break;

        Ca_GlyphQuad q;
        float glyph_cs_eff = ca_font_glyph_cs_eff(glyph_tier, desired_size, cs);
        float glyph_xpos = glyph_raster_xpos;
        float glyph_ypos = floorf(baseline_logical * glyph_cs_eff + 0.5f);
        ca_font_get_quad(pc, font->atlas_w, font->atlas_h,
                         &glyph_xpos, &glyph_ypos, &q);

        float gw = (q.x1 - q.x0) / glyph_cs_eff;
        float gh = (q.y1 - q.y0) / glyph_cs_eff;
        if (gw < 0.5f || gh < 0.5f) {
            glyph_raster_xpos += pc->xadvance;
            continue;
        }

        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_GLYPH;
        cmd->font_page_index = (int16_t)glyph_tier->page_index;
        cmd->x = q.x0 / glyph_cs_eff;  cmd->y = q.y0 / glyph_cs_eff;
        cmd->w = gw;          cmd->h = gh;
        cmd->r = r;  cmd->g = g;  cmd->b = b;  cmd->a = a;
        cmd->u0 = q.s0;  cmd->v0 = q.t0;
        cmd->u1 = q.s1;  cmd->v1 = q.t1;
        cmd->z_index = node->desc.z_index;
        cmd->in_use = true;
        set_clip(cmd, node_clip);
        glyph_raster_xpos += pc->xadvance + letter_spacing_raster;
    }
}

/* Emit glyph draw commands for a text string LEFT-ALIGNED in the given node rect,
   respecting padding. Used for text inputs. */
static void paint_text_left(Ca_Window *win, Ca_Font *font,
                            Ca_Node *node,
                            const char *text, uint32_t packed_color)
{
    if (!text || text[0] == '\0') return;
    if (!node || !node->in_use)   return;
    if (node->desc.hidden)        return;

    float r, g, b, a;
    if (packed_color == 0)
        r = g = b = a = 1.0f;
    else
        unpack_color(packed_color, &r, &g, &b, &a);

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float desired_size = node->desc.font_size > 0.0f ? node->desc.font_size : font->default_size;
    Ca_FontTier *tier  = ca_font_select_tier_for_size(font, desired_size * ui_s, node->desc.font_bold);
    float font_scale   = desired_size / tier->logical_px;
    float metric_scale = font_scale * ui_s;

    float baseline_logical =
        node->y + node->h * 0.5f
        + (tier->ascent * metric_scale + tier->descent * metric_scale) * 0.5f;
    float left_logical = node->x + node->desc.padding_left;

    /* See the identical raster-space-accumulation comment in paint_text():
       accumulating pc->xadvance directly (raster units, exact) instead of
       repeatedly round-tripping an absolute logical xpos through cs_eff and
       re-snapping it keeps glyphs drift-free at any ui_scale. Must snap
       using glyph_cs_eff (line_cs_eff), not plain cs — see paint_text()'s
       comment on the font_scale unit mismatch this caused when using cs. */
    float line_cs_eff = ca_font_glyph_cs_eff(tier, desired_size, cs);
    float glyph_raster_xpos = snap_text_position(left_logical * line_cs_eff, line_cs_eff,
                                                 font->display_scale);

    ClipRect input_clip = text_clip_for_node(node);

    const char *p = text;
    while (*p) {
        uint32_t cp = ca_utf8_decode(&p);
        Ca_FontTier *glyph_tier = tier;
        Ca_Glyph *pc = ca_font_glyph_from_tier(tier, cp, &glyph_tier);
        if (!pc) continue;
        if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) break;

        Ca_GlyphQuad q;
        float glyph_cs_eff = ca_font_glyph_cs_eff(glyph_tier, desired_size, cs);
        float glyph_xpos = glyph_raster_xpos;
        float glyph_ypos = floorf(baseline_logical * glyph_cs_eff + 0.5f);
        ca_font_get_quad(pc, font->atlas_w, font->atlas_h,
                         &glyph_xpos, &glyph_ypos, &q);

        float gw = (q.x1 - q.x0) / glyph_cs_eff;
        float gh = (q.y1 - q.y0) / glyph_cs_eff;
        if (gw < 0.5f || gh < 0.5f) {
            glyph_raster_xpos += pc->xadvance;
            continue;
        }

        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_GLYPH;
        cmd->font_page_index = (int16_t)glyph_tier->page_index;
        cmd->x = q.x0 / glyph_cs_eff;  cmd->y = q.y0 / glyph_cs_eff;
        cmd->w = gw;          cmd->h = gh;
        cmd->r = r;  cmd->g = g;  cmd->b = b;  cmd->a = a;
        cmd->u0 = q.s0;  cmd->v0 = q.t0;
        cmd->u1 = q.s1;  cmd->v1 = q.t1;
        cmd->in_use = true;
        set_clip(cmd, input_clip);
        glyph_raster_xpos += pc->xadvance;
    }
}

/* Measure x-advance for a substring of text (byte_count bytes) */
static float measure_text_advance(Ca_Font *font, const char *text, int byte_count,
                                  float content_scale, float ui_scale,
                                  float font_size, bool bold)
{
    float ui_s = ui_scale > 0.0f ? ui_scale : 1.0f;
    float cs   = content_scale / ui_s;
    float desired = font_size > 0.0f ? font_size : font->default_size;
    Ca_FontTier *tier = ca_font_select_tier_for_size(font, desired * ui_s, bold);
    float w = 0.0f;
    const char *p   = text;
    const char *end = text + byte_count;
    while (*p && p < end) {
        uint32_t cp = ca_utf8_decode(&p);
        w += glyph_adv(tier, cp, cs, desired);
    }
    return w;
}

/* Paint a thin cursor line at the given cursor byte offset in the input text */
static void paint_cursor(Ca_Window *win, Ca_Font *font,
                         Ca_Node *node, const char *text, int cursor_pos)
{
    if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) return;

    float advance = measure_text_advance(font, text, cursor_pos,
                                         font->content_scale, win->ui_scale,
                                         node->desc.font_size,
                                         node->desc.font_bold);
    float cursor_x = node->x + node->desc.padding_left + advance;
    float cursor_h = node->h * 0.7f;
    float cursor_y = node->y + (node->h - cursor_h) * 0.5f;

    Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type   = CA_DRAW_RECT;
    cmd->x      = cursor_x;
    cmd->y      = cursor_y;
    cmd->w      = 1.5f * (win->ui_scale > 0.0f ? win->ui_scale : 1.0f);
    cmd->h      = cursor_h;
    cmd->r = 1.0f; cmd->g = 1.0f; cmd->b = 1.0f; cmd->a = 0.9f;
    cmd->in_use = true;

    ClipRect clip = find_clip_for_node(node);
    ClipRect input_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);
    set_clip(cmd, input_clip);
}

/* Paint a focus ring (outline) around a node */
static void paint_focus_ring(Ca_Window *win, Ca_Node *node)
{
    if (!node || !node->in_use || node->desc.hidden) return;

    /* Respect the clip region of any overflow: scroll/hidden ancestor */
    ClipRect clip = find_clip_for_node(node);

    float thickness = 2.0f;
    float inset     = -2.0f; /* negative = outside */

    /* We draw 4 thin rects around the node (top, bottom, left, right) */
    struct { float x, y, w, h; } sides[4] = {
        { node->x + inset, node->y + inset, node->w - 2*inset, thickness }, /* top */
        { node->x + inset, node->y + node->h - inset - thickness, node->w - 2*inset, thickness }, /* bottom */
        { node->x + inset, node->y + inset, thickness, node->h - 2*inset }, /* left */
        { node->x + node->w - inset - thickness, node->y + inset, thickness, node->h - 2*inset }, /* right */
    };

    for (int i = 0; i < 4; ++i) {
        if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) break;
        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_RECT;
        cmd->x      = sides[i].x;
        cmd->y      = sides[i].y;
        cmd->w      = sides[i].w;
        cmd->h      = sides[i].h;
        cmd->r = 0.4f; cmd->g = 0.6f; cmd->b = 1.0f; cmd->a = 0.85f;
        cmd->in_use = true;
        set_clip(cmd, clip);
    }
}

/* ================================================================
   INCREMENTAL PAINT — cache infrastructure
   ================================================================ */

/* Copy a range of freshly-painted draw commands into the per-node cache. */
static void cache_commands(Ca_Window *win, Ca_Node *node,
                           uint32_t draw_start, uint32_t count, bool post)
{
    uint32_t *cs = post ? &node->cache_post_start : &node->cache_start;
    uint32_t *cc = post ? &node->cache_post_count : &node->cache_count;

    if (count == 0) { *cc = 0; return; }

    if (count <= *cc) {
        /* Fits in existing cache slot — overwrite in-place */
        memcpy(&win->paint_cache[*cs],
               &win->draw_cmds[draw_start],
               count * sizeof(Ca_DrawCmd));
    } else {
        if (count > UINT32_MAX - win->paint_cache_used ||
            !ca_window_reserve_paint_cache(
                win, (size_t)win->paint_cache_used + count)) {
            *cc = 0;
            return;
        }
        *cs = win->paint_cache_used;
        memcpy(&win->paint_cache[*cs],
               &win->draw_cmds[draw_start],
               count * sizeof(Ca_DrawCmd));
        win->paint_cache_used += count;
    }
    *cc = count;
}

/* ================================================================
   PAINT CACHE COMPACTION
   ================================================================
   Defragments the append-only cache pool by collecting all live spans
   (pre-children + post-children) from in-use nodes, sorting them by
   position, and copying them forward.  Dead/orphaned slots are
   reclaimed without marking any node dirty — only genuinely modified
   nodes will be repainted on the next frame.
   ================================================================ */

void ca_paint_cache_compact(Ca_Window *win)
{
    /* Span descriptor — points back into the node so we can update offsets */
    typedef struct { uint32_t start; uint32_t count; uint32_t *p_start; } CacheSpan;

    size_t node_slots = ca_pool_slot_count(&win->node_pool);
    if (node_slots > SIZE_MAX / 2u) return;
    size_t maximum_spans = node_slots * 2u;
    if (win->paint_cache_spans.element_size == 0 &&
        !ca_dyn_array_init(&win->paint_cache_spans, sizeof(CacheSpan)))
        return;
    if (!ca_dyn_array_resize(&win->paint_cache_spans, maximum_spans)) return;
    CacheSpan *spans = win->paint_cache_spans.data;
    size_t span_count = 0;

    for (size_t i = 0; i < node_slots; ++i) {
        Ca_Node *n = CA_POOL_AT(win->node_pool, Ca_Node, i);
        if (!n->in_use) continue;
        if (n->cache_count > 0 && span_count < maximum_spans)
            spans[span_count++] = (CacheSpan){ n->cache_start, n->cache_count, &n->cache_start };
        if (n->cache_post_count > 0 && span_count < maximum_spans)
            spans[span_count++] = (CacheSpan){ n->cache_post_start, n->cache_post_count, &n->cache_post_start };
    }

    if (span_count == 0) {
        win->paint_cache_used = 0;
        return;
    }

    /* Insertion sort by start position (runs only during infrequent compaction) */
    for (size_t i = 1; i < span_count; ++i) {
        CacheSpan tmp = spans[i];
        size_t j = i;
        while (j > 0 && spans[j - 1].start > tmp.start) {
            spans[j] = spans[j - 1];
            j--;
        }
        spans[j] = tmp;
    }

    /* Compact forward — dest <= source always holds because we
       process spans in ascending start order. */
    uint32_t dest = 0;
    for (size_t i = 0; i < span_count; ++i) {
        if (spans[i].start != dest) {
            memmove(&win->paint_cache[dest],
                    &win->paint_cache[spans[i].start],
                    spans[i].count * sizeof(Ca_DrawCmd));
        }
        *spans[i].p_start = dest;
        dest += spans[i].count;
    }

    win->paint_cache_used = dest;
}

/* DFS tree walk with per-node paint caching.
   - Dirty nodes: paint fresh → cache → commands already in draw_cmds
   - Clean nodes: copy from cache → draw_cmds */

static void clear_dirty_recursive(Ca_Node *node)
{
    if (!node || !node->in_use) return;
    node->dirty &= ~CA_DIRTY_CONTENT;
    for (uint32_t i = 0; i < node->child_count; ++i)
        clear_dirty_recursive(node->children[i]);
}

/* True when a node is entirely outside an active clip rect.
   Used to cull off-screen nodes inside scroll containers so we don't
   waste draw command budget on content that will never be visible. */
static bool node_outside_clip(const Ca_Node *node, ClipRect clip)
{
    if (!clip.active) return false;
    return (node->x + node->w <= clip.x  ||
            node->x            >= clip.x + clip.w ||
            node->y + node->h <= clip.y  ||
            node->y            >= clip.y + clip.h);
}

/* Apply effective_z to freshly-emitted or replayed draw commands in the
   range [start, start+count).  Only commands with z_index==0 are updated
   so that any explicitly-set positive z on a child is always preserved. */
static void apply_inherited_z(Ca_Window *win, uint32_t start, uint32_t count,
                              int16_t effective_z)
{
    if (effective_z == 0) return;
    for (uint32_t ci = start; ci < start + count; ++ci)
        if (win->draw_cmds[ci].z_index == 0)
            win->draw_cmds[ci].z_index = effective_z;
}

/* Stamp the accumulated subtree transform onto freshly-emitted or replayed
   draw commands in [start, start+count).  Applied unconditionally (not only
   to untransformed commands, unlike inherited z) because a command's
   transform is always fully determined by its node's position in the tree,
   and cached commands replay with whatever transform was current when they
   were first painted. */
/* `replayed` must be true for commands copied out of the paint cache: those
   carry whatever transform was current when they were first painted, so an
   identity transform still has to be written back over them. Freshly
   emitted commands are memset to zero by paint_node_content, so the
   identity case can skip the stores entirely — which is the common path,
   since almost nothing in a typical tree is transformed. */
static void apply_transform(Ca_Window *win, uint32_t start, uint32_t count,
                            Ca_Transform2D xf, bool replayed)
{
    if (!xf.active && !replayed) return;
    for (uint32_t ci = start; ci < start + count; ++ci) {
        Ca_DrawCmd *cmd = &win->draw_cmds[ci];
        cmd->xf_active = xf.active;
        cmd->xf_a  = xf.a;  cmd->xf_b  = xf.b;
        cmd->xf_c  = xf.c;  cmd->xf_d  = xf.d;
        cmd->xf_tx = xf.tx; cmd->xf_ty = xf.ty;
    }
}

static void paint_tree_cached(Ca_Instance *inst, Ca_Window *win,
                              Ca_Node *node, ClipRect clip, int16_t inherited_z,
                              Ca_Transform2D inherited_xf)
{
    if (!node || !node->in_use) return;

    /* Hidden nodes produce no draw commands.  Clear dirty flags on the
       entire subtree so they don't perpetually trigger paint passes. */
    if (node->desc.hidden) {
        clear_dirty_recursive(node);
        return;
    }

    /* Nodes fully outside the current clip (e.g. scrolled out of view) are
       culled: no draw commands are generated.  Dirty flags are cleared so
       the incremental-paint scan doesn't trigger phantom paint passes every
       frame.  When the node scrolls back into view, layout_and_invalidate
       will re-mark it dirty via position-change detection. */
    if (node_outside_clip(node, clip)) {
        clear_dirty_recursive(node);
        return;
    }

    /* Effective z: use this node's own z_index if set, otherwise inherit
       from the parent.  This ensures all draw commands inside a z>0 subtree
       (e.g. sticky-header buttons and their text children) are placed in the
       correct overlay phase rather than rendering behind normal-phase content. */
    int16_t effective_z = (node->desc.z_index != 0) ? node->desc.z_index
                                                     : inherited_z;

    /* Effective transform: this node's own rotation/scale about its pivot,
       composed under whatever its transformed ancestors already contribute.
       Computed from the node's laid-out box, so it tracks layout changes
       without any extra invalidation. */
    Ca_Transform2D effective_xf = ca_transform_mul(
        inherited_xf,
        ca_transform_from_desc(&node->desc, node->x, node->y,
                               node->w, node->h));

    bool was_dirty = (node->dirty & CA_DIRTY_CONTENT) != 0;

    /* ---- Pre-children: background + widget visuals ---- */
    if (was_dirty) {
        uint32_t start = win->draw_cmd_count;
        paint_node_content(win, inst->font, node, clip);
        uint32_t count = win->draw_cmd_count - start;
        apply_inherited_z(win, start, count, effective_z);
        apply_transform(win, start, count, effective_xf, false);
        cache_commands(win, node, start, count, false);
        node->dirty &= ~CA_DIRTY_CONTENT;
        if (win->debug_overlay && !win->dbg_force_repaint)
            node->dbg_repainted = true;
    } else if (node->cache_count > 0 &&
               ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + node->cache_count))
    {
        uint32_t replay_start = win->draw_cmd_count;
        memcpy(&win->draw_cmds[win->draw_cmd_count],
               &win->paint_cache[node->cache_start],
               node->cache_count * sizeof(Ca_DrawCmd));
        node->draw_cmd_idx = (int32_t)win->draw_cmd_count;
        win->draw_cmd_count += node->cache_count;
        touch_font_pages_for_cmds(inst->font,
                                  &win->draw_cmds[replay_start],
                                  node->cache_count);
        apply_inherited_z(win, replay_start, node->cache_count, effective_z);
        apply_transform(win, replay_start, node->cache_count, effective_xf, true);
    }

    /* ---- Child clip ---- */
    ClipRect child_clip = clip;
    if (node->desc.overflow_x >= 1 || node->desc.overflow_y >= 1)
        child_clip = clip_intersect(clip, node->x, node->y,
                                    ca_scrollbar_viewport_width(node),
                                    ca_scrollbar_viewport_height(node));

    /* ---- Recurse children (propagate effective_z and transform down) ---- */
    for (uint32_t i = 0; i < node->child_count; ++i)
        paint_tree_cached(inst, win, node->children[i], child_clip,
                          effective_z, effective_xf);

    /* ---- Post-children: scrollbars ---- */
    if (was_dirty) {
        uint32_t sb_start = win->draw_cmd_count;
        paint_scrollbars(win, node, clip);
        uint32_t sb_count = win->draw_cmd_count - sb_start;
        apply_inherited_z(win, sb_start, sb_count, effective_z);
        apply_transform(win, sb_start, sb_count, effective_xf, false);
        cache_commands(win, node, sb_start, sb_count, true);
    } else if (node->cache_post_count > 0 &&
               ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + node->cache_post_count))
    {
        uint32_t sb_replay = win->draw_cmd_count;
        memcpy(&win->draw_cmds[win->draw_cmd_count],
               &win->paint_cache[node->cache_post_start],
               node->cache_post_count * sizeof(Ca_DrawCmd));
        win->draw_cmd_count += node->cache_post_count;
        touch_font_pages_for_cmds(inst->font,
                                  &win->draw_cmds[sb_replay],
                                  node->cache_post_count);
        apply_inherited_z(win, sb_replay, node->cache_post_count, effective_z);
        apply_transform(win, sb_replay, node->cache_post_count, effective_xf, true);
    }
}

/* ================================================================
   OVERLAY PASS — always regenerated (not cached)
   ================================================================ */

static void paint_overlays(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Font *font = inst->font;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    const bool use_fallback_chrome = inst->stylesheet == NULL;

    /* ---- Select dropdown overlays ---- */
    if (ca_pool_slot_count(&win->select_pool) > 0 && font) {
        for (uint32_t i = 0; i < ca_pool_slot_count(&win->select_pool); ++i) {
            Ca_Select *sel = CA_POOL_AT(win->select_pool, Ca_Select, i);
            if (!sel->in_use || !sel->node) continue;
            /* If the host widget (or any ancestor panel) is hidden, force-close
               the dropdown so it doesn't ghost-render at stale coordinates. */
            if (node_is_ancestor_hidden(sel->node)) {
                sel->open = false;
                continue;
            }
            if (!sel->open) continue;
            Ca_Node *n = sel->node;
            const OverlayCssStyle popup_style = overlay_css_style(
                win, n, "ca-select-popup", CA_THEME_POPUP_BG,
                CA_THEME_POPUP_TEXT, 4.0f * ui_s);
            const OverlayCssStyle hover_style = overlay_css_style(
                win, n, "ca-overlay-hover", CA_THEME_BG_SURFACE,
                popup_style.color, 0.0f);
            const OverlayCssStyle selected_style = overlay_css_style(
                win, n, "ca-overlay-selected", CA_THEME_BG_OVERLAY,
                popup_style.color, 0.0f);

            float opt_h   = n->h;
            float drop_y  = n->y + n->h;
            int   visible = sel->option_count < CA_SELECT_MAX_VISIBLE
                            ? sel->option_count : CA_SELECT_MAX_VISIBLE;
            int   scroll  = sel->scroll_offset;

            /* Clamp scroll so we never exceed available options */
            if (scroll > sel->option_count - visible) scroll = sel->option_count - visible;
            if (scroll < 0) scroll = 0;

            /* Dropdown background — height capped to visible rows */
            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x; c->y = drop_y;
                c->w = n->w; c->h = opt_h * (float)visible;
                c->corner_radius = popup_style.radius;
                unpack_color(popup_style.background, &c->r, &c->g, &c->b, &c->a);
                if (use_fallback_chrome) {
                    c->border_width = 1.0f * ui_s;
                    c->border_r = 0.200f; c->border_g = 0.200f;
                    c->border_b = 0.267f; c->border_a = 1.0f;
                }
                c->in_use = true;
                c->overlay = true;
            }
            /* Track which option is under the cursor; fire on_hover when it changes. */
            int new_hover = -1;
            if (win->mouse_x >= 0) {
                for (int vi = 0; vi < visible; ++vi) {
                    int oi = scroll + vi;
                    if (oi >= sel->option_count) break;
                    float oy = drop_y + opt_h * (float)vi;
                    if ((double)n->x <= win->mouse_x && win->mouse_x <= (double)(n->x + n->w) &&
                        (double)oy   <= win->mouse_y && win->mouse_y <= (double)(oy + opt_h)) {
                        new_hover = oi;
                        break;
                    }
                }
            }
            if (new_hover != sel->hover_item) {
                sel->hover_item = new_hover;
                if (sel->on_hover) sel->on_hover(sel, sel->hover_data);
            }

            /* Options — only the visible window */
            for (int vi = 0; vi < visible; ++vi) {
                int oi = scroll + vi;
                if (oi >= sel->option_count) break;
                float oy = drop_y + opt_h * (float)vi;
                bool is_selected = (oi == sel->selected);
                bool is_hovered  = (oi == sel->hover_item);
                if ((is_selected || is_hovered) && ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                    Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                    memset(c, 0, sizeof(*c));
                    c->type = CA_DRAW_RECT;
                    c->x = n->x; c->y = oy; c->w = n->w; c->h = opt_h;
                    const uint32_t fill = is_selected
                        ? selected_style.background : hover_style.background;
                    unpack_color(fill, &c->r, &c->g, &c->b, &c->a);
                    c->in_use = true;
                    c->overlay = true;
                }
                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp = *n;
                tmp.parent = NULL; /* break parent chain — overlay must not inherit parent clip */
                tmp.x = n->x; tmp.y = oy; tmp.h = opt_h;
                paint_text(win, font, &tmp, sel->options[oi], popup_style.color);
                for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                    win->draw_cmds[gi].overlay = true;
            }
        }
    }

    /* ---- Tooltips ---- */
    if (ca_pool_slot_count(&win->tooltip_pool) > 0 && font &&
        win->hovered_node) {
        float cs   = font->content_scale / ui_s;

        /* Logical window size for bounds clamping (same coordinate space as
           node x/y and mouse_x/y — differs from sc.extent on HiDPI displays). */
        int tooltip_win_w = 0, tooltip_win_h = 0;
        if (win->glfw)
            glfwGetWindowSize(win->glfw, &tooltip_win_w, &tooltip_win_h);

        for (uint32_t i = 0; i < ca_pool_slot_count(&win->tooltip_pool); ++i) {
            Ca_Tooltip *tt = CA_POOL_AT(win->tooltip_pool, Ca_Tooltip, i);
            if (!tt->in_use || !tt->node || tt->text[0] == '\0') continue;
            Ca_Node *hover = win->hovered_node;
            bool match = false;
            while (hover) {
                if (hover == tt->node) { match = true; break; }
                hover = hover->parent;
            }
            if (!match) continue;
            const OverlayCssStyle tooltip_style = overlay_css_style(
                win, tt->node, "ca-tooltip", CA_THEME_POPUP_BG,
                CA_THEME_POPUP_TEXT, 3.0f * ui_s);

            /* Font size resolved at widget-build time and cached in the slot */
            float tooltip_fs  = tt->font_size > 0.0f ? tt->font_size : font->default_size;
            Ca_FontTier *tier = ca_font_select_tier_for_size(font, tooltip_fs * ui_s, false);
            float font_scale  = tooltip_fs / tier->logical_px;
            float metric_scale = font_scale * ui_s;
            float pad         = 5.0f * ui_s;
            float text_h      = (tier->ascent - tier->descent) * metric_scale;
            float tip_h       = text_h + pad * 2.0f;

            /* Measure text width at the resolved font scale */
            float tw = 0.0f;
            {
                const char *tp = tt->text;
                while (*tp) {
                    uint32_t cp = ca_utf8_decode(&tp);
                    tw += glyph_adv(tier, cp, cs, tooltip_fs);
                }
            }

            float tip_w = tw + pad * 2.0f;

            /* Follow the cursor (Qt / ImGui style): place below-right by default,
               flip sides when the tooltip would extend past a window edge. */
            float tip_x = (float)win->mouse_x + 12.0f * ui_s;
            float tip_y = (float)win->mouse_y + 16.0f * ui_s;
            if (tooltip_win_w > 0 && tip_x + tip_w > (float)tooltip_win_w)
                tip_x = (float)win->mouse_x - tip_w - 4.0f * ui_s;
            if (tooltip_win_h > 0 && tip_y + tip_h > (float)tooltip_win_h)
                tip_y = (float)win->mouse_y - tip_h - 4.0f * ui_s;
            if (tip_x < 0.0f) tip_x = 0.0f;
            if (tip_y < 0.0f) tip_y = 0.0f;

            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = tip_x; c->y = tip_y; c->w = tip_w; c->h = tip_h;
                c->corner_radius = tooltip_style.radius;
                unpack_color(tooltip_style.background, &c->r, &c->g, &c->b, &c->a);
                if (use_fallback_chrome) {
                    c->border_width = 1.0f * ui_s;
                    c->border_r = 0.200f; c->border_g = 0.200f;
                    c->border_b = 0.267f; c->border_a = 1.0f;
                }
                c->in_use = true;
                c->overlay = true;
            }
            uint32_t glyph_start = win->draw_cmd_count;
            Ca_Node tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.in_use = true;
            tmp.x = tip_x; tmp.y = tip_y; tmp.w = tip_w; tmp.h = tip_h;
            tmp.desc.font_size    = tooltip_fs;
            tmp.desc.padding_left = pad;
            tmp.window = win;
            paint_text(win, font, &tmp, tt->text, tooltip_style.color);
            for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                win->draw_cmds[gi].overlay = true;
        }
    }

    /* ---- Context menus ---- */
    if (ca_pool_slot_count(&win->ctxmenu_pool) > 0 && font) {
        const float item_h  = 24.0f * ui_s;
        const float sep_h   =  8.0f * ui_s;
        const float pad_x   = 12.0f * ui_s;
        const float menu_w  = 180.0f * ui_s;

        for (uint32_t i = 0; i < ca_pool_slot_count(&win->ctxmenu_pool); ++i) {
            Ca_CtxMenu *cm = CA_POOL_AT(win->ctxmenu_pool, Ca_CtxMenu, i);
            if (!cm->in_use || !cm->open || cm->item_count <= 0) continue;
            /* If the owning node (or any ancestor panel) is hidden, force-close
               the menu so it doesn't reappear at stale geometry when the panel
               becomes visible again — same rule select dropdowns follow above. */
            if (cm->node && node_is_ancestor_hidden(cm->node)) {
                cm->open = false;
                continue;
            }
            const OverlayCssStyle menu_style = overlay_css_style(
                win, cm->node, "ca-context-menu", CA_THEME_POPUP_BG,
                CA_THEME_POPUP_TEXT, 4.0f * ui_s);
            const OverlayCssStyle item_hover_style = overlay_css_style(
                win, cm->node, "ca-overlay-hover", CA_THEME_BG_OVERLAY,
                menu_style.color, 0.0f);

            /* Compute total height (items + separators) */
            float menu_h = 6.0f * ui_s; /* top + bottom inset */
            for (int mi = 0; mi < cm->item_count; ++mi) {
                bool is_sep = (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
                menu_h += is_sep ? sep_h : item_h;
            }

            /* Clamp to window edges */
            float mx_pos = cm->open_x;
            float my_pos = cm->open_y;
            if (win->sc.extent.width  > 0 && mx_pos + menu_w > (float)win->sc.extent.width)
                mx_pos = (float)win->sc.extent.width  - menu_w;
            if (win->sc.extent.height > 0 && my_pos + menu_h > (float)win->sc.extent.height)
                my_pos = (float)win->sc.extent.height - menu_h;
            if (mx_pos < 0) mx_pos = 0;
            if (my_pos < 0) my_pos = 0;

            /* Background */
            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = mx_pos; c->y = my_pos;
                c->w = menu_w; c->h = menu_h;
                c->corner_radius = menu_style.radius;
                unpack_color(menu_style.background, &c->r, &c->g, &c->b, &c->a);
                if (use_fallback_chrome) {
                    c->border_width = 1.0f * ui_s;
                    c->border_r = 0.200f; c->border_g = 0.200f;
                    c->border_b = 0.267f; c->border_a = 1.0f;
                }
                c->in_use = true;
                c->overlay = true;
            }

            /* Items */
            float iy = my_pos + 3.0f * ui_s; /* top inset */
            for (int mi = 0; mi < cm->item_count; ++mi) {
                bool is_sep = (cm->items[mi][0] == '-' && cm->items[mi][1] == '\0');
                if (is_sep) {
                    /* Separator line */
                    if (use_fallback_chrome &&
                        ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                        Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                        memset(c, 0, sizeof(*c));
                        c->type = CA_DRAW_RECT;
                        c->x = mx_pos + 8.0f * ui_s; c->y = iy + sep_h * 0.5f - 0.5f * ui_s;
                        c->w = menu_w - 16.0f * ui_s; c->h = 1.0f * ui_s;
                        { float _r, _g, _b, _a; unpack_color(CA_THEME_POPUP_BORDER, &_r, &_g, &_b, &_a); c->r = _r; c->g = _g; c->b = _b; c->a = _a; }
                        c->in_use = true;
                        c->overlay = true;
                    }
                    iy += sep_h;
                    continue;
                }

                /* Hover highlight */
                bool hovered = ((double)mx_pos <= win->mouse_x &&
                                win->mouse_x   <= (double)(mx_pos + menu_w) &&
                                (double)iy     <= win->mouse_y &&
                                win->mouse_y   <= (double)(iy + item_h));
                if (hovered && ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                    Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                    memset(c, 0, sizeof(*c));
                    c->type = CA_DRAW_RECT;
                    c->x = mx_pos + 2.0f * ui_s; c->y = iy;
                    c->w = menu_w - 4.0f * ui_s; c->h = item_h;
                    c->corner_radius = item_hover_style.radius;
                    unpack_color(item_hover_style.background,
                                 &c->r, &c->g, &c->b, &c->a);
                    c->in_use = true;
                    c->overlay = true;
                }

                /* Item label */
                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.in_use = true;
                tmp.x = mx_pos + pad_x;
                tmp.y = iy;
                tmp.w = menu_w - pad_x * 2.0f;
                tmp.h = item_h;
                tmp.window = win;
                tmp.desc.font_size = 12.0f;
                paint_text(win, font, &tmp, cm->items[mi], menu_style.color);
                for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                    win->draw_cmds[gi].overlay = true;

                iy += item_h;
            }
        }
    }

    /* ---- Menu bar dropdowns ---- */
    if (ca_pool_slot_count(&win->menubar_pool) > 0 && font) {
        for (uint32_t i = 0; i < ca_pool_slot_count(&win->menubar_pool); ++i) {
            Ca_MenuBar *mb = CA_POOL_AT(win->menubar_pool, Ca_MenuBar, i);
            if (!mb->in_use || !mb->node || mb->active_menu < 0) continue;
            /* If the menu bar (or any ancestor panel) is hidden, force-close
               the open dropdown so it doesn't reappear at stale geometry when
               the panel becomes visible again — same rule select dropdowns
               and context menus follow above. */
            if (node_is_ancestor_hidden(mb->node)) {
                mb->active_menu = -1;
                continue;
            }

            /* Highlight the active header */
            Ca_MenuBarMenu *am = &mb->menus[mb->active_menu];
            Ca_Node *hdr = am->header_node;
            if (!hdr) continue;
            const OverlayCssStyle menu_style = overlay_css_style(
                win, mb->node, "ca-menubar-popup", mb->dropdown_bg,
                mb->dropdown_text, 0.0f);
            const OverlayCssStyle item_hover_style = overlay_css_style(
                win, mb->node, "ca-overlay-hover", mb->dropdown_hover,
                menu_style.color, 0.0f);
            const OverlayCssStyle active_style = overlay_css_style(
                win, mb->node, "ca-overlay-selected", mb->header_highlight,
                mb->text_color, 0.0f);

            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = hdr->x; c->y = hdr->y;
                c->w = hdr->w; c->h = hdr->h;
                unpack_color(active_style.background, &c->r, &c->g, &c->b, &c->a);
                c->in_use = true;
                c->overlay = true;
            }

            /* Re-paint the header label on top of the highlight rect */
            {
                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.in_use = true;
                tmp.x = hdr->x; tmp.y = hdr->y;
                tmp.w = hdr->w; tmp.h = hdr->h;
                tmp.window = win;
                tmp.desc.text_align = 1; /* centered */
                tmp.desc.font_size = mb->item_font_size > 0.0f ? mb->item_font_size : 12.0f;
                paint_text(win, font, &tmp, am->label, active_style.color);
                for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                    win->draw_cmds[gi].overlay = true;
            }

            const float sep_h = 8.0f * ui_s;
            float item_h = 24.0f * ui_s;
            float menu_w = 180.0f * ui_s;
            float drop_item_fs = mb->item_font_size > 0.0f ? mb->item_font_size : 12.0f;
            float drop_x = 0.0f;
            float drop_y = 0.0f;

            /* Compute total menu height (6px top+bottom inset, same as ctx menus) */
            float menu_h = 6.0f * ui_s;
            for (int ii = 0; ii < am->item_count; ++ii)
                menu_h += am->items[ii].separator ? sep_h : item_h;
            ca_menubar_dropdown_geometry(win, hdr, menu_w, menu_h,
                                         &drop_x, &drop_y);

            /* Dropdown background */
            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = drop_x; c->y = drop_y;
                c->w = menu_w; c->h = menu_h;
                c->corner_radius = menu_style.radius;
                unpack_color(menu_style.background, &c->r, &c->g, &c->b, &c->a);
                c->in_use = true;
                c->overlay = true;
                if (use_fallback_chrome) {
                    c->border_width = 1.0f * ui_s;
                    unpack_color(mb->dropdown_border,
                                 &c->border_r, &c->border_g,
                                 &c->border_b, &c->border_a);
                }
            }

            /* Dropdown items */
            float iy = drop_y + 3.0f * ui_s; /* 3px top inset, same as ctx menus */
            for (int ii = 0; ii < am->item_count; ++ii) {
                float this_h = am->items[ii].separator ? sep_h : item_h;

                /* --- Separator --- */
                if (am->items[ii].separator) {
                    if (use_fallback_chrome &&
                        ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                        Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                        memset(c, 0, sizeof(*c));
                        c->type = CA_DRAW_RECT;
                        c->x    = drop_x + 8.0f * ui_s;
                        c->y    = iy + this_h * 0.5f - 0.5f * ui_s;
                        c->w    = menu_w - 16.0f * ui_s;
                        c->h    = 1.0f * ui_s;
                        unpack_color(mb->dropdown_border, &c->r, &c->g, &c->b, &c->a);
                        c->in_use  = true;
                        c->overlay = true;
                    }
                    iy += this_h;
                    continue;
                }

                /* Hover highlight — also highlight when this item's sub-menu is open */
                if (mb->hover_item == ii || am->active_sub == ii) {
                    if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                        Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                        memset(c, 0, sizeof(*c));
                        c->type = CA_DRAW_RECT;
                        c->x = drop_x + 2.0f * ui_s; c->y = iy;
                        c->w = menu_w - 4.0f * ui_s; c->h = this_h;
                        c->corner_radius = 0.0f;
                        unpack_color(item_hover_style.background,
                                     &c->r, &c->g, &c->b, &c->a);
                        c->in_use  = true;
                        c->overlay = true;
                    }
                }

                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.in_use = true;
                tmp.x = drop_x + 12.0f * ui_s;
                tmp.y = iy;
                tmp.w = menu_w - 24.0f * ui_s;
                tmp.h = this_h;
                tmp.window = win;
                tmp.desc.text_align = 0; /* left-align */
                tmp.desc.font_size = drop_item_fs;
                paint_text(win, font, &tmp, am->items[ii].label,
                           menu_style.color);
                for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                    win->draw_cmds[gi].overlay = true;

                /* Chevron arrow for items with a sub-menu */
                if (am->items[ii].sub_item_count > 0) {
                    uint32_t gs2 = win->draw_cmd_count;
                    Ca_Node tmp2;
                    memset(&tmp2, 0, sizeof(tmp2));
                    tmp2.in_use = true;
                    tmp2.x = drop_x + menu_w - 20.0f * ui_s;
                    tmp2.y = iy;
                    tmp2.w = 16.0f * ui_s;
                    tmp2.h = this_h;
                    tmp2.window = win;
                    tmp2.desc.text_align = 0;
                    tmp2.desc.font_size = drop_item_fs;
                    paint_text(win, font, &tmp2,
                               CA_ICON_NF_COD_CHEVRON_RIGHT,
                               menu_style.color);
                    for (uint32_t gi = gs2; gi < win->draw_cmd_count; ++gi)
                        win->draw_cmds[gi].overlay = true;
                }

                iy += this_h;
            }

            /* Sub-menu panel (shown when active_sub >= 0) */
            if (am->active_sub >= 0 && am->active_sub < am->item_count) {
                int   asi        = am->active_sub;
                float sub_item_h = 24.0f * ui_s;
                float sub_menu_w = 180.0f * ui_s;
                float parent_offset_y = 3.0f * ui_s;
                for (int jj = 0; jj < asi; ++jj)
                    parent_offset_y += am->items[jj].separator ? sep_h : item_h;
                float sub_h      = sub_item_h * (float)am->items[asi].sub_item_count + 6.0f * ui_s;
                float sub_x = 0.0f;
                float sub_y = 0.0f;
                ca_menubar_submenu_geometry(win, drop_x, drop_y,
                                            menu_w, sub_menu_w, sub_h,
                                            parent_offset_y,
                                            &sub_x, &sub_y);

                /* Sub-menu background */
                if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                    Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                    memset(c, 0, sizeof(*c));
                    c->type = CA_DRAW_RECT;
                    c->x = sub_x; c->y = sub_y;
                    c->w = sub_menu_w; c->h = sub_h;
                    c->corner_radius = menu_style.radius;
                    unpack_color(menu_style.background,
                                 &c->r, &c->g, &c->b, &c->a);
                    c->in_use      = true;
                    c->overlay     = true;
                    if (use_fallback_chrome) {
                        c->border_width = 1.0f * ui_s;
                        unpack_color(mb->dropdown_border,
                                     &c->border_r, &c->border_g,
                                     &c->border_b, &c->border_a);
                    }
                }

                float sub_iy = sub_y + 3.0f * ui_s; /* 3px top inset */
                for (int si = 0; si < am->items[asi].sub_item_count; ++si) {
                    float siy = sub_iy + sub_item_h * (float)si;

                    /* Hover highlight */
                    if (mb->hover_sub_item == si) {
                        if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                            memset(c, 0, sizeof(*c));
                            c->type = CA_DRAW_RECT;
                            c->x = sub_x + 2.0f * ui_s; c->y = siy;
                            c->w = sub_menu_w - 4.0f * ui_s; c->h = sub_item_h;
                            c->corner_radius = 0.0f;
                            unpack_color(item_hover_style.background,
                                         &c->r, &c->g, &c->b, &c->a);
                            c->in_use  = true;
                            c->overlay = true;
                        }
                    }

                    /* Sub-item text */
                    uint32_t gs = win->draw_cmd_count;
                    Ca_Node stmp;
                    memset(&stmp, 0, sizeof(stmp));
                    stmp.in_use = true;
                    stmp.x = sub_x + 12.0f * ui_s;
                    stmp.y = siy;
                    stmp.w = sub_menu_w - 24.0f * ui_s;
                    stmp.h = sub_item_h;
                    stmp.window = win;
                    stmp.desc.text_align = 0;
                    stmp.desc.font_size = drop_item_fs;
                    paint_text(win, font, &stmp,
                               am->items[asi].sub_items[si].label,
                               menu_style.color);
                    for (uint32_t gi = gs; gi < win->draw_cmd_count; ++gi)
                        win->draw_cmds[gi].overlay = true;
                }
            }
        }
    }

    /* ---- Modals ---- */
    if (ca_pool_slot_count(&win->modal_pool) > 0) {
        for (uint32_t i = 0; i < ca_pool_slot_count(&win->modal_pool); ++i) {
            Ca_Modal *m = CA_POOL_AT(win->modal_pool, Ca_Modal, i);
            if (!m->in_use || !m->visible) continue;
            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                int lw, lh;
                glfwGetWindowSize(win->glfw, &lw, &lh);
                float or_r, or_g, or_b, or_a;
                unpack_color(m->overlay_color, &or_r, &or_g, &or_b, &or_a);
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = 0; c->y = 0; c->w = (float)lw; c->h = (float)lh;
                c->r = or_r; c->g = or_g; c->b = or_b; c->a = or_a;
                c->in_use = true;
                c->overlay = true;
            }
        }
    }
}

/* ================================================================
   DEBUG OVERLAY — toggled by F9, shows rendering stats
   ================================================================ */

static void paint_debug_overlay(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Font *font = inst->font;
    if (!font) return;

    /* --- Paint-flash: green tinted rect over nodes repainted THIS frame --- */
    uint32_t repainted_count = 0;
    if (ca_pool_slot_count(&win->node_pool) > 0) {
        for (uint32_t i = 0; i < ca_pool_slot_count(&win->node_pool); ++i) {
            Ca_Node *n = CA_POOL_AT(win->node_pool, Ca_Node, i);
            if (!n->in_use || !n->dbg_repainted) continue;
            if (n->w < 1.0f || n->h < 1.0f) continue;
            if (!ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) break;

            /* Green tint overlay on repainted node */
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = n->x;  c->y = n->y;
            c->w = n->w;  c->h = n->h;
            c->r = 0.0f;  c->g = 1.0f;  c->b = 0.0f;  c->a = 0.18f;
            c->corner_radius = n->desc.corner_radius;
            c->in_use  = true;
            c->overlay = true;

            /* Green border to make it clearly visible */
            if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
                Ca_DrawCmd *b = &win->draw_cmds[win->draw_cmd_count++];
                memset(b, 0, sizeof(*b));
                b->type = CA_DRAW_RECT;
                b->x = n->x;  b->y = n->y;
                b->w = n->w;  b->h = n->h;
                b->r = 0.0f;  b->g = 0.0f;  b->b = 0.0f;  b->a = 0.0f;
                b->corner_radius = n->desc.corner_radius;
                b->border_width  = 1.5f;
                b->border_r = 0.0f; b->border_g = 1.0f;
                b->border_b = 0.0f; b->border_a = 0.9f;
                b->in_use  = true;
                b->overlay = true;
            }
            repainted_count++;
            n->dbg_repainted = false;
        }
    }

    /* If we drew green rects, schedule a follow-up paint pass to clear them.
       Frame N: green shown.  Frame N+1: green gone (flags already cleared). */
    if (repainted_count > 0) {
        win->dbg_force_repaint = true;
        glfwPostEmptyEvent();
    }

    /* --- Stats panel --- */

    /* Count active nodes */
    uint32_t node_count = 0;
    if (ca_pool_slot_count(&win->node_pool) > 0) {
        for (uint32_t i = 0; i < ca_pool_slot_count(&win->node_pool); ++i)
            if (CA_POOL_AT(win->node_pool, Ca_Node, i)->in_use) node_count++;
    }
    win->dbg_node_count = node_count;

    /* Process RSS in MB */
    double rss_mb = 0;
#ifdef __APPLE__
    {
        struct mach_task_basic_info info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      (task_info_t)&info, &count) == KERN_SUCCESS)
            rss_mb = (double)info.resident_size / (1024.0 * 1024.0);
    }
#elif defined(__linux__)
    {
        FILE *f = fopen("/proc/self/status", "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                long kb;
                if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) {
                    rss_mb = (double)kb / 1024.0;
                    break;
                }
            }
            fclose(f);
        }
    }
#elif defined(_WIN32)
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            rss_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
#endif

    /* GPU type string */
    const char *gpu_type_str = "Unknown";
    switch (inst->gpu_type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: gpu_type_str = "Integrated"; break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   gpu_type_str = "Discrete";   break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    gpu_type_str = "Virtual";    break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            gpu_type_str = "CPU";        break;
        default: break;
    }

    /* Present mode string */
    const char *pm_str = "FIFO";
    switch (inst->present_mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    pm_str = "Immediate";    break;
        case VK_PRESENT_MODE_MAILBOX_KHR:      pm_str = "Mailbox";      break;
        case VK_PRESENT_MODE_FIFO_KHR:         pm_str = "FIFO (VSync)"; break;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: pm_str = "FIFO Relaxed"; break;
        default: break;
    }

    /* Panel position & sizing */
    float pad   = 8.0f;
    float line_h = 16.0f;
    int   n_lines = 26;
    float panel_w = 290.0f;
    float panel_h = pad + line_h * (float)n_lines + pad;
    float panel_x = pad;
    float panel_y = pad;

    /* Background rect */
    if (ca_window_reserve_draw_commands(win, (size_t)win->draw_cmd_count + 1u)) {
        Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
        memset(c, 0, sizeof(*c));
        c->type = CA_DRAW_RECT;
        c->x = panel_x; c->y = panel_y;
        c->w = panel_w;  c->h = panel_h;
        c->corner_radius = 6.0f;
        c->r = 0.05f; c->g = 0.05f; c->b = 0.07f; c->a = 0.88f;
        c->in_use  = true;
        c->overlay = true;
    }

    /* Helper macros for overlay text lines */
    char buf[96];
    float y = panel_y + pad;
    Ca_Node tmp;
    uint32_t dbg_green  = ca_color(0.0f,  1.0f,  0.0f,  1.0f);
    uint32_t dbg_yellow = ca_color(1.0f,  0.85f, 0.0f,  1.0f);
    uint32_t dbg_white  = ca_color(0.85f, 0.85f, 0.85f, 1.0f);
    uint32_t dbg_cyan   = ca_color(0.3f,  0.9f,  1.0f,  1.0f);
    uint32_t dbg_dim    = ca_color(0.5f,  0.5f,  0.5f,  1.0f);

#define DBG_LINE_C(color, fmt, ...)                                    \
    do {                                                               \
        snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);                \
        memset(&tmp, 0, sizeof(tmp));                                  \
        tmp.in_use = true;                                             \
        tmp.window = win;                                              \
        tmp.x = panel_x + pad; tmp.y = y;                             \
        tmp.w = panel_w - pad * 2; tmp.h = line_h;                    \
        tmp.desc.text_align = 0; /* left */                            \
        tmp.desc.font_size  = 11.0f;                                  \
        {                                                              \
            uint32_t _gs = win->draw_cmd_count;                        \
            paint_text(win, font, &tmp, buf, color);                   \
            for (uint32_t _gi = _gs; _gi < win->draw_cmd_count; ++_gi)\
                win->draw_cmds[_gi].overlay = true;                    \
        }                                                              \
        y += line_h;                                                   \
    } while (0)

#define DBG_LINE(fmt, ...)   DBG_LINE_C(dbg_white, fmt, ##__VA_ARGS__)
#define DBG_HDR(fmt, ...)    DBG_LINE_C(dbg_yellow, fmt, ##__VA_ARGS__)
#define DBG_DIM(fmt, ...)    DBG_LINE_C(dbg_dim, fmt, ##__VA_ARGS__)

    /* ---- Header ---- */
    DBG_LINE_C(dbg_green, "Debug Overlay (F9)");

    /* ---- GPU / Device ---- */
    DBG_HDR("GPU");
    DBG_LINE("  %s", inst->gpu_name);
    DBG_DIM("  %s  |  Vulkan %u.%u.%u",
            gpu_type_str,
            VK_API_VERSION_MAJOR(inst->vk_api_version),
            VK_API_VERSION_MINOR(inst->vk_api_version),
            VK_API_VERSION_PATCH(inst->vk_api_version));
    DBG_LINE("  VRAM: %.0f MB  |  Heaps: %u",
             (double)inst->gpu_heap_total / (1024.0 * 1024.0),
             inst->gpu_heap_count);
    DBG_LINE("  Queue: gfx=%u  present=%u%s",
             inst->gfx_family, inst->present_family,
             inst->gfx_family == inst->present_family ? " (shared)" : "");

    /* ---- Presentation ---- */
    DBG_HDR("Presentation");
    int fb_w, fb_h;
    glfwGetFramebufferSize(win->glfw, &fb_w, &fb_h);
    int win_w, win_h;
    glfwGetWindowSize(win->glfw, &win_w, &win_h);
    float xscale, yscale;
    glfwGetWindowContentScale(win->glfw, &xscale, &yscale);
    DBG_LINE("  Window: %dx%d  FB: %dx%d", win_w, win_h, fb_w, fb_h);
    DBG_LINE("  DPI Scale: %.1fx  |  Images: %u  |  %s",
             xscale, win->sc.image_count, pm_str);

    /* ---- Performance ---- */
    DBG_HDR("Performance");
    DBG_LINE_C(dbg_cyan, "  %.1f FPS  |  %.2f ms/frame",
               win->dbg_fps, win->dbg_frame_time_ms);
    DBG_LINE("  Frames: %u", win->dbg_frames_rendered);

    /* ---- Memory ---- */
    DBG_HDR("Memory");
    DBG_LINE("  Process RSS: %.1f MB", rss_mb);

    /* ---- Renderer ---- */
    DBG_HDR("Renderer");
    DBG_LINE("  Draw cmds: %u  |  Batches: %u",
             win->dbg_draw_cmds, win->dbg_batches);
    DBG_LINE("  Rect inst: %u  |  Txt/Img inst: %u",
             win->dbg_rect_instances, win->dbg_ti_instances);

    /* ---- UI Tree ---- */
    DBG_HDR("UI Tree");
    DBG_LINE("  Nodes: %u / %u  |  Repainted: %u",
             node_count, (uint32_t)ca_pool_slot_count(&win->node_pool), repainted_count);
    DBG_LINE("  Layouts: %u  Dirty: %u  Transitions: %u",
             win->dbg_layout_count, win->dbg_dirty_count,
             win->dbg_transition_count);

#undef DBG_LINE
#undef DBG_LINE_C
#undef DBG_HDR
#undef DBG_DIM
}

/* ================================================================
   PUBLIC — paint pass entry point
   ================================================================ */

void ca_paint_pass(Ca_Instance *inst, Ca_Window *win)
{
    if (!win->root) return;

    /* 1. Reset the draw command list — it is rebuilt from cached + fresh data */
    win->draw_cmd_count = 0;

    /* 2. Incremental tree walk: only dirty nodes repaint, clean reuse cache */
    ClipRect no_clip = { .active = false };
    paint_tree_cached(inst, win, win->root, no_clip, 0,
                      ca_transform_identity());

    /* 3. Decorations — always fresh, never cached (depend on global focus state) */
    Ca_Font *font = inst->font;
    if (font) {
        /* Cursor for focused text input */
        if (win->focused_node &&
            ca_pool_slot_count(&win->input_pool) > 0) {
            for (uint32_t i = 0; i < ca_pool_slot_count(&win->input_pool); ++i) {
                Ca_TextInput *inp = CA_POOL_AT(win->input_pool, Ca_TextInput, i);
                if (inp->in_use && inp->node == win->focused_node) {
                    paint_cursor(win, font, inp->node, inp->text, inp->cursor);
                    break;
                }
            }
        }
    }

    /* 4. Overlays — always fresh (dropdowns, tooltips, context menus, modals) */
    paint_overlays(inst, win);

    /* 5. Debug overlay — when enabled, always fresh */
    if (win->debug_overlay)
        paint_debug_overlay(inst, win);
}
