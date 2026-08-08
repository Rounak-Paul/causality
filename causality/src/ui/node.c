// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* node.c — node pool, tree building, and subscription wiring */
#include "node.h"
#include "menu_storage.h"
#include "viewport.h"

#include <string.h>
#include <assert.h>

/** Initializes pointer-stable, demand-grown UI object pools for a window. */
bool ca_node_system_init(Ca_Window *win)
{
    win->draw_cmd_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_DrawCmd);
    win->sorted_index_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(uint32_t);
    win->paint_cache_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_DrawCmd);
    win->layout_scratch_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(float);
    win->char_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(uint32_t);
    win->key_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(int);
    win->key_action_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(int);
    win->key_mods_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(int);
    win->draw_cmds = NULL;
    win->sorted_idx = NULL;
    win->paint_cache = NULL;
    win->layout_scratch = NULL;
    win->char_buf = NULL;
    win->key_buf = NULL;
    win->key_action_buf = NULL;
    win->key_mods_buf = NULL;
    bool pools_ready =
        ca_pool_init(&win->node_pool, sizeof(Ca_Node),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Node))) &&
        ca_pool_init(&win->label_pool, sizeof(Ca_Label),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Label))) &&
        ca_pool_init(&win->button_pool, sizeof(Ca_Button),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Button))) &&
        ca_pool_init(&win->input_pool, sizeof(Ca_TextInput),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_TextInput))) &&
        ca_pool_init(&win->checkbox_pool, sizeof(Ca_Checkbox),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Checkbox))) &&
        ca_pool_init(&win->radio_pool, sizeof(Ca_Radio),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Radio))) &&
        ca_pool_init(&win->slider_pool, sizeof(Ca_Slider),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Slider))) &&
        ca_pool_init(&win->toggle_pool, sizeof(Ca_Toggle),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Toggle))) &&
        ca_pool_init(&win->progress_pool, sizeof(Ca_Progress),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Progress))) &&
        ca_pool_init(&win->select_pool, sizeof(Ca_Select),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Select))) &&
        ca_pool_init(&win->tabbar_pool, sizeof(Ca_TabBar),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_TabBar))) &&
        ca_pool_init(&win->treenode_pool, sizeof(Ca_TreeNode),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_TreeNode))) &&
        ca_pool_init(&win->table_pool, sizeof(Ca_Table),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Table))) &&
        ca_pool_init(&win->tooltip_pool, sizeof(Ca_Tooltip),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Tooltip))) &&
        ca_pool_init(&win->ctxmenu_pool, sizeof(Ca_CtxMenu),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_CtxMenu))) &&
        ca_pool_init(&win->modal_pool, sizeof(Ca_Modal),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Modal))) &&
        ca_pool_init(&win->splitter_pool, sizeof(Ca_Splitter),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Splitter))) &&
        ca_pool_init(&win->viewport_pool, sizeof(Ca_Viewport),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Viewport))) &&
        ca_pool_init(&win->menubar_pool, sizeof(Ca_MenuBar),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_MenuBar)));
    if (!pools_ready) {
        ca_node_system_shutdown(win);
        return false;
    }
    win->root           = NULL;
    win->draw_cmd_count = 0;
    win->paint_cache_used = 0;
    win->layout_scratch_capacity = 0;
    win->layout_scratch_used = 0;
    win->hovered_node   = NULL;
    win->drag_node      = NULL;

    return true;
}

/** Releases all demand-grown UI storage owned by a window. */
void ca_node_system_shutdown(Ca_Window *win)
{
    for (size_t i = 0; i < ca_pool_slot_count(&win->label_pool); ++i) {
        Ca_Label *label = CA_POOL_AT(win->label_pool, Ca_Label, i);
        CA_FREE(label->dyn_text);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&win->node_pool); ++i) {
        Ca_Node *node = CA_POOL_AT(win->node_pool, Ca_Node, i);
        if (node->builder_effect) ca_effect_destroy(node->builder_effect);
        ca_css_destroy(node->scoped_stylesheet);
        ca_dyn_array_destroy(&node->transition_storage);
        CA_FREE(node->children);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&win->select_pool); ++i) {
        Ca_Select *select = CA_POOL_AT(win->select_pool, Ca_Select, i);
        ca_dyn_array_destroy(&select->option_storage);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&win->tabbar_pool); ++i) {
        Ca_TabBar *tab_bar = CA_POOL_AT(win->tabbar_pool, Ca_TabBar, i);
        ca_dyn_array_destroy(&tab_bar->label_storage);
        ca_dyn_array_destroy(&tab_bar->tab_node_storage);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&win->table_pool); ++i) {
        Ca_Table *table = CA_POOL_AT(win->table_pool, Ca_Table, i);
        ca_dyn_array_destroy(&table->column_width_storage);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&win->ctxmenu_pool); ++i) {
        Ca_CtxMenu *menu = CA_POOL_AT(win->ctxmenu_pool, Ca_CtxMenu, i);
        ca_dyn_array_destroy(&menu->item_storage);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&win->menubar_pool); ++i) {
        Ca_MenuBar *menu_bar = CA_POOL_AT(win->menubar_pool, Ca_MenuBar, i);
        ca_menu_storage_destroy(&menu_bar->menu_storage, &menu_bar->menus);
    }
    ca_pool_destroy(&win->node_pool, NULL, NULL);
    ca_dyn_array_destroy(&win->draw_cmd_storage);
    ca_dyn_array_destroy(&win->sorted_index_storage);
    ca_pool_destroy(&win->label_pool, NULL, NULL);
    ca_pool_destroy(&win->button_pool, NULL, NULL);
    ca_pool_destroy(&win->input_pool, NULL, NULL);
    ca_pool_destroy(&win->checkbox_pool, NULL, NULL);
    ca_pool_destroy(&win->radio_pool, NULL, NULL);
    ca_pool_destroy(&win->slider_pool, NULL, NULL);
    ca_pool_destroy(&win->toggle_pool, NULL, NULL);
    ca_pool_destroy(&win->progress_pool, NULL, NULL);
    ca_pool_destroy(&win->select_pool, NULL, NULL);
    ca_pool_destroy(&win->tabbar_pool, NULL, NULL);
    ca_pool_destroy(&win->treenode_pool, NULL, NULL);
    ca_pool_destroy(&win->table_pool, NULL, NULL);
    ca_pool_destroy(&win->tooltip_pool, NULL, NULL);
    ca_pool_destroy(&win->ctxmenu_pool, NULL, NULL);
    ca_pool_destroy(&win->modal_pool, NULL, NULL);
    ca_pool_destroy(&win->splitter_pool, NULL, NULL);
    ca_pool_destroy(&win->viewport_pool, NULL, NULL);
    ca_pool_destroy(&win->menubar_pool, NULL, NULL);
    ca_dyn_array_destroy(&win->paint_cache_storage);
    ca_dyn_array_destroy(&win->layout_scratch_storage);
    ca_dyn_array_destroy(&win->paint_cache_spans);
    ca_dyn_array_destroy(&win->char_storage);
    ca_dyn_array_destroy(&win->key_storage);
    ca_dyn_array_destroy(&win->key_action_storage);
    ca_dyn_array_destroy(&win->key_mods_storage);
    win->draw_cmds      = NULL;
    win->sorted_idx     = NULL;
    win->paint_cache    = NULL;
    win->layout_scratch = NULL;
    win->char_buf = NULL;
    win->key_buf = NULL;
    win->key_action_buf = NULL;
    win->key_mods_buf = NULL;
    win->layout_scratch_capacity = 0;
    win->layout_scratch_used = 0;
    win->root           = NULL;
    win->draw_cmd_count = 0;
}

bool ca_window_reserve_draw_commands(Ca_Window *win, size_t minimum)
{
    if (!win || minimum > UINT32_MAX ||
        !ca_dyn_array_reserve(&win->draw_cmd_storage, minimum))
        return false;
    win->draw_cmds = win->draw_cmd_storage.data;
    return true;
}

bool ca_window_reserve_sorted_indices(Ca_Window *win, size_t minimum)
{
    if (!win || minimum > UINT32_MAX ||
        !ca_dyn_array_reserve(&win->sorted_index_storage, minimum))
        return false;
    win->sorted_idx = win->sorted_index_storage.data;
    return true;
}

bool ca_window_reserve_paint_cache(Ca_Window *win, size_t minimum)
{
    if (!win || minimum > UINT32_MAX ||
        !ca_dyn_array_reserve(&win->paint_cache_storage, minimum))
        return false;
    win->paint_cache = win->paint_cache_storage.data;
    return true;
}

/* ---- Helpers ---- */

/** Grows a node's child pointer array without an application-size limit. */
static bool node_grow_children(Ca_Node *parent, uint32_t needed)
{
    if (needed <= parent->child_capacity) return true;
    uint32_t cap = parent->child_capacity ? parent->child_capacity : 8u;
    while (cap < needed) {
        if (cap > UINT32_MAX / 2u) {
            cap = needed;
            break;
        }
        cap *= 2u;
    }
    if ((size_t)cap > SIZE_MAX / sizeof(Ca_Node *)) return false;
    Ca_Node **nc = CA_REALLOC(parent->children,
                              (size_t)cap * sizeof(Ca_Node *));
    if (!nc) return false;
    parent->children = nc;
    parent->child_capacity = cap;
    return true;
}

static Ca_Node *alloc_node(Ca_Window *win)
{
    Ca_Node *node = ca_pool_acquire(&win->node_pool);
    if (!node) return NULL;
    node->draw_cmd_idx = -1;
    return node;
}

static void release_widget(Ca_Node *node)
{
    if (!node->widget) return;
    Ca_Window *window = node->window;
    switch (node->widget_type) {
    case CA_WIDGET_LABEL: {
        Ca_Label *lbl = (Ca_Label *)node->widget;
        CA_FREE(lbl->dyn_text);
        lbl->dyn_text = NULL;
        ca_pool_release(&window->label_pool, lbl);
        break;
    }
    case CA_WIDGET_BUTTON: ca_pool_release(&window->button_pool, node->widget); break;
    case CA_WIDGET_TEXT_INPUT: ca_pool_release(&window->input_pool, node->widget); break;
    case CA_WIDGET_CHECKBOX: ca_pool_release(&window->checkbox_pool, node->widget); break;
    case CA_WIDGET_RADIO: ca_pool_release(&window->radio_pool, node->widget); break;
    case CA_WIDGET_SLIDER: ca_pool_release(&window->slider_pool, node->widget); break;
    case CA_WIDGET_TOGGLE: ca_pool_release(&window->toggle_pool, node->widget); break;
    case CA_WIDGET_PROGRESS: ca_pool_release(&window->progress_pool, node->widget); break;
    case CA_WIDGET_SELECT: {
        Ca_Select *select = node->widget;
        ca_dyn_array_destroy(&select->option_storage);
        ca_pool_release(&window->select_pool, select);
        break;
    }
    case CA_WIDGET_TABBAR: {
        Ca_TabBar *tab_bar = node->widget;
        ca_dyn_array_destroy(&tab_bar->label_storage);
        ca_dyn_array_destroy(&tab_bar->tab_node_storage);
        ca_pool_release(&window->tabbar_pool, tab_bar);
        break;
    }
    case CA_WIDGET_TREENODE: ca_pool_release(&window->treenode_pool, node->widget); break;
    case CA_WIDGET_TABLE: {
        Ca_Table *table = node->widget;
        ca_dyn_array_destroy(&table->column_width_storage);
        ca_pool_release(&window->table_pool, table);
        break;
    }
    case CA_WIDGET_SPLITTER: ca_pool_release(&window->splitter_pool, node->widget); break;
    case CA_WIDGET_VIEWPORT: {
        Ca_Viewport *vp = node->widget;
        /* GPU resources (images, views, descriptor sets) are a separate
           lifetime from the pool slot — must be torn down explicitly or
           they leak every time a viewport node is removed (panel close,
           reconciled subtree drop), not just on full window/app teardown. */
        ca_viewport_gpu_destroy(vp->instance, vp);
        ca_pool_release(&window->viewport_pool, vp);
        break;
    }
    case CA_WIDGET_MODAL: ca_pool_release(&window->modal_pool, node->widget); break;
    case CA_WIDGET_MENUBAR: {
        Ca_MenuBar *menu_bar = node->widget;
        ca_menu_storage_destroy(&menu_bar->menu_storage, &menu_bar->menus);
        ca_pool_release(&window->menubar_pool, menu_bar);
        break;
    }
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
    ca_dyn_array_destroy(&node->transition_storage);
    ca_css_destroy(node->scoped_stylesheet);
    node->scoped_stylesheet = NULL;
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
        {
            for (size_t i = 0; i < ca_pool_slot_count(&w->tooltip_pool); ++i) {
                Ca_Tooltip *tt = CA_POOL_AT(w->tooltip_pool, Ca_Tooltip, i);
                if (tt->in_use && tt->node == node) {
                    ca_pool_release(&w->tooltip_pool, tt);
                }
            }
        }
        {
            for (size_t i = 0; i < ca_pool_slot_count(&w->ctxmenu_pool); ++i) {
                Ca_CtxMenu *cm = CA_POOL_AT(w->ctxmenu_pool, Ca_CtxMenu, i);
                if (cm->in_use && cm->node == node) {
                    ca_dyn_array_destroy(&cm->item_storage);
                    ca_pool_release(&w->ctxmenu_pool, cm);
                }
            }
        }
    }
    release_widget(node);
    /* Free the heap-allocated children pointer array (the child nodes
       themselves were already freed by the recursive calls above). */
    CA_FREE(node->children);
    if (node->window) ca_pool_release(&node->window->node_pool, node);
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
           a->pos_right      != b->pos_right      ||
           a->pos_bottom     != b->pos_bottom     ||
           a->position_offsets != b->position_offsets ||
           a->position_percent != b->position_percent ||
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
        for (size_t i = 0; i < ca_pool_slot_count(&win->node_pool); ++i) {
            Ca_Node *n = CA_POOL_AT(win->node_pool, Ca_Node, i);
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
                /* Ordered removal — matches ca_node_trim_children's ordered
                   truncation and the sibling-order invariant claim_child's
                   reconcile logic depends on. A swap-remove here would
                   silently reorder later siblings for any future caller
                   that removes a node from a reconciled subtree. */
                memmove(&p->children[i], &p->children[i + 1],
                       (size_t)(p->child_count - i - 1) * sizeof(Ca_Node *));
                --p->child_count;
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
