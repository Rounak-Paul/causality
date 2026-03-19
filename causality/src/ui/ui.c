/* ui.c — wires state, node, layout, and paint passes together */
#include "ui.h"
#include "state.h"
#include "node.h"
#include "layout.h"
#include "paint.h"
#include "widget.h"
#include "css.h"

#include <GLFW/glfw3.h>

/* Linear interpolation */
static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* Ease-in-out cubic */
static float ease_in_out(float t)
{
    if (t < 0.5f) return 4.0f * t * t * t;
    float f = 2.0f * t - 2.0f;
    return 0.5f * f * f * f + 1.0f;
}

/* Interpolate packed RGBA colors */
static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    float ra = (float)((a >> 24) & 0xFF);
    float ga = (float)((a >> 16) & 0xFF);
    float ba = (float)((a >>  8) & 0xFF);
    float aa = (float)((a)       & 0xFF);
    float rb = (float)((b >> 24) & 0xFF);
    float gb = (float)((b >> 16) & 0xFF);
    float bb = (float)((b >>  8) & 0xFF);
    float ab = (float)((b)       & 0xFF);
    uint32_t r = (uint32_t)lerpf(ra, rb, t);
    uint32_t g = (uint32_t)lerpf(ga, gb, t);
    uint32_t bi = (uint32_t)lerpf(ba, bb, t);
    uint32_t ai = (uint32_t)lerpf(aa, ab, t);
    return (r << 24) | (g << 16) | (bi << 8) | ai;
}

/* Start or update a transition on a node for a given property */
static void transition_start(Ca_Node *node, uint8_t prop,
                             float from_f, float to_f,
                             uint32_t from_color, uint32_t to_color)
{
    /* Find existing transition for this prop, or an inactive slot */
    Ca_Transition *slot = NULL;
    for (int i = 0; i < CA_MAX_TRANSITIONS_PER_NODE; ++i) {
        if (node->transitions[i].active && node->transitions[i].prop == prop) {
            slot = &node->transitions[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < CA_MAX_TRANSITIONS_PER_NODE; ++i) {
            if (!node->transitions[i].active) {
                slot = &node->transitions[i];
                break;
            }
        }
    }
    if (!slot) return; /* all slots busy */

    slot->prop       = prop;
    slot->active     = true;
    slot->from_f     = from_f;
    slot->to_f       = to_f;
    slot->from_color = from_color;
    slot->to_color   = to_color;
    slot->start_time = glfwGetTime();
    slot->duration   = node->transition_duration;
}

/* Tick all active transitions on a node. Returns true if any are still active. */
static bool transition_tick(Ca_Node *node, double now)
{
    bool any_active = false;
    for (int i = 0; i < CA_MAX_TRANSITIONS_PER_NODE; ++i) {
        Ca_Transition *tr = &node->transitions[i];
        if (!tr->active) continue;

        float elapsed = (float)(now - tr->start_time);
        float t = (tr->duration > 0.0f) ? elapsed / tr->duration : 1.0f;
        if (t >= 1.0f) {
            t = 1.0f;
            tr->active = false;
        } else {
            any_active = true;
        }

        float eased = ease_in_out(t);

        /* Apply interpolated value to the node */
        switch ((Ca_CssPropId)tr->prop) {
            case CA_CSS_PROP_BACKGROUND_COLOR:
                node->desc.background = lerp_color(tr->from_color, tr->to_color, eased);
                break;
            case CA_CSS_PROP_WIDTH:
                node->desc.width = lerpf(tr->from_f, tr->to_f, eased);
                break;
            case CA_CSS_PROP_HEIGHT:
                node->desc.height = lerpf(tr->from_f, tr->to_f, eased);
                break;
            case CA_CSS_PROP_BORDER_RADIUS:
                node->desc.corner_radius = lerpf(tr->from_f, tr->to_f, eased);
                break;
            case CA_CSS_PROP_OPACITY:
                /* opacity modulates the alpha channel of background */
                break;
            default: break;
        }
    }
    return any_active;
}

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

        /* 2. Transition tick — update animated properties, mark dirty */
        double now = glfwGetTime();
        bool any_transitions = false;
        for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
            Ca_Node *n = &win->node_pool[j];
            if (!n->in_use) continue;
            if (transition_tick(n, now)) {
                any_transitions = true;
                n->dirty |= CA_DIRTY_CONTENT;
            }
        }

        /* 3. Check what kind of work this window needs */
        bool any_layout  = false;
        bool any_content = false;

        for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
            Ca_Node *n = &win->node_pool[j];
            if (!n->in_use) continue;
            if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN)) any_layout  = true;
            if (n->dirty & CA_DIRTY_CONTENT)                       any_content = true;
        }

        /* 4. Layout pass — propagate upward then recompute rects */
        if (any_layout) {
            ca_node_propagate_layout(win);
            ca_layout_pass(win);

            /* After re-layout all positions changed — reset paint cache and
               mark every node dirty so the cache is rebuilt from scratch. */
            win->paint_cache_used = 0;
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (n->in_use) {
                    n->dirty |= CA_DIRTY_CONTENT;
                    n->cache_count      = 0;
                    n->cache_post_count = 0;
                }
            }
            any_content = true;
        }

        /* 5. Input pass — hit-test buttons and fire click callbacks.
              Run BEFORE paint so that input-driven dirty flags are
              picked up in the same frame's paint pass.  */
        Ca_Node *prev_hovered = win->hovered_node;
        Ca_Node *prev_focused = win->focused_node;
        ca_widget_input_pass(win);

        /* Mark hover and focus changes so repaint catches them */
        if (win->hovered_node != prev_hovered) {
            if (prev_hovered)       prev_hovered->dirty       |= CA_DIRTY_CONTENT;
            if (win->hovered_node)  win->hovered_node->dirty  |= CA_DIRTY_CONTENT;
        }
        if (win->focused_node != prev_focused) {
            if (prev_focused)       prev_focused->dirty       |= CA_DIRTY_CONTENT;
            if (win->focused_node)  win->focused_node->dirty  |= CA_DIRTY_CONTENT;
        }

        /* Re-scan for content dirty after input (widget state changes may
           have dirtied nodes that weren't dirty before). */
        if (!any_content) {
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (n->in_use && (n->dirty & CA_DIRTY_CONTENT)) {
                    any_content = true;
                    break;
                }
            }
        }

        /* 6. Incremental paint pass — only dirty nodes are repainted;
              clean nodes reuse cached draw commands.  */
        if (any_content) {
            ca_paint_pass(inst, win);
            win->needs_render = true;
        }

        /* 7. Request another tick for active transitions so we don't stall
              inside glfwWaitEvents() while animations are running.  */
        if (any_transitions)
            glfwPostEmptyEvent();
    }
}
