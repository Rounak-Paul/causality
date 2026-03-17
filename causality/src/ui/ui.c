/* ui.c — wires state, node, layout, and paint passes together */
#include "ui.h"
#include "state.h"
#include "node.h"
#include "layout.h"
#include "paint.h"
#include "widget.h"

void ca_ui_init(Ca_Instance *inst)
{
    ca_state_system_init(inst);
}

void ca_ui_shutdown(Ca_Instance *inst)
{
    ca_state_system_shutdown(inst);
}

void ca_ui_window_init(Ca_Window *win)
{
    ca_node_system_init(win);
}

void ca_ui_window_shutdown(Ca_Window *win)
{
    ca_node_system_shutdown(win);
}

void ca_ui_update(Ca_Instance *inst)
{
    /* 1. Propagate state dirty flags to subscriber nodes */
    ca_state_flush_dirty(inst);

    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        Ca_Window *win = &inst->windows[i];
        if (!win->in_use || !win->root || !win->node_pool) continue;

        /* 2. Check what kind of work this window needs */
        bool any_layout  = false;
        bool any_content = false;

        for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
            Ca_Node *n = &win->node_pool[j];
            if (!n->in_use) continue;
            if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN)) any_layout  = true;
            if (n->dirty & CA_DIRTY_CONTENT)                       any_content = true;
        }

        /* 3. Layout pass — propagate upward then recompute rects */
        if (any_layout) {
            ca_node_propagate_layout(win);
            ca_layout_pass(win);

            /* After re-layout all positions changed, so all nodes need repaint */
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (n->in_use) n->dirty |= CA_DIRTY_CONTENT;
            }
            any_content = true;
        }

        /* 4. Paint pass — full draw-list rebuild */
        if (any_content) {
            /* Reset slots and mark every node dirty so paint_node rebuilds
               the ENTIRE draw list (not just the changed nodes).  Otherwise
               clean siblings would be missing from the draw commands after a
               partial update (e.g. one button's colour changed). */
            win->draw_cmd_count = 0;
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (n->in_use) {
                    n->draw_cmd_idx = -1;
                    n->dirty |= CA_DIRTY_CONTENT;
                }
            }
            ca_paint_pass(inst, win);
            win->needs_render = true;
        }

        /* 5. Input pass — hit-test buttons and fire click callbacks */
        ca_widget_input_pass(win);
    }
}
