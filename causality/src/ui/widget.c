/* widget.c — HTML-like declarative UI elements
 *
 * Every element can nest children, just like HTML.
 * An implicit parent stack tracks hierarchy automatically.
 *
 *     ca_ui_begin(window, &(Ca_DivDesc){ ... });
 *       ca_text(&(Ca_TextDesc){ .text = "Hello" });
 *       ca_btn_begin(&(Ca_BtnDesc){ .on_click = fn });
 *         ca_text(&(Ca_TextDesc){ .text = "Click me" });
 *       ca_btn_end();
 *       ca_list_begin(NULL);
 *         ca_li_begin(NULL);
 *           ca_text(&(Ca_TextDesc){ .text = "Item 1" });
 *         ca_li_end();
 *       ca_list_end();
 *     ca_ui_end();
 */
#include "widget.h"
#include "node.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ============================================================
   INTERNAL — convert descriptors to Ca_NodeDesc (scaled)
   ============================================================ */

static Ca_Direction dir_from_int(int direction)
{
    return (direction == CA_VERTICAL) ? CA_DIR_COLUMN : CA_DIR_ROW;
}

/* Forward-declared; defined after g_ctx. */
static Ca_NodeDesc div_to_nd(const Ca_DivDesc *d);
static float s(float v);

/* ============================================================
   INTERNAL — pool allocators (label + button only)
   ============================================================ */

static Ca_Label *alloc_label(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_LABELS_PER_WINDOW; ++i) {
        if (!win->label_pool[i].in_use)
            return &win->label_pool[i];
    }
    fprintf(stderr, "[causality] label pool exhausted (max %d)\n", CA_MAX_LABELS_PER_WINDOW);
    return NULL;
}

static Ca_Button *alloc_button(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
        if (!win->button_pool[i].in_use)
            return &win->button_pool[i];
    }
    fprintf(stderr, "[causality] button pool exhausted (max %d)\n", CA_MAX_BUTTONS_PER_WINDOW);
    return NULL;
}

/* ============================================================
   INTERNAL — node creation helpers
   ============================================================ */

/* Create a container node (used by div, span, ul, ol, li, btn_begin) */
static Ca_Node *add_container(Ca_Node *parent, const Ca_NodeDesc *nd)
{
    return ca_node_add(parent, nd);
}

/* Create a label under a parent node */
static Ca_Label *add_label(Ca_Window *win, Ca_Node *parent, const Ca_TextDesc *desc)
{
    Ca_Label *slot = alloc_label(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width  = s(desc->width);
    nd.height = s((desc->height > 0.0f) ? desc->height : 16.0f);

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) return NULL;

    slot->node   = node;
    slot->in_use = true;
    slot->color  = desc->color;
    if (desc->text)
        snprintf(slot->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    return slot;
}

/* Create a button under a parent node */
static Ca_Button *add_button(Ca_Window *win, Ca_Node *parent, const Ca_BtnDesc *desc)
{
    Ca_Button *slot = alloc_button(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd   = {0};
    nd.width         = s((desc->width  > 0.0f) ? desc->width  : 72.0f);
    nd.height        = s((desc->height > 0.0f) ? desc->height : 24.0f);
    nd.background    = desc->background;
    nd.corner_radius = s(desc->corner_radius);
    nd.padding_top   = s(desc->padding[0]);
    nd.padding_right = s(desc->padding[1]);
    nd.padding_bottom= s(desc->padding[2]);
    nd.padding_left  = s(desc->padding[3]);
    nd.gap           = s(desc->gap);
    nd.direction     = dir_from_int(desc->direction);

    Ca_Node *node = ca_node_add(parent, &nd);
    if (!node) return NULL;

    slot->node       = node;
    slot->in_use     = true;
    slot->text_color = desc->text_color;
    if (desc->text)
        snprintf(slot->text, CA_BUTTON_TEXT_MAX, "%s", desc->text);
    if (desc->on_click) {
        slot->on_click   = desc->on_click;
        slot->click_data = desc->click_data;
    }
    return slot;
}

/* ============================================================
   IMPLICIT PARENT STACK — stores Ca_Node* directly
   ============================================================ */

#define CA_STACK_MAX 64

static struct {
    Ca_Window *window;
    Ca_Node   *stack[CA_STACK_MAX];
    int        depth;    /* index of top; -1 = empty */
    bool       active;
} g_ctx;

static Ca_Node *ctx_top(void)
{
    assert(g_ctx.active && g_ctx.depth >= 0);
    return g_ctx.stack[g_ctx.depth];
}

static void ctx_push(Ca_Node *node)
{
    assert(g_ctx.depth + 1 < CA_STACK_MAX);
    g_ctx.stack[++g_ctx.depth] = node;
}

static void ctx_pop(void)
{
    assert(g_ctx.depth >= 0);
    --g_ctx.depth;
}

/* Scale a value by the window's UI scale factor */
static float s(float v) { return v * g_ctx.window->ui_scale; }

static Ca_NodeDesc div_to_nd(const Ca_DivDesc *d)
{
    Ca_NodeDesc nd = {0};
    if (!d) return nd;
    nd.width          = s(d->width);
    nd.height         = s(d->height);
    nd.padding_top    = s(d->padding[0]);
    nd.padding_right  = s(d->padding[1]);
    nd.padding_bottom = s(d->padding[2]);
    nd.padding_left   = s(d->padding[3]);
    nd.gap            = s(d->gap);
    nd.direction      = dir_from_int(d->direction);
    nd.background     = d->background;
    nd.corner_radius  = s(d->corner_radius);
    return nd;
}

/* ============================================================
   PUBLIC — tree root
   ============================================================ */

void ca_ui_begin(Ca_Window *window, const Ca_DivDesc *root_desc)
{
    assert(window);
    assert(!g_ctx.active && "ca_ui_begin called without matching ca_ui_end");

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.window = window;
    g_ctx.depth  = -1;
    g_ctx.active = true;

    Ca_NodeDesc nd = div_to_nd(root_desc);
    Ca_Node *root  = ca_node_root(window);
    assert(root && "failed to create root node");
    ca_node_set_desc(root, &nd);
    ctx_push(root);
}

void ca_ui_end(void)
{
    assert(g_ctx.active && "ca_ui_end called without ca_ui_begin");
    assert(g_ctx.depth == 0 && "mismatched begin / end calls");
    g_ctx.active = false;
    g_ctx.depth  = -1;
}

/* ============================================================
   PUBLIC — <div>
   ============================================================ */

void ca_div_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    Ca_Node *node  = add_container(ctx_top(), &nd);
    assert(node);
    ctx_push(node);
}

void ca_div_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — <p> / <text>  (self-closing leaf)
   ============================================================ */

Ca_Label *ca_text(const Ca_TextDesc *desc)
{
    assert(g_ctx.active && desc);
    return add_label(g_ctx.window, ctx_top(), desc);
}

/* ============================================================
   PUBLIC — <button>  (nestable OR self-closing)
   ============================================================ */

/* Self-closing: ca_btn creates a leaf button with built-in text */
Ca_Button *ca_btn(const Ca_BtnDesc *desc)
{
    assert(g_ctx.active && desc);
    return add_button(g_ctx.window, ctx_top(), desc);
}

/* Nestable: btn_begin pushes the button onto the stack so children
   (text, icons, other elements) are laid out inside the button rect. */
Ca_Button *ca_btn_begin(const Ca_BtnDesc *desc)
{
    assert(g_ctx.active && desc);
    Ca_Button *btn = add_button(g_ctx.window, ctx_top(), desc);
    assert(btn);
    ctx_push(btn->node);
    return btn;
}

void ca_btn_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — <list>  (vertical container, default gap 4)
   ============================================================ */

void ca_list_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_COLUMN;
    if (!desc || nd.gap <= 0.0f) nd.gap = s(2.0f);
    Ca_Node *node = add_container(ctx_top(), &nd);
    assert(node);
    ctx_push(node);
}

void ca_list_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — <li>  (list item — horizontal container, default gap 8)
   ============================================================ */

void ca_li_begin(const Ca_DivDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = div_to_nd(desc);
    nd.direction = CA_DIR_ROW;
    if (!desc || nd.gap <= 0.0f) nd.gap = s(4.0f);
    Ca_Node *node = add_container(ctx_top(), &nd);
    assert(node);
    ctx_push(node);
}

void ca_li_end(void)
{
    assert(g_ctx.active && g_ctx.depth > 0);
    ctx_pop();
}

/* ============================================================
   PUBLIC — <hr>  (horizontal rule — self-closing)
   ============================================================ */

void ca_hr(const Ca_HrDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = {0};
    nd.height     = s((desc && desc->thickness > 0.0f) ? desc->thickness : 1.0f);
    nd.background = (desc && desc->color) ? desc->color : ca_color(0.3f, 0.3f, 0.3f, 1.0f);
    add_container(ctx_top(), &nd);
}

/* ============================================================
   PUBLIC — spacer  (invisible spacing element)
   ============================================================ */

void ca_spacer(const Ca_SpacerDesc *desc)
{
    assert(g_ctx.active);
    Ca_NodeDesc nd = {0};
    if (desc) {
        nd.width  = s(desc->width);
        nd.height = s(desc->height);
    }
    add_container(ctx_top(), &nd);
}

/* ============================================================
   PUBLIC — headings  (convenience wrappers around ca_text)
   ============================================================ */

static Ca_Label *heading(const Ca_TextDesc *desc, float default_height)
{
    assert(g_ctx.active && desc);
    Ca_TextDesc hd = *desc;
    if (hd.height <= 0.0f) hd.height = default_height;
    return add_label(g_ctx.window, ctx_top(), &hd);
}

Ca_Label *ca_h1(const Ca_TextDesc *desc) { return heading(desc, 24.0f); }
Ca_Label *ca_h2(const Ca_TextDesc *desc) { return heading(desc, 20.0f); }
Ca_Label *ca_h3(const Ca_TextDesc *desc) { return heading(desc, 18.0f); }
Ca_Label *ca_h4(const Ca_TextDesc *desc) { return heading(desc, 16.0f); }
Ca_Label *ca_h5(const Ca_TextDesc *desc) { return heading(desc, 14.0f); }
Ca_Label *ca_h6(const Ca_TextDesc *desc) { return heading(desc, 12.0f); }

/* ============================================================
   PUBLIC — runtime setters
   ============================================================ */

void ca_label_set_text(Ca_Label *label, const char *text)
{
    assert(label && label->in_use);
    snprintf(label->text, CA_LABEL_TEXT_MAX, "%s", text ? text : "");
    label->node->dirty |= CA_DIRTY_CONTENT;
}

void ca_button_set_text(Ca_Button *button, const char *text)
{
    assert(button && button->in_use);
    snprintf(button->text, CA_BUTTON_TEXT_MAX, "%s", text ? text : "");
    button->node->dirty |= CA_DIRTY_CONTENT;
}

void ca_button_set_background(Ca_Button *button, uint32_t color)
{
    assert(button && button->in_use);
    button->node->desc.background = color;
    button->node->dirty |= CA_DIRTY_CONTENT;
}

/* ============================================================
   INPUT PASS — hit-test buttons against the mouse
   ============================================================ */

void ca_widget_input_pass(Ca_Window *win)
{
    if (!win->mouse_click_this_frame || !win->button_pool) return;

    float mx = (float)win->mouse_x;
    float my = (float)win->mouse_y;

    for (uint32_t i = 0; i < CA_MAX_BUTTONS_PER_WINDOW; ++i) {
        Ca_Button *btn = &win->button_pool[i];
        if (!btn->in_use || !btn->on_click || !btn->node) continue;

        Ca_Node *n = btn->node;
        if (mx >= n->x && mx <= n->x + n->w &&
            my >= n->y && my <= n->y + n->h)
        {
            btn->on_click(btn, btn->click_data);
        }
    }
}
