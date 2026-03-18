/* paint.c — CPU-side draw command generation */
#include "paint.h"
#include "font.h"

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
        unsigned char c = (unsigned char)(*p);
        if (c >= CA_FONT_GLYPH_FIRST &&
            c <  CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            text_w += font->glyphs[c - CA_FONT_GLYPH_FIRST].xadvance / cs;
    }

    /* Compute baseline (vertically centred) and left edge (horizontally centred) */
    float baseline_logical =
        node->y + node->h * 0.5f + (font->ascent + font->descent) * 0.5f;
    float left_logical = node->x + (node->w - text_w) * 0.5f;

    /* GetBakedQuad works in native (baked) pixel space */
    float xpos = left_logical * cs;
    float ypos = baseline_logical * cs;

    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)(*p);
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

        /* Inherit clip rect from node's ancestor overflow containers */
        ClipRect clip = find_clip_for_node(node);
        set_clip(cmd, clip);
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
}
