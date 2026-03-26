/* paint.c — CPU-side draw command generation */
#include "paint.h"
#include "font.h"
#include <GLFW/glfw3.h>

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
            clip = clip_intersect(clip, cur->x, cur->y, cur->w, cur->h);
        }
        cur = cur->parent;
    }
    return clip;
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

    /* Record starting draw cmd index so we can apply disabled dim after. */
    uint32_t cmd_start = win->draw_cmd_count;

    /* ---- Box shadow (drawn before background, behind everything) ---- */
    if (node->desc.shadow_color != 0 &&
        win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
        float sr, sg, sb, sa;
        unpack_color(node->desc.shadow_color, &sr, &sg, &sb, &sa);
        float blur = node->desc.shadow_blur;
        float expand = blur * 0.5f; /* expand rect to simulate blur spread */
        Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
        memset(cmd, 0, sizeof(*cmd));
        cmd->type          = CA_DRAW_RECT;
        cmd->x             = node->x + node->desc.shadow_offset_x - expand;
        cmd->y             = node->y + node->desc.shadow_offset_y - expand;
        cmd->w             = node->w + expand * 2.0f;
        cmd->h             = node->h + expand * 2.0f;
        cmd->r = sr; cmd->g = sg; cmd->b = sb; cmd->a = sa;
        cmd->corner_radius = node->desc.corner_radius + expand;
        cmd->z_index       = node->desc.z_index;
        cmd->in_use        = true;
        set_clip(cmd, clip);
    }

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
        cmd->z_index       = node->desc.z_index;
        /* Border */
        cmd->border_width  = node->desc.border_width;
        if (node->desc.border_color != 0) {
            unpack_color(node->desc.border_color,
                         &cmd->border_r, &cmd->border_g,
                         &cmd->border_b, &cmd->border_a);
        }
        cmd->in_use        = true;
        set_clip(cmd, clip);
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

        /* Hover highlight on header row */
        if (win->mouse_x >= hdr->x && win->mouse_x <= hdr->x + hdr->w &&
            win->mouse_y >= hdr->y && win->mouse_y <= hdr->y + hdr->h) {
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = hdr->x; c->y = hdr->y;
                c->w = hdr->w; c->h = hdr->h;
                unpack_color(hdr->desc.background, &c->r, &c->g, &c->b, &c->a);
                c->corner_radius = hdr->desc.corner_radius;
            }
        }

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
                ? "\xEF\x83\x97"   /* U+F0D7 caret-down  */
                : "\xEF\x83\x9A"; /* U+F0DA caret-right */
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
        bool hovered = (win->hovered_node == node);
        uint32_t color = hovered ? sp->bar_hover_color : sp->bar_color;
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            Ca_Instance *inst = win->instance;
            int16_t img_idx = (int16_t)(img - inst->images);
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type        = CA_DRAW_IMAGE;
            cmd->x           = node->x;
            cmd->y           = node->y;
            cmd->w           = node->w;
            cmd->h           = node->h;
            cmd->r = 1; cmd->g = 1; cmd->b = 1; cmd->a = 1;
            cmd->u0 = 0; cmd->v0 = 0; cmd->u1 = 1; cmd->v1 = 1;
            cmd->image_index = img_idx;
            cmd->z_index     = node->desc.z_index;
            cmd->in_use      = true;
            set_clip(cmd, clip);
        }
        break;
    }
    case CA_WIDGET_VIEWPORT: {
        Ca_Viewport *vp = (Ca_Viewport *)node->widget;
        if (!vp || !vp->in_use) break;
        if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
            int16_t vp_idx = (int16_t)(vp - win->viewport_pool);
            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type           = CA_DRAW_VIEWPORT;
            cmd->x              = node->x;
            cmd->y              = node->y;
            cmd->w              = node->w;
            cmd->h              = node->h;
            cmd->r = 1; cmd->g = 1; cmd->b = 1; cmd->a = 1;
            cmd->u0 = 0; cmd->v0 = 0; cmd->u1 = 1; cmd->v1 = 1;
            cmd->viewport_index = vp_idx;
            cmd->z_index        = node->desc.z_index;
            cmd->in_use         = true;
            set_clip(cmd, clip);
        }
        break;
    }
    default: break;
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

/* Helper: glyph advance for a codepoint */
static inline float glyph_adv(Ca_FontTier *tier, uint32_t cp, float cs_eff)
{
    stbtt_packedchar *g = ca_font_glyph(tier, cp);
    return g ? g->xadvance / cs_eff : 0.0f;
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
    Ca_FontTier *tier  = ca_font_tier(font, desired_size);
    float font_scale   = desired_size / tier->logical_px;
    float cs_eff       = cs / font_scale;

    float line_height = (tier->ascent - tier->descent + tier->line_gap) * font_scale;
    if (line_height < 1.0f) line_height = desired_size * 1.3f;

    float max_w = node->w - node->desc.padding_left - node->desc.padding_right;
    if (max_w < 1.0f) return;

    float space_adv = glyph_adv(tier, ' ', cs_eff);

    /* First pass: determine line breaks (word wrap). */
    float cur_line_w = 0.0f;
    int line_count = 1;
    const char *p = text;
    while (*p) {
        const char *word_start = p;
        float word_w = 0.0f;
        while (*p && *p != ' ' && *p != '\n') {
            uint32_t cp = ca_utf8_decode(&p);
            word_w += glyph_adv(tier, cp, cs_eff);
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
                    + (tier->ascent * font_scale + tier->descent * font_scale) * 0.5f
                    + line_height * 0.5f;
    float left_x  = node->x + node->desc.padding_left;
    cur_line_w = 0.0f;
    int cur_line = 0;

    float xpos = floorf(left_x * cs_eff + 0.5f);
    float ypos = floorf(start_y * cs_eff + 0.5f);

    ClipRect clip = find_clip_for_node(node);
    ClipRect node_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);

    p = text;
    while (*p) {
        /* Measure next word */
        const char *wp = p;
        float word_w = 0.0f;
        while (*wp && *wp != ' ' && *wp != '\n') {
            const char *prev = wp;
            uint32_t cp = ca_utf8_decode(&wp);
            word_w += glyph_adv(tier, cp, cs_eff);
        }
        float with_space = (cur_line_w > 0.0f) ? cur_line_w + space_adv + word_w : word_w;
        if (cur_line_w > 0.0f && with_space > max_w) {
            cur_line++;
            cur_line_w = word_w;
            xpos = floorf(left_x * cs_eff + 0.5f);
            ypos = floorf((start_y + line_height * cur_line) * cs_eff + 0.5f);
        } else {
            if (cur_line_w > 0.0f) {
                xpos += space_adv * cs_eff;
                cur_line_w += space_adv;
            }
            cur_line_w += word_w;
        }

        /* Emit glyphs for this word */
        while (p < wp) {
            uint32_t cp = ca_utf8_decode(&p);
            stbtt_packedchar *pc = ca_font_glyph(tier, cp);
            if (!pc) continue;
            if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) goto done;

            stbtt_aligned_quad q;
            ca_font_get_quad(pc, font->atlas_w, font->atlas_h, &xpos, &ypos, &q);
            float gw = (q.x1 - q.x0) / cs_eff;
            float gh = (q.y1 - q.y0) / cs_eff;
            if (gw < 0.5f || gh < 0.5f) continue;

            Ca_DrawCmd *cmd = &win->draw_cmds[win->draw_cmd_count++];
            memset(cmd, 0, sizeof(*cmd));
            cmd->type = CA_DRAW_GLYPH;
            cmd->x = q.x0 / cs_eff; cmd->y = q.y0 / cs_eff;
            cmd->w = gw; cmd->h = gh;
            cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
            cmd->u0 = q.s0; cmd->v0 = q.t0;
            cmd->u1 = q.s1; cmd->v1 = q.t1;
            cmd->z_index = node->desc.z_index;
            cmd->in_use = true;
            set_clip(cmd, node_clip);
        }
        if (*p == '\n') {
            cur_line++;
            cur_line_w = 0;
            xpos = floorf(left_x * cs_eff + 0.5f);
            ypos = floorf((start_y + line_height * cur_line) * cs_eff + 0.5f);
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

    float r, g, b, a;
    if (packed_color == 0) {
        r = g = b = a = 1.0f;
    } else {
        r = (float)((packed_color >> 24) & 0xFF) / 255.0f;
        g = (float)((packed_color >> 16) & 0xFF) / 255.0f;
        b = (float)((packed_color >>  8) & 0xFF) / 255.0f;
        a = (float)((packed_color)       & 0xFF) / 255.0f;
    }

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    float cs   = font->content_scale / ui_s;
    float desired_size = node->desc.font_size > 0.0f ? node->desc.font_size : font->default_size;
    Ca_FontTier *tier  = ca_font_tier(font, desired_size);
    float font_scale   = desired_size / tier->logical_px;
    float cs_eff       = cs / font_scale;

    /* Measure total advance width in logical pixels */
    float text_w = 0.0f;
    {
        const char *p = text;
        while (*p) {
            uint32_t cp = ca_utf8_decode(&p);
            stbtt_packedchar *pc = ca_font_glyph(tier, cp);
            if (pc) text_w += pc->xadvance / cs_eff;
        }
    }

    float baseline_logical =
        node->y + node->h * 0.5f
        + (tier->ascent * font_scale + tier->descent * font_scale) * 0.5f;
    float left_logical;
    if (text_w > node->w) {
        left_logical = node->x + node->desc.padding_left;
    } else {
        switch (node->desc.text_align) {
        case 1:  left_logical = node->x + node->desc.padding_left; break;
        case 2:  left_logical = node->x + node->w - text_w - node->desc.padding_right; break;
        default: left_logical = node->x + (node->w - text_w) * 0.5f; break;
        }
    }

    /* Snap run origin to the nearest physical pixel (macOS-style: crisp
       baseline; individual advances still accumulate fractionally for
       accurate kerning with 1x1 atlas glyph coverage AA). */
    float xpos = floorf(left_logical * cs_eff + 0.5f);
    float ypos = floorf(baseline_logical * cs_eff + 0.5f);

    ClipRect clip      = find_clip_for_node(node);
    ClipRect node_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);

    const char *p = text;
    while (*p) {
        uint32_t cp = ca_utf8_decode(&p);
        stbtt_packedchar *pc = ca_font_glyph(tier, cp);
        if (!pc) continue;
        if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) break;

        stbtt_aligned_quad q;
        ca_font_get_quad(pc, font->atlas_w, font->atlas_h, &xpos, &ypos, &q);

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
        cmd->z_index = node->desc.z_index;
        cmd->in_use = true;
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
    float desired_size = node->desc.font_size > 0.0f ? node->desc.font_size : font->default_size;
    Ca_FontTier *tier  = ca_font_tier(font, desired_size);
    float font_scale   = desired_size / tier->logical_px;
    float cs_eff       = cs / font_scale;

    float baseline_logical =
        node->y + node->h * 0.5f
        + (tier->ascent * font_scale + tier->descent * font_scale) * 0.5f;
    float left_logical = node->x + node->desc.padding_left;

    float xpos = floorf(left_logical * cs_eff + 0.5f);
    float ypos = floorf(baseline_logical * cs_eff + 0.5f);

    ClipRect clip       = find_clip_for_node(node);
    ClipRect input_clip = clip_intersect(clip, node->x, node->y, node->w, node->h);

    const char *p = text;
    while (*p) {
        uint32_t cp = ca_utf8_decode(&p);
        stbtt_packedchar *pc = ca_font_glyph(tier, cp);
        if (!pc) continue;
        if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) break;

        stbtt_aligned_quad q;
        ca_font_get_quad(pc, font->atlas_w, font->atlas_h, &xpos, &ypos, &q);

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
        set_clip(cmd, input_clip);
    }
}

/* Measure x-advance for a substring of text (byte_count bytes) */
static float measure_text_advance(Ca_Font *font, const char *text, int byte_count,
                                  float content_scale, float ui_scale,
                                  float font_size)
{
    float ui_s = ui_scale > 0.0f ? ui_scale : 1.0f;
    float cs   = content_scale / ui_s;
    float desired = font_size > 0.0f ? font_size : font->default_size;
    Ca_FontTier *tier = ca_font_tier(font, desired);
    float fs     = desired / tier->logical_px;
    float cs_eff = cs / fs;
    float w = 0.0f;
    const char *p   = text;
    const char *end = text + byte_count;
    while (*p && p < end) {
        uint32_t cp = ca_utf8_decode(&p);
        stbtt_packedchar *pc = ca_font_glyph(tier, cp);
        if (pc) w += pc->xadvance / cs_eff;
    }
    return w;
}

/* Paint a thin cursor line at the given cursor byte offset in the input text */
static void paint_cursor(Ca_Window *win, Ca_Font *font,
                         Ca_Node *node, const char *text, int cursor_pos)
{
    if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) return;

    float advance = measure_text_advance(font, text, cursor_pos,
                                         font->content_scale, win->ui_scale,
                                         node->desc.font_size);
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

    /* Worst case: every in-use node has both pre and post entries */
    CacheSpan spans[CA_MAX_NODES_PER_WINDOW * 2];
    uint32_t span_count = 0;

    for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
        Ca_Node *n = &win->node_pool[i];
        if (!n->in_use) continue;
        if (n->cache_count > 0 && span_count < CA_MAX_NODES_PER_WINDOW * 2)
            spans[span_count++] = (CacheSpan){ n->cache_start, n->cache_count, &n->cache_start };
        if (n->cache_post_count > 0 && span_count < CA_MAX_NODES_PER_WINDOW * 2)
            spans[span_count++] = (CacheSpan){ n->cache_post_start, n->cache_post_count, &n->cache_post_start };
    }

    if (span_count == 0) {
        win->paint_cache_used = 0;
        return;
    }

    /* Insertion sort by start position (runs only during infrequent compaction) */
    for (uint32_t i = 1; i < span_count; ++i) {
        CacheSpan tmp = spans[i];
        uint32_t j = i;
        while (j > 0 && spans[j - 1].start > tmp.start) {
            spans[j] = spans[j - 1];
            j--;
        }
        spans[j] = tmp;
    }

    /* Compact forward — dest <= source always holds because we
       process spans in ascending start order. */
    uint32_t dest = 0;
    for (uint32_t i = 0; i < span_count; ++i) {
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

static void paint_tree_cached(Ca_Instance *inst, Ca_Window *win,
                              Ca_Node *node, ClipRect clip)
{
    if (!node || !node->in_use) return;

    /* Hidden nodes produce no draw commands.  Clear dirty flags on the
       entire subtree so they don't perpetually trigger paint passes. */
    if (node->desc.hidden) {
        clear_dirty_recursive(node);
        return;
    }

    bool was_dirty = (node->dirty & CA_DIRTY_CONTENT) != 0;

    /* ---- Pre-children: background + widget visuals ---- */
    if (was_dirty) {
        uint32_t start = win->draw_cmd_count;
        paint_node_content(win, inst->font, node, clip);
        uint32_t count = win->draw_cmd_count - start;
        cache_commands(win, node, start, count, false);
        node->dirty &= ~CA_DIRTY_CONTENT;
        if (win->debug_overlay && !win->dbg_force_repaint)
            node->dbg_repainted = true;
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
            {
                float cs = font->content_scale / (win->ui_scale > 0 ? win->ui_scale : 1.0f);
                Ca_FontTier *tier = ca_font_tier(font, font->default_size);
                float fs = font->default_size / tier->logical_px;
                float cs_eff = cs / fs;
                const char *tp = tt->text;
                while (*tp) {
                    uint32_t cp = ca_utf8_decode(&tp);
                    stbtt_packedchar *pc = ca_font_glyph(tier, cp);
                    if (pc) tw += pc->xadvance / cs_eff;
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

    /* ---- Menu bar dropdowns ---- */
    if (win->menubar_pool && font) {
        for (uint32_t i = 0; i < CA_MAX_MENUBARS_PER_WINDOW; ++i) {
            Ca_MenuBar *mb = &win->menubar_pool[i];
            if (!mb->in_use || !mb->node || mb->active_menu < 0) continue;

            /* Highlight the active header */
            Ca_MenuBarMenu *am = &mb->menus[mb->active_menu];
            Ca_Node *hdr = am->header_node;
            if (!hdr) continue;

            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = hdr->x; c->y = hdr->y;
                c->w = hdr->w; c->h = hdr->h;
                unpack_color(mb->header_highlight, &c->r, &c->g, &c->b, &c->a);
                c->in_use = true;
                c->overlay = true;
            }

            float item_h = 24.0f;
            float menu_w = 160.0f;
            float drop_x = hdr->x;
            float drop_y = hdr->y + hdr->h;
            float menu_h = item_h * (float)am->item_count;

            /* Dropdown background */
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                memset(c, 0, sizeof(*c));
                c->type = CA_DRAW_RECT;
                c->x = drop_x; c->y = drop_y;
                c->w = menu_w; c->h = menu_h;
                c->corner_radius = 3.0f;
                unpack_color(mb->dropdown_bg, &c->r, &c->g, &c->b, &c->a);
                c->in_use = true;
                c->overlay = true;
                c->border_width = 1.0f;
                unpack_color(mb->dropdown_border,
                             &c->border_r, &c->border_g,
                             &c->border_b, &c->border_a);
            }

            /* Dropdown items */
            for (int ii = 0; ii < am->item_count; ++ii) {
                float iy = drop_y + item_h * (float)ii;

                /* Hover highlight */
                if (win->mouse_x >= drop_x && win->mouse_x <= drop_x + menu_w &&
                    win->mouse_y >= iy && win->mouse_y <= iy + item_h) {
                    if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
                        Ca_DrawCmd *c = &win->draw_cmds[win->draw_cmd_count++];
                        memset(c, 0, sizeof(*c));
                        c->type = CA_DRAW_RECT;
                        c->x = drop_x; c->y = iy;
                        c->w = menu_w; c->h = item_h;
                        unpack_color(mb->dropdown_hover, &c->r, &c->g, &c->b, &c->a);
                        c->in_use = true;
                        c->overlay = true;
                    }
                }

                uint32_t glyph_start = win->draw_cmd_count;
                Ca_Node tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.in_use = true;
                tmp.x = drop_x + 12.0f;
                tmp.y = iy;
                tmp.w = menu_w - 24.0f;
                tmp.h = item_h;
                tmp.window = win;
                tmp.desc.text_align = 1; /* left-align */
                paint_text(win, font, &tmp, am->items[ii].label,
                           mb->dropdown_text);
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
   DEBUG OVERLAY — toggled by F9, shows rendering stats
   ================================================================ */

static void paint_debug_overlay(Ca_Instance *inst, Ca_Window *win)
{
    Ca_Font *font = inst->font;
    if (!font) return;

    /* --- Paint-flash: green tinted rect over nodes repainted THIS frame --- */
    uint32_t repainted_count = 0;
    if (win->node_pool) {
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
            Ca_Node *n = &win->node_pool[i];
            if (!n->in_use || !n->dbg_repainted) continue;
            if (n->w < 1.0f || n->h < 1.0f) continue;
            if (win->draw_cmd_count >= CA_MAX_DRAW_CMDS_PER_WINDOW) break;

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
            if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
    if (win->node_pool) {
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i)
            if (win->node_pool[i].in_use) node_count++;
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
    if (win->draw_cmd_count < CA_MAX_DRAW_CMDS_PER_WINDOW) {
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
        tmp.desc.text_align = 1; /* left */                            \
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
             node_count, (uint32_t)CA_MAX_NODES_PER_WINDOW, repainted_count);
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

    /* 5. Debug overlay — when enabled, always fresh */
    if (win->debug_overlay)
        paint_debug_overlay(inst, win);
}
