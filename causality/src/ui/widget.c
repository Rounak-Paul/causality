/* widget.c — public widget API backed by the internal node/state system */
#include "widget.h"
#include "node.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ---- Internal helpers ---- */

static Ca_Direction orientation_to_dir(Ca_Orientation o)
{
    return (o == CA_VERTICAL) ? CA_DIR_COLUMN : CA_DIR_ROW;
}

static Ca_NodeDesc panel_to_node_desc(const Ca_PanelDesc *d)
{
    Ca_NodeDesc nd = {0};
    nd.width          = d->width;
    nd.height         = d->height;
    nd.padding_top    = d->padding_top;
    nd.padding_right  = d->padding_right;
    nd.padding_bottom = d->padding_bottom;
    nd.padding_left   = d->padding_left;
    nd.gap            = d->gap;
    nd.direction      = orientation_to_dir(d->orientation);
    nd.background     = d->background;
    nd.corner_radius  = d->corner_radius;
    return nd;
}

static Ca_Panel *alloc_panel(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_PANELS_PER_WINDOW; ++i) {
        if (!win->panel_pool[i].in_use)
            return &win->panel_pool[i];
    }
    fprintf(stderr, "[causality] panel pool exhausted (max %d)\n", CA_MAX_PANELS_PER_WINDOW);
    return NULL;
}

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

/* ======================================================
   PANEL
   ====================================================== */

Ca_Panel *ca_panel_create(Ca_Window *window, const Ca_PanelDesc *desc)
{
    assert(window && desc);

    Ca_Panel *slot = alloc_panel(window);
    if (!slot) return NULL;

    Ca_NodeDesc nd = panel_to_node_desc(desc);

    /* ca_node_root is idempotent — returns existing root or creates one */
    Ca_Node *root = ca_node_root(window);
    if (!root) return NULL;
    ca_node_set_desc(root, &nd);

    slot->node   = root;
    slot->window = window;
    slot->in_use = true;
    return slot;
}

Ca_Panel *ca_panel_add(Ca_Panel *parent, const Ca_PanelDesc *desc)
{
    assert(parent && parent->in_use && desc);

    Ca_Window *win = parent->window;
    Ca_Panel  *slot = alloc_panel(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd   = panel_to_node_desc(desc);
    Ca_Node    *node = ca_node_add(parent->node, &nd);
    if (!node) return NULL;

    slot->node   = node;
    slot->window = win;
    slot->in_use = true;
    return slot;
}

void ca_panel_destroy(Ca_Panel *panel)
{
    if (!panel || !panel->in_use) return;
    ca_node_remove(panel->node);
    memset(panel, 0, sizeof(*panel));
}

void ca_panel_set_background(Ca_Panel *panel, uint32_t color)
{
    assert(panel && panel->in_use);
    panel->node->desc.background = color;
    panel->node->dirty |= CA_DIRTY_CONTENT;
}

/* ======================================================
   LABEL
   ====================================================== */

Ca_Label *ca_label_add(Ca_Panel *parent, const Ca_LabelDesc *desc)
{
    assert(parent && parent->in_use && desc);

    Ca_Window *win  = parent->window;
    Ca_Label  *slot = alloc_label(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd = {0};
    nd.width       = desc->width;
    nd.height      = (desc->height > 0.0f) ? desc->height : 24.0f;
    nd.background  = 0; /* transparent */

    Ca_Node *node = ca_node_add(parent->node, &nd);
    if (!node) return NULL;

    slot->node   = node;
    slot->in_use = true;
    slot->color  = desc->color;
    if (desc->text)
        snprintf(slot->text, CA_LABEL_TEXT_MAX, "%s", desc->text);
    return slot;
}

void ca_label_destroy(Ca_Label *label)
{
    if (!label || !label->in_use) return;
    ca_node_remove(label->node);
    memset(label, 0, sizeof(*label));
}

void ca_label_set_text(Ca_Label *label, const char *text)
{
    assert(label && label->in_use);
    snprintf(label->text, CA_LABEL_TEXT_MAX, "%s", text ? text : "");
    label->node->dirty |= CA_DIRTY_CONTENT;
}

/* ======================================================
   BUTTON
   ====================================================== */

Ca_Button *ca_button_add(Ca_Panel *parent, const Ca_ButtonDesc *desc)
{
    assert(parent && parent->in_use && desc);

    Ca_Window *win  = parent->window;
    Ca_Button *slot = alloc_button(win);
    if (!slot) return NULL;

    Ca_NodeDesc nd   = {0};
    nd.width         = (desc->width  > 0.0f) ? desc->width  : 120.0f;
    nd.height        = (desc->height > 0.0f) ? desc->height :  36.0f;
    nd.background    = desc->background;
    nd.corner_radius = desc->corner_radius;

    Ca_Node *node = ca_node_add(parent->node, &nd);
    if (!node) return NULL;

    slot->node       = node;
    slot->in_use     = true;
    slot->text_color = desc->text_color;
    if (desc->text)
        snprintf(slot->text, CA_BUTTON_TEXT_MAX, "%s", desc->text);
    return slot;
}

void ca_button_destroy(Ca_Button *button)
{
    if (!button || !button->in_use) return;
    ca_node_remove(button->node);
    memset(button, 0, sizeof(*button));
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

void ca_button_on_click(Ca_Button *button, Ca_ClickFn fn, void *user_data)
{
    assert(button && button->in_use);
    button->on_click   = fn;
    button->click_data = user_data;
}

/* ======================================================
   INPUT PASS — hit-test buttons against the mouse
   ====================================================== */

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
