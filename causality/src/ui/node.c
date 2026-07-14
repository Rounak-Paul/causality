// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* node.c — node pool, tree building, and subscription wiring */
#include "node.h"

#include <string.h>
#include <assert.h>

void ca_node_system_init(Ca_Window *win)
{
    win->node_pool      = (Ca_Node *)CA_CALLOC(CA_MAX_NODES_PER_WINDOW, sizeof(Ca_Node));
    win->draw_cmds      = (Ca_DrawCmd *)CA_CALLOC(CA_MAX_DRAW_CMDS_PER_WINDOW, sizeof(Ca_DrawCmd));
    win->label_pool     = (Ca_Label *)CA_CALLOC(CA_MAX_LABELS_PER_WINDOW,  sizeof(Ca_Label));
    win->button_pool    = (Ca_Button *)CA_CALLOC(CA_MAX_BUTTONS_PER_WINDOW, sizeof(Ca_Button));
    win->input_pool     = (Ca_TextInput *)CA_CALLOC(CA_MAX_INPUTS_PER_WINDOW, sizeof(Ca_TextInput));
    win->checkbox_pool  = (Ca_Checkbox *)CA_CALLOC(CA_MAX_CHECKBOXES_PER_WINDOW, sizeof(Ca_Checkbox));
    win->radio_pool     = (Ca_Radio *)CA_CALLOC(CA_MAX_RADIOS_PER_WINDOW, sizeof(Ca_Radio));
    win->slider_pool    = (Ca_Slider *)CA_CALLOC(CA_MAX_SLIDERS_PER_WINDOW, sizeof(Ca_Slider));
    win->toggle_pool    = (Ca_Toggle *)CA_CALLOC(CA_MAX_TOGGLES_PER_WINDOW, sizeof(Ca_Toggle));
    win->progress_pool  = (Ca_Progress *)CA_CALLOC(CA_MAX_PROGRESS_PER_WINDOW, sizeof(Ca_Progress));
    win->select_pool    = (Ca_Select *)CA_CALLOC(CA_MAX_SELECTS_PER_WINDOW, sizeof(Ca_Select));
    win->tabbar_pool    = (Ca_TabBar *)CA_CALLOC(CA_MAX_TABBARS_PER_WINDOW, sizeof(Ca_TabBar));
    win->treenode_pool  = (Ca_TreeNode *)CA_CALLOC(CA_MAX_TREENODES_PER_WINDOW, sizeof(Ca_TreeNode));
    win->table_pool     = (Ca_Table *)CA_CALLOC(CA_MAX_TABLES_PER_WINDOW, sizeof(Ca_Table));
    win->tooltip_pool   = (Ca_Tooltip *)CA_CALLOC(CA_MAX_TOOLTIPS_PER_WINDOW, sizeof(Ca_Tooltip));
    win->ctxmenu_pool   = (Ca_CtxMenu *)CA_CALLOC(CA_MAX_CTXMENUS_PER_WINDOW, sizeof(Ca_CtxMenu));
    win->modal_pool     = (Ca_Modal *)CA_CALLOC(CA_MAX_MODALS_PER_WINDOW, sizeof(Ca_Modal));
    win->splitter_pool   = (Ca_Splitter *)CA_CALLOC(CA_MAX_SPLITTERS_PER_WINDOW, sizeof(Ca_Splitter));
    win->viewport_pool   = (Ca_Viewport *)CA_CALLOC(CA_MAX_VIEWPORTS_PER_WINDOW, sizeof(Ca_Viewport));
    win->menubar_pool    = (Ca_MenuBar *)CA_CALLOC(CA_MAX_MENUBARS_PER_WINDOW, sizeof(Ca_MenuBar));
    win->root           = NULL;
    win->draw_cmd_count = 0;
    win->sorted_idx      = (uint32_t *)CA_CALLOC(CA_MAX_DRAW_CMDS_PER_WINDOW, sizeof(uint32_t));
    win->paint_cache     = (Ca_DrawCmd *)CA_CALLOC(CA_MAX_DRAW_CMDS_PER_WINDOW, sizeof(Ca_DrawCmd));
    win->paint_cache_used = 0;
    win->layout_scratch  = (float *)CA_CALLOC((size_t)CA_MAX_NODES_PER_WINDOW * 7u, sizeof(float));
    win->layout_scratch_capacity = CA_MAX_NODES_PER_WINDOW;
    win->layout_scratch_used = 0;
    win->hovered_node   = NULL;
    win->drag_node      = NULL;

    /* Pre-set all draw_cmd_idx to -1 (0 is a valid slot index) */
    for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i)
        win->node_pool[i].draw_cmd_idx = -1;
}

void ca_node_system_shutdown(Ca_Window *win)
{
    /* Free dynamic text buffers before releasing the label pool */
    for (uint32_t i = 0; i < CA_MAX_LABELS_PER_WINDOW; ++i)
        CA_FREE(win->label_pool[i].dyn_text);

    /* Free heap-allocated children arrays before releasing the node pool.
       ca_node_clear keeps arrays alive for reuse, so they are NOT freed
       by free_subtree and must be cleaned up here at shutdown. */
    if (win->node_pool) {
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
            if (win->node_pool[i].builder_effect)
                ca_effect_destroy(win->node_pool[i].builder_effect);
            CA_FREE(win->node_pool[i].children);
        }
    }
    CA_FREE(win->node_pool);
    CA_FREE(win->draw_cmds);
    CA_FREE(win->sorted_idx);
    CA_FREE(win->label_pool);
    CA_FREE(win->button_pool);
    CA_FREE(win->input_pool);
    CA_FREE(win->checkbox_pool);
    CA_FREE(win->radio_pool);
    CA_FREE(win->slider_pool);
    CA_FREE(win->toggle_pool);
    CA_FREE(win->progress_pool);
    CA_FREE(win->select_pool);
    CA_FREE(win->tabbar_pool);
    CA_FREE(win->treenode_pool);
    CA_FREE(win->table_pool);
    CA_FREE(win->tooltip_pool);
    CA_FREE(win->ctxmenu_pool);
    CA_FREE(win->modal_pool);
    CA_FREE(win->splitter_pool);
    CA_FREE(win->viewport_pool);
    CA_FREE(win->menubar_pool);
    CA_FREE(win->paint_cache);
    CA_FREE(win->layout_scratch);
    win->node_pool      = NULL;
    win->draw_cmds      = NULL;
    win->sorted_idx     = NULL;
    win->paint_cache    = NULL;
    win->layout_scratch = NULL;
    win->layout_scratch_capacity = 0;
    win->layout_scratch_used = 0;
    win->label_pool     = NULL;
    win->button_pool    = NULL;
    win->input_pool     = NULL;
    win->checkbox_pool  = NULL;
    win->radio_pool     = NULL;
    win->slider_pool    = NULL;
    win->toggle_pool    = NULL;
    win->progress_pool  = NULL;
    win->select_pool    = NULL;
    win->tabbar_pool    = NULL;
    win->treenode_pool  = NULL;
    win->table_pool     = NULL;
    win->tooltip_pool   = NULL;
    win->ctxmenu_pool   = NULL;
    win->modal_pool     = NULL;
    win->splitter_pool   = NULL;
    win->viewport_pool   = NULL;
    win->menubar_pool    = NULL;
    win->root           = NULL;
    win->draw_cmd_count = 0;
}

/* ---- Helpers ---- */

/* Grow a node's children array to fit at least `needed` entries.
   Starts at 8 and doubles, capped by CA_MAX_NODE_CHILDREN.
   Returns false on OOM. */
static bool node_grow_children(Ca_Node *parent, uint32_t needed)
{
    if (needed <= parent->child_capacity) return true;
    uint32_t cap = parent->child_capacity ? parent->child_capacity * 2u : 8u;
    while (cap < needed) cap *= 2u;
    if (cap > CA_MAX_NODE_CHILDREN) cap = CA_MAX_NODE_CHILDREN;
    if (needed > cap) return false; /* hard cap exceeded */
    Ca_Node **nc = (Ca_Node **)CA_REALLOC(parent->children, cap * sizeof(Ca_Node *));
    if (!nc) return false;
    parent->children = nc;
    parent->child_capacity = cap;
    return true;
}

static Ca_Node *alloc_node(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
        Ca_Node *n = &win->node_pool[i];
        if (!n->in_use) {
            memset(n, 0, sizeof(*n));
            /* children and child_capacity are zero-initialised by the
               memset; the array is allocated lazily in node_grow_children. */
            n->draw_cmd_idx = -1;
            return n;
        }
    }
    fprintf(stderr, "[causality] ca_node pool exhausted (max %d)\n", CA_MAX_NODES_PER_WINDOW);
    return NULL;
}

static void release_widget(Ca_Node *node)
{
    if (!node->widget) return;
    switch (node->widget_type) {
    case CA_WIDGET_LABEL: {
        Ca_Label *lbl = (Ca_Label *)node->widget;
        CA_FREE(lbl->dyn_text);
        lbl->dyn_text = NULL;
        lbl->in_use = false;
        break;
    }
    case CA_WIDGET_BUTTON:    ((Ca_Button *)node->widget)->in_use = false; break;
    case CA_WIDGET_TEXT_INPUT: ((Ca_TextInput *)node->widget)->in_use = false; break;
    case CA_WIDGET_CHECKBOX:  ((Ca_Checkbox *)node->widget)->in_use = false; break;
    case CA_WIDGET_RADIO:     ((Ca_Radio *)node->widget)->in_use = false; break;
    case CA_WIDGET_SLIDER:    ((Ca_Slider *)node->widget)->in_use = false; break;
    case CA_WIDGET_TOGGLE:    ((Ca_Toggle *)node->widget)->in_use = false; break;
    case CA_WIDGET_PROGRESS:  ((Ca_Progress *)node->widget)->in_use = false; break;
    case CA_WIDGET_SELECT:    ((Ca_Select *)node->widget)->in_use = false; break;
    case CA_WIDGET_TABBAR:    ((Ca_TabBar *)node->widget)->in_use = false; break;
    case CA_WIDGET_TREENODE:  ((Ca_TreeNode *)node->widget)->in_use = false; break;
    case CA_WIDGET_TABLE:     ((Ca_Table *)node->widget)->in_use = false; break;
    case CA_WIDGET_SPLITTER:  ((Ca_Splitter *)node->widget)->in_use = false; break;
    case CA_WIDGET_VIEWPORT:  ((Ca_Viewport *)node->widget)->in_use = false; break;
    case CA_WIDGET_MODAL:     ((Ca_Modal   *)node->widget)->in_use  = false; break;
    case CA_WIDGET_MENUBAR:   ((Ca_MenuBar *)node->widget)->in_use  = false; break;
    default: break;
    }
}

static void free_subtree(Ca_Node *node)
{
    if (!node) return;
    for (uint32_t i = 0; i < node->child_count; ++i)
        free_subtree(node->children[i]);
    if (node->builder_effect) {
        ca_effect_destroy(node->builder_effect);
        node->builder_effect = NULL;
    }
    if (node->scroll_y_signal) {
        ca_signal_destroy(node->scroll_y_signal);
        node->scroll_y_signal = NULL;
    }
    /* Clear any window-level pointers that reference this node, otherwise
       input handlers will dereference a freed slot next frame (UAF). */
    if (node->window) {
        Ca_Window *w = node->window;
        if (w->hovered_node        == node) w->hovered_node        = NULL;
        if (w->drag_node           == node) w->drag_node           = NULL;
        if (w->user_drag_node      == node) w->user_drag_node      = NULL;
        if (w->scrollbar_drag_node == node) w->scrollbar_drag_node = NULL;

        /* Tooltips and context menus attach to an *external* node (e.g. a
           tree-node header row) by raw pointer, tracked in a separate pool
           rather than owned 1:1 like other widgets — release_widget() below
           can't reach them. Left unreleased, a freed/reused node slot
           (e.g. after a reconcile recycles it for an unrelated widget in a
           different panel) keeps matching win->hovered_node by pointer
           equality, so a stale tooltip/menu bound to the old owner renders
           over whatever now occupies that memory. */
        if (w->tooltip_pool) {
            for (uint32_t i = 0; i < CA_MAX_TOOLTIPS_PER_WINDOW; ++i) {
                Ca_Tooltip *tt = &w->tooltip_pool[i];
                if (tt->in_use && tt->node == node) {
                    tt->in_use = false;
                    tt->node   = NULL;
                }
            }
        }
        if (w->ctxmenu_pool) {
            for (uint32_t i = 0; i < CA_MAX_CTXMENUS_PER_WINDOW; ++i) {
                Ca_CtxMenu *cm = &w->ctxmenu_pool[i];
                if (cm->in_use && cm->node == node) {
                    cm->in_use = false;
                    cm->node   = NULL;
                }
            }
        }
    }
    release_widget(node);
    /* Free the heap-allocated children pointer array (the child nodes
       themselves were already freed by the recursive calls above). */
    CA_FREE(node->children);
    memset(node, 0, sizeof(*node));
    node->draw_cmd_idx = -1;
}

bool layout_desc_changed(const Ca_NodeDesc *a, const Ca_NodeDesc *b)
{
    return a->width          != b->width          ||
           a->height         != b->height         ||
           a->min_w          != b->min_w          ||
           a->min_h          != b->min_h          ||
           a->max_w          != b->max_w          ||
           a->max_h          != b->max_h          ||
           a->padding_top    != b->padding_top    ||
           a->padding_right  != b->padding_right  ||
           a->padding_bottom != b->padding_bottom ||
           a->padding_left   != b->padding_left   ||
           a->margin_top     != b->margin_top     ||
           a->margin_right   != b->margin_right   ||
           a->margin_bottom  != b->margin_bottom  ||
           a->margin_left    != b->margin_left    ||
           a->gap            != b->gap            ||
           a->direction      != b->direction      ||
           a->align_items    != b->align_items    ||
           a->justify_content!= b->justify_content||
           a->flex_grow      != b->flex_grow      ||
           a->flex_shrink    != b->flex_shrink    ||
           a->flex_wrap      != b->flex_wrap      ||
           a->overflow_x     != b->overflow_x     ||
           a->overflow_y     != b->overflow_y     ||
           a->scrollbar_width != b->scrollbar_width ||
           a->scrollbar_width_set != b->scrollbar_width_set ||
           a->hidden         != b->hidden         ||
           a->position       != b->position       ||
           a->pos_x          != b->pos_x          ||
           a->pos_y          != b->pos_y          ||
           a->border_width   != b->border_width   ||
           a->text_wrap      != b->text_wrap      ||
           a->width_pct      != b->width_pct      ||
           a->height_pct     != b->height_pct     ||
           a->font_size      != b->font_size      ||
           a->font_bold      != b->font_bold;
}

bool content_desc_changed(const Ca_NodeDesc *a, const Ca_NodeDesc *b)
{
    return a->background      != b->background      ||
           a->corner_radius   != b->corner_radius   ||
           a->opacity         != b->opacity         ||
           a->font_size       != b->font_size       ||
           a->font_bold       != b->font_bold       ||
           a->text_align      != b->text_align      ||
           a->disabled        != b->disabled        ||
           a->hidden          != b->hidden          ||
           a->overflow_x      != b->overflow_x      ||
           a->overflow_y      != b->overflow_y      ||
           a->scrollbar_width != b->scrollbar_width ||
           a->scrollbar_track_color != b->scrollbar_track_color ||
           a->scrollbar_thumb_color != b->scrollbar_thumb_color ||
           a->scrollbar_thumb_active_color != b->scrollbar_thumb_active_color ||
           a->scrollbar_radius != b->scrollbar_radius ||
           a->scrollbar_width_set != b->scrollbar_width_set ||
           a->scrollbar_track_color_set != b->scrollbar_track_color_set ||
           a->scrollbar_thumb_color_set != b->scrollbar_thumb_color_set ||
           a->scrollbar_thumb_active_color_set != b->scrollbar_thumb_active_color_set ||
           a->scrollbar_radius_set != b->scrollbar_radius_set ||
           a->border_color    != b->border_color    ||
           a->border_width    != b->border_width    ||
           a->shadow_offset_x != b->shadow_offset_x ||
           a->shadow_offset_y != b->shadow_offset_y ||
           a->shadow_blur     != b->shadow_blur     ||
           a->shadow_color    != b->shadow_color    ||
           a->z_index         != b->z_index         ||
           a->text_wrap       != b->text_wrap;
}

/* ---- Propagation ---- */

void ca_node_propagate_layout(Ca_Window *win)
{
    /* Bubble DIRTY_LAYOUT upward so the root will always be layout-dirty
       when any descendant needs re-layout. Iterate until stable. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
            Ca_Node *n = &win->node_pool[i];
            if (!n->in_use || !n->parent) continue;
            if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN)) {
                if (!(n->parent->dirty & CA_DIRTY_LAYOUT)) {
                    n->parent->dirty |= CA_DIRTY_LAYOUT;
                    changed = true;
                }
            }
        }
    }
}

/* ---- Public API ---- */

Ca_Node *ca_node_root(Ca_Window *window)
{
    assert(window);
    if (window->root) return window->root;

    Ca_Node *n = alloc_node(window);
    if (!n) return NULL;

    n->window = window;
    n->in_use = true;
    n->dirty  = CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
    window->root = n;
    return n;
}

Ca_Node *ca_node_add(Ca_Node *parent, const Ca_NodeDesc *desc)
{
    assert(parent && parent->in_use && desc);

    if (parent->child_count >= CA_MAX_NODE_CHILDREN) {
        fprintf(stderr, "[causality] ca_node_add: child limit reached (%d)\n", CA_MAX_NODE_CHILDREN);
        return NULL;
    }

    if (!node_grow_children(parent, parent->child_count + 1u)) {
        fprintf(stderr, "[causality] ca_node_add: OOM growing children\n");
        return NULL;
    }

    Ca_Node *n = alloc_node(parent->window);
    if (!n) return NULL;

    n->window = parent->window;
    n->parent = parent;
    n->desc   = *desc;
    n->in_use = true;
    n->dirty  = CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;

    parent->children[parent->child_count++] = n;
    parent->dirty |= CA_DIRTY_CHILDREN | CA_DIRTY_LAYOUT;
    return n;
}

void ca_node_remove(Ca_Node *node)
{
    if (!node || !node->in_use) return;

    if (node->parent) {
        Ca_Node *p = node->parent;
        for (uint32_t i = 0; i < p->child_count; ++i) {
            if (p->children[i] == node) {
                /* Swap-remove */
                p->children[i] = p->children[--p->child_count];
                break;
            }
        }
        p->dirty |= CA_DIRTY_CHILDREN | CA_DIRTY_LAYOUT;
    } else if (node->window) {
        node->window->root = NULL;
    }

    free_subtree(node);
}

void ca_node_clear(Ca_Node *node)
{
    if (!node || !node->in_use) return;
    for (uint32_t i = 0; i < node->child_count; ++i)
        free_subtree(node->children[i]);
    node->child_count = 0;
    node->dirty |= CA_DIRTY_CHILDREN | CA_DIRTY_LAYOUT;
}

void ca_node_trim_children(Ca_Node *parent, uint32_t keep_count)
{
    if (!parent || !parent->in_use) return;
    if (keep_count >= parent->child_count) return;

    for (uint32_t i = keep_count; i < parent->child_count; ++i)
        free_subtree(parent->children[i]);

    parent->child_count = keep_count;
    parent->dirty |= CA_DIRTY_CHILDREN | CA_DIRTY_LAYOUT;
}

void ca_node_set_desc(Ca_Node *node, const Ca_NodeDesc *desc)
{
    assert(node && node->in_use && desc);

    bool layout  = layout_desc_changed(&node->desc, desc);
    bool content = content_desc_changed(&node->desc, desc);
    node->desc = *desc;
    if (content) node->dirty |= CA_DIRTY_CONTENT;
    if (layout)  node->dirty |= CA_DIRTY_LAYOUT;
}
