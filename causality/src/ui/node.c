/* node.c — node pool, tree building, and subscription wiring */
#include "node.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

void ca_node_system_init(Ca_Window *win)
{
    win->node_pool      = (Ca_Node *)calloc(CA_MAX_NODES_PER_WINDOW, sizeof(Ca_Node));
    win->draw_cmds      = (Ca_DrawCmd *)calloc(CA_MAX_DRAW_CMDS_PER_WINDOW, sizeof(Ca_DrawCmd));
    win->label_pool     = (Ca_Label *)calloc(CA_MAX_LABELS_PER_WINDOW,  sizeof(Ca_Label));
    win->button_pool    = (Ca_Button *)calloc(CA_MAX_BUTTONS_PER_WINDOW, sizeof(Ca_Button));
    win->input_pool     = (Ca_TextInput *)calloc(CA_MAX_INPUTS_PER_WINDOW, sizeof(Ca_TextInput));
    win->checkbox_pool  = (Ca_Checkbox *)calloc(CA_MAX_CHECKBOXES_PER_WINDOW, sizeof(Ca_Checkbox));
    win->radio_pool     = (Ca_Radio *)calloc(CA_MAX_RADIOS_PER_WINDOW, sizeof(Ca_Radio));
    win->slider_pool    = (Ca_Slider *)calloc(CA_MAX_SLIDERS_PER_WINDOW, sizeof(Ca_Slider));
    win->toggle_pool    = (Ca_Toggle *)calloc(CA_MAX_TOGGLES_PER_WINDOW, sizeof(Ca_Toggle));
    win->progress_pool  = (Ca_Progress *)calloc(CA_MAX_PROGRESS_PER_WINDOW, sizeof(Ca_Progress));
    win->select_pool    = (Ca_Select *)calloc(CA_MAX_SELECTS_PER_WINDOW, sizeof(Ca_Select));
    win->tabbar_pool    = (Ca_TabBar *)calloc(CA_MAX_TABBARS_PER_WINDOW, sizeof(Ca_TabBar));
    win->treenode_pool  = (Ca_TreeNode *)calloc(CA_MAX_TREENODES_PER_WINDOW, sizeof(Ca_TreeNode));
    win->table_pool     = (Ca_Table *)calloc(CA_MAX_TABLES_PER_WINDOW, sizeof(Ca_Table));
    win->tooltip_pool   = (Ca_Tooltip *)calloc(CA_MAX_TOOLTIPS_PER_WINDOW, sizeof(Ca_Tooltip));
    win->ctxmenu_pool   = (Ca_CtxMenu *)calloc(CA_MAX_CTXMENUS_PER_WINDOW, sizeof(Ca_CtxMenu));
    win->modal_pool     = (Ca_Modal *)calloc(CA_MAX_MODALS_PER_WINDOW, sizeof(Ca_Modal));
    win->root           = NULL;
    win->draw_cmd_count = 0;
    win->paint_cache     = (Ca_DrawCmd *)calloc(CA_MAX_DRAW_CMDS_PER_WINDOW, sizeof(Ca_DrawCmd));
    win->paint_cache_used = 0;
    win->hovered_node   = NULL;
    win->drag_node      = NULL;

    /* Pre-set all draw_cmd_idx to -1 (0 is a valid slot index) */
    for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i)
        win->node_pool[i].draw_cmd_idx = -1;
}

void ca_node_system_shutdown(Ca_Window *win)
{
    free(win->node_pool);
    free(win->draw_cmds);
    free(win->label_pool);
    free(win->button_pool);
    free(win->input_pool);
    free(win->checkbox_pool);
    free(win->radio_pool);
    free(win->slider_pool);
    free(win->toggle_pool);
    free(win->progress_pool);
    free(win->select_pool);
    free(win->tabbar_pool);
    free(win->treenode_pool);
    free(win->table_pool);
    free(win->tooltip_pool);
    free(win->ctxmenu_pool);
    free(win->modal_pool);
    free(win->paint_cache);
    win->node_pool      = NULL;
    win->draw_cmds      = NULL;
    win->paint_cache    = NULL;
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
    win->root           = NULL;
    win->draw_cmd_count = 0;
}

/* ---- Helpers ---- */

static Ca_Node *alloc_node(Ca_Window *win)
{
    for (uint32_t i = 0; i < CA_MAX_NODES_PER_WINDOW; ++i) {
        Ca_Node *n = &win->node_pool[i];
        if (!n->in_use) {
            memset(n, 0, sizeof(*n));
            n->draw_cmd_idx = -1;
            return n;
        }
    }
    fprintf(stderr, "[causality] ca_node pool exhausted (max %d)\n", CA_MAX_NODES_PER_WINDOW);
    return NULL;
}

static void free_subtree(Ca_Node *node)
{
    if (!node) return;
    for (uint32_t i = 0; i < node->child_count; ++i)
        free_subtree(node->children[i]);
    memset(node, 0, sizeof(*node));
    node->draw_cmd_idx = -1;
}

static bool layout_desc_changed(const Ca_NodeDesc *a, const Ca_NodeDesc *b)
{
    return a->width         != b->width          ||
           a->height        != b->height         ||
           a->min_w         != b->min_w          ||
           a->min_h         != b->min_h          ||
           a->max_w         != b->max_w          ||
           a->max_h         != b->max_h          ||
           a->padding_top   != b->padding_top    ||
           a->padding_right != b->padding_right  ||
           a->padding_bottom!= b->padding_bottom ||
           a->padding_left  != b->padding_left   ||
           a->gap           != b->gap            ||
           a->direction     != b->direction;
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

void ca_node_set_desc(Ca_Node *node, const Ca_NodeDesc *desc)
{
    assert(node && node->in_use && desc);

    bool layout = layout_desc_changed(&node->desc, desc);
    node->desc   = *desc;
    node->dirty |= CA_DIRTY_CONTENT;
    if (layout) node->dirty |= CA_DIRTY_LAYOUT;
}

void ca_node_subscribe(Ca_Node *node, Ca_State *state, Ca_DirtyFlags on_change)
{
    assert(node && node->in_use && state && state->in_use);

    if (node->sub_count >= CA_MAX_NODE_SUBS) {
        fprintf(stderr, "[causality] ca_node_subscribe: node sub limit (%d)\n", CA_MAX_NODE_SUBS);
        return;
    }
    if (state->sub_count >= CA_MAX_STATE_SUBSCRIBERS) {
        fprintf(stderr, "[causality] ca_node_subscribe: state sub limit (%d)\n", CA_MAX_STATE_SUBSCRIBERS);
        return;
    }

    node->subs[node->sub_count]      = state;
    node->sub_flags[node->sub_count] = (uint8_t)on_change;
    node->sub_count++;

    state->subscribers[state->sub_count] = node;
    state->sub_flags[state->sub_count]   = (uint8_t)on_change;
    state->sub_count++;
}
