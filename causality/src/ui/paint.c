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

/* Forward declarations — used by paint_node_content before definition */
static void paint_text(Ca_Window *win, Ca_Font *font,
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
            clip = clip_intersect(clip, cur->x, cur->y, cur->w, cur->h);
        }
        cur = cur->parent;
    }
    return clip;
}

/* Paint a single node's OWN visual content (background rect + widget-specific).
   Does NOT recurse into children.  Does NOT paint scrollbars (post-children). */
static void paint_node_content(Ca_Window *win, Ca_Font *font, Ca_Node *node, ClipRect clip)
{
    if (!node->in_use) return;
    if (node->desc.hidden) return;

    /* ---- Background rect ---- */
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
        float op = (node->desc.opacity > 0.0f) ? node->desc.opacity : 1.0f;
        cmd->a *= op;
        cmd->corner_radius = node->desc.corner_radius;
        cmd->in_use        = true;
        set_clip(cmd, clip);
    }

    /* ---- Widget-specific content ---- */
    if (!font) return;

    switch (node->widget_type) {
    case CA_WIDGET_LABEL: {
        Ca_Label *lbl = (Ca_Label *)node->widget;
        if (lbl && lbl->in_use && lbl->text[0])
            paint_text(win, font, node, lbl->text, lbl->color);
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
        float bx = node->x + 1.0f;
        float by = node->y + (node->h - bs) * 0.5f;
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
        /* Checkmark */
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
        if (cb->text[0]) {
            Ca_Node tn = *node;
            tn.x = bx + bs + 6.0f;
            tn.w = node->w - bs - 6.0f;
            paint_text(win, font, &tn, cb->text, cb->text_color);
        }
        break;
    }
    case CA_WIDGET_RADIO: {
        Ca_Radio *r = (Ca_Radio *)node->widget;
        if (!r || !r->in_use) break;
        float bs = node->h * 0.8f;
        float bx = node->x + 1.0f;
        float by = node->y + (node->h - bs) * 0.5f;
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
        if (r->text[0]) {
            Ca_Node tn = *node;
            tn.x = bx + bs + 6.0f;
            tn.w = node->w - bs - 6.0f;
            paint_text(win, font, &tn, r->text, r->text_color);
        }
        break;
    }
    case CA_WIDGET_SLIDER: {
        Ca_Slider *sl = (Ca_Slider *)node->widget;
        if (!sl || !sl->in_use) break;
        float track_h = 4.0f;
        float track_y = node->y + (node->h - track_h) * 0.5f;
        float pct = (sl->max_val > sl->min_val)
            ? (sl->value - sl->min_val) / (sl->max_val - sl->min_val) : 0;
        /* Track background */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = track_y; c->w = node->w; c->h = track_h;
            c->corner_radius = 2.0f;
            c->r = 0.25f; c->g = 0.25f; c->b = 0.3f; c->a = 1.0f;
            c->in_use = true;
        }
        /* Fill */
        float fill_w = node->w * pct;
        if (fill_w > 0 && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = track_y; c->w = fill_w; c->h = track_h;
            c->corner_radius = 2.0f;
            c->r = 0.25f; c->g = 0.55f; c->b = 1.0f; c->a = 1.0f;
            c->in_use = true;
        }
        /* Thumb */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            float thumb_sz = 14.0f;
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
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = node->y; c->w = node->w; c->h = node->h;
            c->corner_radius = node->h * 0.5f;
            if (t->on) { c->r = 0.2f; c->g = 0.6f; c->b = 0.3f; c->a = 1.0f; }
            else       { c->r = 0.3f; c->g = 0.3f; c->b = 0.35f; c->a = 1.0f; }
            c->in_use = true;
        }
        /* Thumb */
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            float inset = 2.0f;
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
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
            memset(c, 0, sizeof(*c));
            c->type = CA_DRAW_RECT;
            c->x = node->x; c->y = node->y; c->w = node->w; c->h = node->h;
            c->corner_radius = rad;
            c->r = 0.2f; c->g = 0.2f; c->b = 0.25f; c->a = 1.0f;
            c->in_use = true;
        }
        /* Fill */
        float fw = node->w * p->value;
        if (fw > 0 && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
                uint32_t tc = (ti == tb->active)
                    ? ca_color(1,1,1,1) : ca_color(0.6f,0.6f,0.6f,1);
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
        const char *indicator = tn->expanded ? "-" : "+";
        Ca_Node ind_n = *hdr;
        ind_n.w = 16.0f;
        paint_text(win, font, &ind_n, indicator, ca_color(0.6f,0.6f,0.6f,1));
        Ca_Node txt_n = *hdr;
        txt_n.x = hdr->x + 16.0f;
        txt_n.w = hdr->w - 16.0f;
        paint_text(win, font, &txt_n, tn->text, tn->text_color);
        break;
    }
    default: break;
    }
}

/* Paint scrollbar overlays for a node (post-children, so they draw on top). */
static void paint_scrollbars(Ca_Window *win, Ca_Node *node, ClipRect clip)
{
    /* ---- Y scrollbar ---- */
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

        /* Track */
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
    /* ---- X scrollbar ---- */
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

    /* Per-node font size scaling */
    float baked_logical = font->baked_px / font->content_scale;
    float desired_size  = node->desc.font_size > 0.0f ? node->desc.font_size : baked_logical;
    float font_scale    = desired_size / baked_logical;
    float cs_eff        = cs / font_scale;

    /* Measure total advance width in logical pixels */
    float text_w = 0.0f;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)(*p);
        if (c >= CA_FONT_GLYPH_FIRST &&
            c <  CA_FONT_GLYPH_FIRST + CA_FONT_GLYPH_COUNT)
            text_w += font->glyphs[c - CA_FONT_GLYPH_FIRST].xadvance / cs_eff;
    }

    /* Compute baseline (vertically centred) and left edge (horizontally centred).
       If text is wider than the node, left-align instead of centering. */
    float baseline_logical =
        node->y + node->h * 0.5f
        + (font->ascent * font_scale + font->descent * font_scale) * 0.5f;
    float left_logical;
    if (text_w > node->w)
        left_logical = node->x + node->desc.padding_left;
    else
        left_logical = node->x + (node->w - text_w) * 0.5f;

    /* GetBakedQuad works in native (baked) pixel space */
    float xpos = left_logical * cs_eff;
    float ypos = baseline_logical * cs_eff;

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

        float gw = (q.x1 - q.x0) / cs_eff;
        float gh = (q.y1 - q.y0) / cs_eff;
        if (gw < 0.5f || gh < 0.5f) continue; /* skip whitespace */

        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_GLYPH;
        cmd->x = q.x0 / cs_eff;  cmd->y = q.y0 / cs_eff;
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

    /* Per-node font size scaling */
    float baked_logical = font->baked_px / font->content_scale;
    float desired_size  = node->desc.font_size > 0.0f ? node->desc.font_size : baked_logical;
    float font_scale    = desired_size / baked_logical;
    float cs_eff        = cs / font_scale;

    float baseline_logical =
        node->y + node->h * 0.5f
        + (font->ascent * font_scale + font->descent * font_scale) * 0.5f;
    float left_logical = node->x + node->desc.padding_left;

    float xpos = left_logical * cs_eff;
    float ypos = baseline_logical * cs_eff;

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

        float gw = (q.x1 - q.x0) / cs_eff;
        float gh = (q.y1 - q.y0) / cs_eff;
        if (gw < 0.5f || gh < 0.5f) continue;

        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type   = CA_DRAW_GLYPH;
        cmd->x = q.x0 / cs_eff;  cmd->y = q.y0 / cs_eff;
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
        /* Allocate fresh space at end of cache pool */
        if (win->paint_cache_used + count > CA_MAX_DRAW_CMDS_PER_WINDOW) {
            /* Cache full — commands are already in draw_cmds, just skip caching */
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

/* DFS tree walk with per-node paint caching.
   - Dirty nodes: paint fresh → cache → commands already in draw_cmds
   - Clean nodes: copy from cache → draw_cmds */
static void paint_tree_cached(Ca_Instance *inst, Ca_Window *win,
                              Ca_Node *node, ClipRect clip)
{
    if (!node || !node->in_use || node->desc.hidden) return;

    bool was_dirty = (node->dirty & CA_DIRTY_CONTENT) != 0;

    /* ---- Pre-children: background + widget visuals ---- */
    if (was_dirty) {
        uint32_t start = win->draw_cmd_count;
        paint_node_content(win, inst->font, node, clip);
        uint32_t count = win->draw_cmd_count - start;
        cache_commands(win, node, start, count, false);
        node->dirty &= ~CA_DIRTY_CONTENT;
    } else if (node->cache_count > 0 &&
               win->draw_cmd_count + node->cache_count <= CA_MAX_DRAW_CMDS_PER_WINDOW)
    {
        memcpy(&win->draw_cmds[win->draw_cmd_count],
               &win->paint_cache[node->cache_start],
               node->cache_count * sizeof(Ca_DrawCmd));
        node->draw_cmd_idx = (int32_t)win->draw_cmd_count;
        win->draw_cmd_count += node->cache_count;
    }

    /* ---- Child clip ---- */
    ClipRect child_clip = clip;
    if (node->desc.overflow_x >= 1 || node->desc.overflow_y >= 1)
        child_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);

    /* ---- Recurse children ---- */
    for (uint32_t i = 0; i < node->child_count; ++i)
        paint_tree_cached(inst, win, node->children[i], child_clip);

    /* ---- Post-children: scrollbars ---- */
    if (was_dirty) {
        uint32_t sb_start = win->draw_cmd_count;
        paint_scrollbars(win, node, clip);
        uint32_t sb_count = win->draw_cmd_count - sb_start;
        cache_commands(win, node, sb_start, sb_count, true);
    } else if (node->cache_post_count > 0 &&
               win->draw_cmd_count + node->cache_post_count <= CA_MAX_DRAW_CMDS_PER_WINDOW)
    {
        memcpy(&win->draw_cmds[win->draw_cmd_count],
               &win->paint_cache[node->cache_post_start],
               node->cache_post_count * sizeof(Ca_DrawCmd));
        win->draw_cmd_count += node->cache_post_count;
    }
}

/* ================================================================
   OVERLAY PASS — always regenerated (not cached)
   ================================================================ */

static void paint_overlays(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Font *font = inst->font;

    /* ---- Select dropdown overlays ---- */
    if (win->select_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_SELECTS_PER_WINDOW; ++i) {
            Ca_Select *sel = &win->select_pool[i];
            if (!sel->in_use || !sel->node || sel->node->desc.hidden) continue;
            if (!sel->open) continue;
            Ca_Node *n = sel->node;

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
                if (oi == sel->selected && win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                    Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                    memset(c, 0, sizeof(*c));
                    c->type = CA_DRAW_RECT;
                    c->x = n->x; c->y = oy; c->w = n->w; c->h = opt_h;
                    c->r = 0.25f; c->g = 0.25f; c->b = 0.35f; c->a = 1.0f;
                    c->in_use = true;
                    c->overlay = true;
                }
                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp = *n;
                tmp.x = n->x; tmp.y = oy; tmp.h = opt_h;
                paint_text(win, font, &tmp, sel->options[oi], 0);
                for (uint32_t gi = glyph_start; gi < win->draw_cmd_count; ++gi)
                    win->draw_cmds[gi].overlay = true;
            }
        }
    }

    /* ---- Tooltips ---- */
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

    /* ---- Context menus ---- */
    if (win->ctxmenu_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
            Ca_CtxMenu *cm = &win->ctxmenu_pool[i];
            if (!cm->in_use || !cm->open || cm->item_count <= 0) continue;

            float item_h = 24.0f;
            float menu_w = 120.0f;
            float menu_h = item_h * (float)cm->item_count;

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

    /* ---- Modals ---- */
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
    paint_tree_cached(inst, win, win->root, no_clip);

    /* 3. Decorations — always fresh, never cached (depend on global focus state) */
    Ca_Font *font = inst->font;
    if (font) {
        /* Focus ring on the currently focused element */
        if (win->focused_node)
            paint_focus_ring(win, win->focused_node);

        /* Cursor for focused text input */
        if (win->focused_node && win->input_pool) {
            for (uint32_t i = 0; i < CA_MAX_INPUTS_PER_WINDOW; ++i) {
                Ca_TextInput *inp = &win->input_pool[i];
                if (inp->in_use && inp->node == win->focused_node) {
                    paint_cursor(win, font, inp->node, inp->text, inp->cursor);
                    break;
                }
            }
        }
    }

    /* 4. Overlays — always fresh (dropdowns, tooltips, context menus, modals) */
    paint_overlays(inst, win);
}
