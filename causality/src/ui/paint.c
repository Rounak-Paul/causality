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

static void paint_node(Ca_Window *win, Ca_Node *node)
{
    if (!node->in_use) return;

    if (node->dirty & CA_DIRTY_CONTENT) {
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            node->draw_cmd_idx = (int32_t)win->draw_cmd_count;
            Ca_DrawCmd *cmd    = &win->draw_cmds[win->draw_cmd_count++];
            cmd->type          = CA_DRAW_RECT;
            cmd->x             = node->x;
            cmd->y             = node->y;
            cmd->w             = node->w;
            cmd->h             = node->h;
            unpack_color(node->desc.background, &cmd->r, &cmd->g, &cmd->b, &cmd->a);
            cmd->corner_radius = node->desc.corner_radius;
            cmd->in_use        = true;
        }
        node->dirty &= ~CA_DIRTY_CONTENT;
    }

    for (uint32_t i = 0; i < node->child_count; ++i)
        paint_node(win, node->children[i]);
}

/* Emit glyph draw commands for a text string centred in the given node rect. */
static void paint_text(Ca_Window *win, Ca_Font *font,
                       Ca_Node *node,
                       const char *text, uint32_t packed_color)
{
    if (!text || text[0] == '\0') return;
    if (!node || !node->in_use)   return;

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

    float cs = font->content_scale;

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
        cmd->type   = CA_DRAW_GLYPH;
        cmd->x = q.x0 / cs;  cmd->y = q.y0 / cs;
        cmd->w = gw;          cmd->h = gh;
        cmd->r = r;  cmd->g = g;  cmd->b = b;  cmd->a = a;
        cmd->u0 = q.s0;  cmd->v0 = q.t0;
        cmd->u1 = q.s1;  cmd->v1 = q.t1;
        cmd->in_use = true;
    }
}

void ca_paint_pass(Ca_Instance *inst, Ca_Window *win)
{
    if (!win->root) return;

    /* 1. Rectangle pass — one solid-colour quad per node */
    paint_node(win, win->root);

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
