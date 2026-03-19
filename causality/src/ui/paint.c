/* paint.c — CPU-side draw command generation */
#include "paint.h"
#include "font.h"
#include <GLFW/glfw3.h>

static void unpack_color(uint32_t packed, float *r, float *g, float *b, float *a)
{
    *r = (float)((packed >> 24) & 0xFF) / 255.0f;
    *g = (float)((packed >> 16) & 0xFF) / 255.0f;
    *b = (float)((packed >>  8) & 0xFF) / 255.0f;
    *a = (float)((packed)       & 0xFF) / 255.0f;
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

/* Walk up from a node and compute its effective clip rect from ancestors
   with overflow != visible. */
static ClipRect find_clip_for_node(Ca_Node *node)
{
    ClipRect clip = { .active = false };
    Ca_Node *cur = node->parent;
    while (cur) {
        if (cur->desc.overflow_x >= 1 || cur->desc.overflow_y >= 1) {
            clip = clip_intersect(clip, cur->x, cur->y, cur->w, cur->h);
        }
        cur = cur->parent;
    }
    return clip;
}

static void paint_node(Ca_Window *win, Ca_Node *node, ClipRect clip)
{
    if (!node->in_use) return;
    if (node->desc.hidden) return;

    if (node->dirty & CA_DIRTY_CONTENT) {
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            node->draw_cmd_idx = (int32_t)win->draw_cmd_count;
            Ca_DrawCmd *cmd    = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = node->x;
            cmd->y             = node->y;
            cmd->w             = node->w;
            cmd->h             = node->h;
            unpack_color(node->desc.background, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            /* Apply opacity (0 means not set = fully opaque) */
            float op = (node->desc.opacity > 0.0f) ? node->desc.opacity : 1.0f;
            cmd->a *= op;
            cmd->corner_radius = node->desc.corner_radius;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
        node->dirty &= ~CA_DIRTY_CONTENT;
    }

    /* Determine clip rect for children if overflow is hidden/scroll */
    ClipRect child_clip = clip;
    bool clips = (node->desc.overflow_x >= 1) || (node->desc.overflow_y >= 1);
    if (clips) {
        child_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);
    }

    for (uint32_t i = 0; i < node->child_count; ++i)
        paint_node(win, node->children[i], child_clip);

    /* ---- Scrollbar overlay for overflow:scroll / auto ---- */
    if (node->desc.overflow_y >= 2 && node->content_h > node->h) {
        float bar_w   = 6.0f;
        float margin  = 2.0f;
        float track_h = node->h - margin * 2;
        float ratio   = node->h / node->content_h;
        float thumb_h = track_h * ratio;
        if (thumb_h < 16.0f) thumb_h = 16.0f;
        if (thumb_h > track_h) thumb_h = track_h;

        float max_scroll = node->content_h - node->h;
        float scroll_pct = (max_scroll > 0.0f) ? node->scroll_y / max_scroll : 0.0f;
        float thumb_y    = node->y + margin + scroll_pct * (track_h - thumb_h);
        float bar_x      = node->x + node->w - bar_w - margin;

        /* Track (subtle background) */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = bar_x;
            cmd->y             = node->y + margin;
            cmd->w             = bar_w;
            cmd->h             = track_h;
            cmd->r = 1.0f; cmd->g = 1.0f; cmd->b = 1.0f; cmd->a = 0.05f;
            cmd->corner_radius = bar_w * 0.5f;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
        /* Thumb */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = bar_x;
            cmd->y             = thumb_y;
            cmd->w             = bar_w;
            cmd->h             = thumb_h;
            cmd->r = 1.0f; cmd->g = 1.0f; cmd->b = 1.0f; cmd->a = 0.35f;
            cmd->corner_radius = bar_w * 0.5f;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
    }
    if (node->desc.overflow_x >= 2 && node->content_w > node->w) {
        float bar_h   = 6.0f;
        float margin  = 2.0f;
        float track_w = node->w - margin * 2;
        float ratio   = node->w / node->content_w;
        float thumb_w = track_w * ratio;
        if (thumb_w < 16.0f) thumb_w = 16.0f;
        if (thumb_w > track_w) thumb_w = track_w;

        float max_scroll = node->content_w - node->w;
        float scroll_pct = (max_scroll > 0.0f) ? node->scroll_x / max_scroll : 0.0f;
        float thumb_x    = node->x + margin + scroll_pct * (track_w - thumb_w);
        float bar_y      = node->y + node->h - bar_h - margin;

        /* Track */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = node->x + margin;
            cmd->y             = bar_y;
            cmd->w             = track_w;
            cmd->h             = bar_h;
            cmd->r = 1.0f; cmd->g = 1.0f; cmd->b = 1.0f; cmd->a = 0.05f;
            cmd->corner_radius = bar_h * 0.5f;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
        /* Thumb */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = thumb_x;
            cmd->y             = bar_y;
            cmd->w             = thumb_w;
            cmd->h             = bar_h;
            cmd->r = 1.0f; cmd->g = 1.0f; cmd->b = 1.0f; cmd->a = 0.35f;
            cmd->corner_radius = bar_h * 0.5f;
            cmd->in_use        = true;
            set_clip(cmd, clip);
        }
    }
}

/* Emit glyph draw commands for a text string centred in the given node rect. */
static void paint_text(Ca_Window *win, Ca_Font *font,
                       Ca_Node *node,
                       const char *text, uint32_t packed_color)
{
    if (!text || text[0] == '\0') return;
    if (!node || !node->in_use)   return;
    if (node->desc.hidden)        return;

    float r, g, b, a;
    if (packed_color == 0) {
        /* Default to opaque white when no colour was specified */
        r = g = b = a = 1.0f;
    } else {
        r = (float)((packed_color >> 24) & 0xFF) / 255.0f;
        g = (float)((packed_color >> 16) & 0xFF) / 255.0f;
        b = (float)((packed_color >>  8) & 0xFF) / 255.0f;
        a = (float)((packed_color)       & 0xFF) / 255.0f;
    }

    /* Effective content scale: divide by ui_scale so glyphs grow with zoom */
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;

    /* Measure total advance width in logical pixels */
    float text_w = 0.0f;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)(*p);
        if (c >= CA_FONT_GLYPH_FIRST &&
            c <  CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            text_w += font->glyphs[c - CA_FONT_GLYPH_FIRST].xadvance / cs;
    }

    /* Compute baseline (vertically centred) and left edge (horizontally centred).
       If text is wider than the node, left-align instead of centering. */
    float baseline_logical =
        node->y + node->h * 0.5f + (font->ascent + font->descent) * 0.5f;
    float left_logical;
    if (text_w > node->w)
        left_logical = node->x + node->desc.padding_left;
    else
        left_logical = node->x + (node->w - text_w) * 0.5f;

    /* GetBakedQuad works in native (baked) pixel space */
    float xpos = left_logical * cs;
    float ypos = baseline_logical * cs;

    for (const char *p = text; *p; p++) {
        int c = (unsigned char)(*p);
        if (c < CA_FONT_GLYPH_FIRST ||
            c >= CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            continue;
        if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) break;

        stbtt_aligned_quad q;
        /* opengl_fillrule=1: y increases downward (screen space = Vulkan) */
        stbtt_GetBakedQuad(font->glyphs,
                           font->atlas_w, font->atlas_h,
                           c - CA_FONT_GLYPH_FIRST,
                           &xpos, &ypos, &q, 1);

        float gw = (q.x1 - q.x0) / cs;
        float gh = (q.y1 - q.y0) / cs;
        if (gw < 0.5f || gh < 0.5f) continue; /* skip whitespace */

        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_GLYPH;
        cmd->x = q.x0 / cs;  cmd->y = q.y0 / cs;
        cmd->w = gw;          cmd->h = gh;
        cmd->r = r;  cmd->g = g;  cmd->b = b;  cmd->a = a;
        cmd->u0 = q.s0;  cmd->v0 = q.t0;
        cmd->u1 = q.s1;  cmd->v1 = q.t1;
        cmd->in_use = true;

        /* Clip to node bounds + ancestor overflow containers */
        ClipRect clip = find_clip_for_node(node);
        ClipRect node_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);
        set_clip(cmd, node_clip);
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

    float baseline_logical =
        node->y + node->h * 0.5f + (font->ascent + font->descent) * 0.5f;
    float left_logical = node->x + node->desc.padding_left;

    float xpos = left_logical * cs;
    float ypos = baseline_logical * cs;

    for (const char *p = text; *p; p++) {
        int c = (unsigned char)(*p);
        if (c < CA_FONT_GLYPH_FIRST ||
            c >= CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            continue;
        if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) break;

        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(font->glyphs,
                           font->atlas_w, font->atlas_h,
                           c - CA_FONT_GLYPH_FIRST,
                           &xpos, &ypos, &q, 1);

        float gw = (q.x1 - q.x0) / cs;
        float gh = (q.y1 - q.y0) / cs;
        if (gw < 0.5f || gh < 0.5f) continue;

        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_GLYPH;
        cmd->x = q.x0 / cs;  cmd->y = q.y0 / cs;
        cmd->w = gw;          cmd->h = gh;
        cmd->r = r;  cmd->g = g;  cmd->b = b;  cmd->a = a;
        cmd->u0 = q.s0;  cmd->v0 = q.t0;
        cmd->u1 = q.s1;  cmd->v1 = q.t1;
        cmd->in_use = true;

        ClipRect clip = find_clip_for_node(node);
        /* Also clip to the input's own bounds */
        ClipRect input_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);
        set_clip(cmd, input_clip);
    }
}

/* Measure x-advance for a substring of text */
static float measure_text_advance(Ca_Font *font, const char *text, int byte_count,
                                  float content_scale, float ui_scale)
{
    float cs = content_scale / (ui_scale > 0.0f ? ui_scale : 1.0f);
    float w = 0.0f;
    int i = 0;
    for (const char *p = text; *p && i < byte_count; p++, i++) {
        int c = (unsigned char)(*p);
        if (c >= CA_FONT_GLYPH_FIRST &&
            c <  CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            w += font->glyphs[c - CA_FONT_GLYPH_FIRST].xadvance / cs;
    }
    return w;
}

/* Paint a thin cursor line at the given cursor byte offset in the input text */
static void paint_cursor(Ca_Window *win, Ca_Font *font,
                         Ca_Node *node, const char *text, int cursor_pos)
{
    if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) return;

    float advance = measure_text_advance(font, text, cursor_pos,
                                         font->content_scale, win->ui_scale);
    float cursor_x = node->x + node->desc.padding_left + advance;
    float cursor_h = node->h * 0.7f;
    float cursor_y = node->y + (node->h - cursor_h) * 0.5f;

    Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type   = CA_DRAW_RECT;
    cmd->x      = cursor_x;
    cmd->y      = cursor_y;
    cmd->w      = 1.5f;
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
        if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) break;
        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_RECT;
        cmd->x      = sides[i].x;
        cmd->y      = sides[i].y;
        cmd->w      = sides[i].w;
        cmd->h      = sides[i].h;
        cmd->r = 0.4f; cmd->g = 0.6f; cmd->b = 1.0f; cmd->a = 0.85f;
        cmd->in_use = true;
    }
}

void ca_paint_pass(Ca_Instance *inst, Ca_Window *win)
{
    if (!win->root) return;

    /* 1. Rectangle pass — one solid-colour quad per node */
    ClipRect no_clip = { .active = false };
    paint_node(win, win->root, no_clip);

    /* 2. Text pass — glyph quads for labels and buttons */
    Ca_Font *font = inst->font;
    if (!font) return;

    /* Labels */
    if (win->label_pool) {
        for (uint32_t i = 0; i < CA_MAX_LABELS_PER_WINDOW; ++i) {
            Ca_Label *lbl = &win->label_pool[i];
            if (!lbl->in_use || lbl->text[0] == '\0') continue;
            paint_text(win, font, lbl->node, lbl->text, lbl->color);
        }
    }

    /* Buttons */
    if (win->button_pool) {
        for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
            Ca_Button *btn = &win->button_pool[i];
            if (!btn->in_use || btn->text[0] == '\0') continue;
            paint_text(win, font, btn->node, btn->text, btn->text_color);
        }
    }

    /* Text inputs */
    if (win->input_pool) {
        for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i) {
            Ca_TextInput *inp = &win->input_pool[i];
            if (!inp->in_use || !inp->node) continue;
            bool focused = (win->focused_node == inp->node);
            if (inp->text[0] != '\0') {
                paint_text_left(win, font, inp->node, inp->text, inp->text_color);
            } else if (inp->placeholder[0] != '\0') {
                paint_text_left(win, font, inp->node, inp->placeholder,
                                inp->placeholder_color);
            }
            if (focused)
                paint_cursor(win, font, inp->node, inp->text, inp->cursor);
        }
    }

    /* Focus ring on the currently focused element */
    if (win->focused_node)
        paint_focus_ring(win, win->focused_node);

    /* ---- New widgets ---- */

    /* Checkboxes: box + checkmark + text */
    if (win->checkbox_pool) {
        for (uint32_t i = 0; i < CA_MAX_CHECKBOXES_PER_WINDOW; ++i) {
            Ca_Checkbox *cb = &win->checkbox_pool[i];
            if (!cb->in_use || !cb->node || cb->node->desc.hidden) continue;
            Ca_Node *n = cb->node;
            float bs = n->h * 0.8f; /* box size */
            float bx = n->x + 1.0f;
            float by = n->y + (n->h - bs) * 0.5f;

            /* Box background */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = bx; c->y = by; c->w = bs; c->h = bs;
                c->corner_radius = 3.0f;
                if (cb->checked) { c->r = 0.25f; c->g = 0.55f; c->b = 1.0f; c->a = 1.0f; }
                else             { c->r = 0.3f;  c->g = 0.3f;  c->b = 0.35f; c->a = 1.0f; }
                c->in_use = true;
            }
            /* Checkmark (two small rects forming an L-shape) */
            if (cb->checked && win->draw_cmd_count + 1 < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
            if (cb->text[0] && font) {
                /* Create a temporary pseudo-node for text positioning */
                Ca_Node tn = *n;
                tn.x = bx + bs + 6.0f;
                tn.w = n->w - bs - 6.0f;
                paint_text(win, font, &tn, cb->text, cb->text_color);
            }
        }
    }

    /* Radio buttons: circle + dot + text */
    if (win->radio_pool) {
        for (uint32_t i = 0; i < CA_MAX_RADIOS_PER_WINDOW; ++i) {
            Ca_Radio *r = &win->radio_pool[i];
            if (!r->in_use || !r->node || r->node->desc.hidden) continue;
            Ca_Node *n = r->node;
            float bs = n->h * 0.8f;
            float bx = n->x + 1.0f;
            float by = n->y + (n->h - bs) * 0.5f;

            /* Outer circle */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = bx; c->y = by; c->w = bs; c->h = bs;
                c->corner_radius = bs * 0.5f;
                c->r = 0.3f; c->g = 0.3f; c->b = 0.35f; c->a = 1.0f;
                c->in_use = true;
            }
            /* Inner dot when selected */
            if (r->value && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                float ds = bs * 0.5f;
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = bx + (bs - ds) * 0.5f;
                c->y = by + (bs - ds) * 0.5f;
                c->w = ds; c->h = ds;
                c->corner_radius = ds * 0.5f;
                c->r = 0.25f; c->g = 0.55f; c->b = 1.0f; c->a = 1.0f;
                c->in_use = true;
            }
            /* Label text */
            if (r->text[0] && font) {
                Ca_Node tn = *n;
                tn.x = bx + bs + 6.0f;
                tn.w = n->w - bs - 6.0f;
                paint_text(win, font, &tn, r->text, r->text_color);
            }
        }
    }

    /* Sliders: track + fill + thumb */
    if (win->slider_pool) {
        for (uint32_t i = 0; i < CA_MAX_SLIDERS_PER_WINDOW; ++i) {
            Ca_Slider *sl = &win->slider_pool[i];
            if (!sl->in_use || !sl->node || sl->node->desc.hidden) continue;
            Ca_Node *n = sl->node;
            float track_h = 4.0f;
            float track_y = n->y + (n->h - track_h) * 0.5f;
            float pct = (sl->max_val > sl->min_val)
                ? (sl->value - sl->min_val) / (sl->max_val - sl->min_val) : 0;

            /* Track background */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x; c->y = track_y; c->w = n->w; c->h = track_h;
                c->corner_radius = 2.0f;
                c->r = 0.25f; c->g = 0.25f; c->b = 0.3f; c->a = 1.0f;
                c->in_use = true;
            }
            /* Fill */
            float fill_w = n->w * pct;
            if (fill_w > 0 && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x; c->y = track_y; c->w = fill_w; c->h = track_h;
                c->corner_radius = 2.0f;
                c->r = 0.25f; c->g = 0.55f; c->b = 1.0f; c->a = 1.0f;
                c->in_use = true;
            }
            /* Thumb */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                float thumb_sz = 14.0f;
                float tx = n->x + fill_w - thumb_sz * 0.5f;
                float ty = n->y + (n->h - thumb_sz) * 0.5f;
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = tx; c->y = ty; c->w = thumb_sz; c->h = thumb_sz;
                c->corner_radius = thumb_sz * 0.5f;
                c->r = 1.0f; c->g = 1.0f; c->b = 1.0f; c->a = 1.0f;
                c->in_use = true;
            }
        }
    }

    /* Toggle switches: pill track + circle thumb */
    if (win->toggle_pool) {
        for (uint32_t i = 0; i < CA_MAX_TOGGLES_PER_WINDOW; ++i) {
            Ca_Toggle *t = &win->toggle_pool[i];
            if (!t->in_use || !t->node || t->node->desc.hidden) continue;
            Ca_Node *n = t->node;

            /* Track */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x; c->y = n->y; c->w = n->w; c->h = n->h;
                c->corner_radius = n->h * 0.5f;
                if (t->on) { c->r = 0.2f; c->g = 0.6f; c->b = 0.3f; c->a = 1.0f; }
                else       { c->r = 0.3f; c->g = 0.3f; c->b = 0.35f; c->a = 1.0f; }
                c->in_use = true;
            }
            /* Thumb */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                float inset = 2.0f;
                float thumb_d = n->h - inset * 2;
                float tx = t->on ? (n->x + n->w - thumb_d - inset) : (n->x + inset);
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = tx; c->y = n->y + inset;
                c->w = thumb_d; c->h = thumb_d;
                c->corner_radius = thumb_d * 0.5f;
                c->r = 1.0f; c->g = 1.0f; c->b = 1.0f; c->a = 1.0f;
                c->in_use = true;
            }
        }
    }

    /* Progress bars: track + fill */
    if (win->progress_pool) {
        for (uint32_t i = 0; i < CA_MAX_PROGRESS_PER_WINDOW; ++i) {
            Ca_Progress *p = &win->progress_pool[i];
            if (!p->in_use || !p->node || p->node->desc.hidden) continue;
            Ca_Node *n = p->node;
            float rad = n->h * 0.5f;

            /* Track */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x; c->y = n->y; c->w = n->w; c->h = n->h;
                c->corner_radius = rad;
                c->r = 0.2f; c->g = 0.2f; c->b = 0.25f; c->a = 1.0f;
                c->in_use = true;
            }
            /* Fill */
            float fw = n->w * p->value;
            if (fw > 0 && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                float fr, fg, fb, fa;
                unpack_color(p->bar_color, &fr, &fg, &fb, &fa);
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x; c->y = n->y; c->w = fw; c->h = n->h;
                c->corner_radius = rad;
                c->r = fr; c->g = fg; c->b = fb; c->a = fa;
                c->in_use = true;
            }
        }
    }

    /* Select / Dropdown: button display + dropdown overlay */
    if (win->select_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_SELECTS_PER_WINDOW; ++i) {
            Ca_Select *sel = &win->select_pool[i];
            if (!sel->in_use || !sel->node || sel->node->desc.hidden) continue;
            Ca_Node *n = sel->node;

            /* Draw current selection text */
            if (sel->selected >= 0 && sel->selected < sel->option_count) {
                paint_text(win, font, n, sel->options[sel->selected], 0);
            }

            /* Down arrow indicator */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                float asz = 6.0f;
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = n->x + n->w - asz - 6.0f;
                c->y = n->y + (n->h - asz * 0.5f) * 0.5f;
                c->w = asz; c->h = asz * 0.5f;
                c->r = 0.7f; c->g = 0.7f; c->b = 0.7f; c->a = 1.0f;
                c->in_use = true;
            }

            /* Open dropdown overlay (rendered after everything) */
            if (sel->open) {
                float opt_h = n->h;
                float drop_y = n->y + n->h;
                /* Dropdown background */
                if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                    Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                    memset(c, 0, sizeof(*c));
                    c->type = CA_DRAW_RECT;
                    c->x = n->x; c->y = drop_y;
                    c->w = n->w; c->h = opt_h * (float)sel->option_count;
                    c->corner_radius = 4.0f;
                    c->r = 0.15f; c->g = 0.15f; c->b = 0.2f; c->a = 0.95f;
                    c->in_use = true;
                    c->overlay = true;
                }
                /* Options */
                for (int oi = 0; oi < sel->option_count; ++oi) {
                    float oy = drop_y + opt_h * (float)oi;
                    /* Highlight selected */
                    if (oi == sel->selected && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                        Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                        memset(c, 0, sizeof(*c));
                        c->type = CA_DRAW_RECT;
                        c->x = n->x; c->y = oy; c->w = n->w; c->h = opt_h;
                        c->r = 0.25f; c->g = 0.25f; c->b = 0.35f; c->a = 1.0f;
                        c->in_use = true;
                        c->overlay = true;
                    }
                    /* Option text (use a temporary node) — mark glyphs as overlay */
                    uint32_t glyph_start = win->draw_cmd_count;
                    Ca_Node tmp = *n;
                    tmp.x = n->x; tmp.y = oy; tmp.h = opt_h;
                    paint_text(win, font, &tmp, sel->options[oi], 0);
                    for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                        win->draw_cmds[gi].overlay = true;
                }
            }
        }
    }

    /* Tab bar headers: text in each tab */
    if (win->tabbar_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_TABBARS_PER_WINDOW; ++i) {
            Ca_TabBar *tb = &win->tabbar_pool[i];
            if (!tb->in_use || !tb->node) continue;
            for (int ti = 0; ti < tb->count; ++ti) {
                if (!tb->tab_nodes[ti]) continue;
                uint32_t tc = (ti == tb->active) ? ca_color(1,1,1,1) : ca_color(0.6f,0.6f,0.6f,1);
                paint_text(win, font, tb->tab_nodes[ti], tb->labels[ti], tc);
            }
        }
    }

    /* Tree node headers: expand indicator + text */
    if (win->treenode_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_TREENODES_PER_WINDOW; ++i) {
            Ca_TreeNode *tn = &win->treenode_pool[i];
            if (!tn->in_use || !tn->node || tn->node->desc.hidden) continue;
            if (tn->node->child_count == 0) continue;
            Ca_Node *hdr = tn->node->children[0];
            if (!hdr || hdr->desc.hidden) continue;

            /* Expand indicator */
            const char *indicator = tn->expanded ? "-" : "+";
            Ca_Node ind_n = *hdr;
            ind_n.w = 16.0f;
            paint_text(win, font, &ind_n, indicator, ca_color(0.6f,0.6f,0.6f,1));

            /* Label text */
            Ca_Node txt_n = *hdr;
            txt_n.x = hdr->x + 16.0f;
            txt_n.w = hdr->w - 16.0f;
            paint_text(win, font, &txt_n, tn->text, tn->text_color);
        }
    }

    /* Tooltips: rendered as overlay when hovering the target node */
    if (win->tooltip_pool && font && win->hovered_node) {
        for (uint32_t i = 0; i < CA_MAX_TOOLTIPS_PER_WINDOW; ++i) {
            Ca_Tooltip *tt = &win->tooltip_pool[i];
            if (!tt->in_use || !tt->node || tt->text[0] == '\0') continue;
            Ca_Node *hover = win->hovered_node;
            bool match = false;
            while (hover) {
                if (hover == tt->node) { match = true; break; }
                hover = hover->parent;
            }
            if (!match) continue;

            float tw = 0;
            for (const char *p = tt->text; *p; p++) {
                int ch = (unsigned char)*p;
                if (ch >= CA_FONT_GLYPH_FIRST && ch < CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT) {
                    float cs = font->content_scale / (win->ui_scale > 0 ? win->ui_scale : 1.0f);
                    tw += font->glyphs[ch - CA_FONT_GLYPH_FIRST].xadvance / cs;
                }
            }

            float pad = 6.0f;
            float tip_w = tw + pad * 2;
            float tip_h = 22.0f;
            float tip_x = tt->node->x;
            float tip_y = tt->node->y - tip_h - 4.0f;
            if (tip_y < 0) tip_y = tt->node->y + tt->node->h + 4.0f;

            /* Background */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = tip_x; c->y = tip_y; c->w = tip_w; c->h = tip_h;
                c->corner_radius = 4.0f;
                c->r = 0.1f; c->g = 0.1f; c->b = 0.15f; c->a = 0.95f;
                c->in_use = true;
                c->overlay = true;
            }
            /* Text */
            uint32_t glyph_start = win->draw_cmd_count;
            Ca_Node tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.in_use = true;
            tmp.x = tip_x; tmp.y = tip_y; tmp.w = tip_w; tmp.h = tip_h;
            tmp.window = win;
            paint_text(win, font, &tmp, tt->text, ca_color(0.9f,0.9f,0.9f,1));
            for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                win->draw_cmds[gi].overlay = true;
        }
    }

    /* Context menus: overlay at right-click position */
    if (win->ctxmenu_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
            Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
            if (!cm->in_use || !cm->open || cm->item_count <= 0) continue;

            float item_h = 24.0f;
            float menu_w = 120.0f;
            float menu_h = item_h * (float)cm->item_count;

            /* Background */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = cm->open_x; c->y = cm->open_y;
                c->w = menu_w; c->h = menu_h;
                c->corner_radius = 4.0f;
                c->r = 0.12f; c->g = 0.12f; c->b = 0.17f; c->a = 0.95f;
                c->in_use = true;
                c->overlay = true;
            }
            /* Items */
            for (int mi = 0; mi < cm->item_count; ++mi) {
                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.in_use = true;
                tmp.x = cm->open_x + 8.0f;
                tmp.y = cm->open_y + item_h * (float)mi;
                tmp.w = menu_w - 16.0f;
                tmp.h = item_h;
                tmp.window = win;
                paint_text(win, font, &tmp, cm->items[mi], ca_color(0.85f,0.85f,0.85f,1));
                for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                    win->draw_cmds[gi].overlay = true;
            }
        }
    }

    /* Modals: overlay background */
    if (win->modal_pool) {
        for (uint32_t i = 0; i < CA_MAX_MODALS_PER_WINDOW; ++i) {
            Ca_Modal *m = &win->modal_pool[i];
            if (!m->in_use || !m->visible) continue;
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
