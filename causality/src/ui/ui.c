// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ui.c — wires reactive flush, node, layout, and paint passes together */
#include "ui.h"
#include "node.h"
#include "layout.h"
#include "paint.h"
#include "widget.h"
#include "css.h"
#include "title_bar.h"
#include "../platform/window.h"

#include <GLFW/glfw3.h>
#include <string.h>

/* Saved geometry for incremental layout invalidation. */
typedef struct { float x, y, w, h, cw, ch; } NodeRect;

/* Recursively rescale all dimension values in a node subtree by `ratio`.
   Applied once per frame at frame-start for deferred scale changes so that
   static nodes (built once, never reconciled) pick up the new ui_scale. */
static void rescale_nodes(Ca_Node *node, float ratio)
{
    if (!node || !node->in_use) return;
    Ca_NodeDesc *d = &node->desc;
    if (!d->width_pct  && d->width  > 0.0f) d->width  *= ratio;
    if (!d->height_pct && d->height > 0.0f) d->height *= ratio;
    if (d->min_w   > 0.0f) d->min_w   *= ratio;
    if (d->max_w   > 0.0f) d->max_w   *= ratio;
    if (d->min_h   > 0.0f) d->min_h   *= ratio;
    if (d->max_h   > 0.0f) d->max_h   *= ratio;
    d->padding_top    *= ratio;
    d->padding_right  *= ratio;
    d->padding_bottom *= ratio;
    d->padding_left   *= ratio;
    d->margin_top     *= ratio;
    d->margin_right   *= ratio;
    d->margin_bottom  *= ratio;
    d->margin_left    *= ratio;
    d->gap            *= ratio;
    d->corner_radius  *= ratio;
    /* font_size is NOT rescaled here: paint.c / layout.c already multiply
       node->desc.font_size by ui_scale for tier selection, so rescaling it
       here would double-count the scale factor and cause wrong tier selection
       and cursor misalignment for one frame until reactive builders re-run. */
    d->pos_x        *= ratio;
    d->pos_y        *= ratio;
    d->border_width *= ratio;
    d->shadow_blur  *= ratio;
    node->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
    for (uint32_t ci = 0; ci < node->child_count; ci++)
        rescale_nodes(node->children[ci], ratio);
}

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
    (void)inst;
}

void ca_ui_shutdown(Ca_Instance *inst)
{
    (void)inst;
}

void ca_ui_window_init(Ca_Window *win)
{
    ca_node_system_init(win);
    ca_title_bar_init(win);
}

void ca_ui_window_shutdown(Ca_Window *win)
{
    ca_node_system_shutdown(win);
}

/* Snapshot geometry before layout, run layout, then invalidate only
   nodes whose position, size, or content size actually changed.
   Returns true if any node was invalidated (paint pass needed). */
static void mark_subtree_content_dirty(Ca_Node *node);

static bool layout_and_invalidate(Ca_Window *win)
{
    static NodeRect prev[CA_MAX_NODES_PER_WINDOW];

    for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
        Ca_Node *n = &win->node_pool[j];
        if (n->in_use)
            prev[j] = (NodeRect){ n->x, n->y, n->w, n->h,
                                  n->content_w, n->content_h };
    }

    ca_node_propagate_layout(win);
    ca_layout_pass(win);

    bool any_dirty = false;
    for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
        Ca_Node *n = &win->node_pool[j];
        if (!n->in_use) continue;
        n->dirty &= ~(CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN);

        if (n->x != prev[j].x  || n->y  != prev[j].y  ||
            n->w != prev[j].w  || n->h  != prev[j].h  ||
            n->content_w != prev[j].cw ||
            n->content_h != prev[j].ch)
        {
            n->dirty |= CA_DIRTY_CONTENT;
            n->cache_count      = 0;
            n->cache_post_count = 0;
            if (n->desc.overflow_x >= 1 || n->desc.overflow_y >= 1) {
                for (uint32_t i = 0; i < n->child_count; ++i)
                    mark_subtree_content_dirty(n->children[i]);
            }
            any_dirty = true;
        }
    }

    return any_dirty;
}

static void mark_subtree_content_dirty(Ca_Node *node)
{
    if (!node || !node->in_use) return;
    node->dirty |= CA_DIRTY_CONTENT;
    node->cache_count = 0;
    node->cache_post_count = 0;
    for (uint32_t i = 0; i < node->child_count; ++i)
        mark_subtree_content_dirty(node->children[i]);
}

void ca_ui_update(Ca_Instance *inst)
{
    /* Builder rebuilds happen during ca_reactive_flush (called by
       ca_instance_tick before this function) so the new tree is already
       in place by the time we run layout. */
    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i) {
        Ca_Window *win = &inst->windows[i];
        if (!win->in_use || !win->root || !win->node_pool) continue;

        /* 1. Deferred scale rescale — applied once at frame-start so that
              static nodes built during ca_ui_begin (never reconciled) pick up
              the new ui_scale without corrupting layout values mid-slider-drag.
              Skipped while a drag is active on this window; the accumulated
              ratio fires on the first frame after the drag ends. */
        if (win->pending_scale_ratio > 0.0f && !win->drag_node) {
            float ratio = win->pending_scale_ratio;
            win->pending_scale_ratio = 0.0f;
            if (win->content_root)
                rescale_nodes(win->content_root, ratio);
        }

        /* 2. Transition tick — update animated properties, mark dirty */
        double now = glfwGetTime();
        bool any_transitions = false;
        uint32_t trans_count = 0;
        for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
            Ca_Node *n = &win->node_pool[j];
            if (!n->in_use) continue;
            if (transition_tick(n, now)) {
                any_transitions = true;
                n->dirty |= CA_DIRTY_CONTENT;
                trans_count++;
            }
        }
        win->dbg_transition_count = trans_count;

        /* 3. Check what kind of work this window needs */
        bool any_layout  = false;
        bool any_content = false;
        win->dbg_layout_count = 0;

        for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
            Ca_Node *n = &win->node_pool[j];
            if (!n->in_use) continue;
            if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN)) any_layout  = true;
            if (n->dirty & CA_DIRTY_CONTENT)                       any_content = true;
        }

        /* 4. Layout pass — recompute rects, only dirty moved nodes */
        if (any_layout) {
            win->dbg_layout_count = 1;
            if (layout_and_invalidate(win))
                any_content = true;
        }

        /* 5. Resize pass — handle edge/corner drag for undecorated windows.
              Must run before input pass so that a resize drag suppresses
              normal widget hit-testing on the same click. */
        ca_window_resize_pass(win);

        /* 6. Input pass — hit-test buttons and fire click callbacks.
              Run BEFORE paint so that input-driven dirty flags are
              picked up in the same frame's paint pass.
              Skip while a resize drag is active so edge clicks don't
              also trigger widgets underneath.  */
        Ca_Node *prev_hovered = win->hovered_node;
        Ca_Node *prev_focused = win->focused_node;
        Ca_Node *prev_drag    = win->drag_node;
        if (!win->resize_active)
            ca_widget_input_pass(win);

        /* Mark interactive state changes so paint catches them.
           Any CSS rule may use :hover / :focus / :focus-within / :active
           on any element, so the safe rule is: when hovered_node /
           focused_node / drag_node changes, walk the OLD and NEW ancestor
           chains and (a) re-resolve CSS in-place for every node along
           them via ca_widget_reapply_css, and (b) the reapply call
           internally sets CA_DIRTY_CONTENT / CA_DIRTY_LAYOUT only when
           the resolved desc actually changed.

           Why (a) is required:
             paint_node_content reads node->desc.background directly — it
             does NOT re-resolve CSS. The :hover / :focus / :active values
             are baked into node->desc by apply_css at widget creation
             time. For panels built with immediate-mode rebuild
             (ca_reconcile_begin every frame inside on_frame_fn), apply_css
             re-runs each frame and pseudo-state changes propagate. For
             panels built with ca_div_set_builder, the builder effect
             only re-runs when its tracked signals change. Without
             ca_widget_reapply_css here, apply_css never re-runs when only
             the hover/focus state changes, so pseudo-state CSS rules
             (e.g. :hover) appear stuck.

             ca_widget_reapply_css resets node->desc from the saved
             base_desc, re-runs ca_style_resolve + ca_style_apply_to_node,
             then diffs to set dirty flags — no subtree rebuild, no node
             allocation. Chains are short (≤ widget tree depth, typically
             5–10), so this is cheap and only repaints nodes whose CSS
             actually changed. */
        if (win->hovered_node != prev_hovered) {
            for (Ca_Node *n = prev_hovered; n; n = n->parent)
                ca_widget_reapply_css(n);
            for (Ca_Node *n = win->hovered_node; n; n = n->parent)
                ca_widget_reapply_css(n);
        }
        if (win->focused_node != prev_focused) {
            /* :focus is exact-match, :focus-within walks ancestors. */
            for (Ca_Node *n = prev_focused; n; n = n->parent)
                ca_widget_reapply_css(n);
            for (Ca_Node *n = win->focused_node; n; n = n->parent)
                ca_widget_reapply_css(n);
        }
        if (win->drag_node != prev_drag) {
            /* :active matches drag_node and its ancestors. */
            for (Ca_Node *n = prev_drag; n; n = n->parent)
                ca_widget_reapply_css(n);
            for (Ca_Node *n = win->drag_node; n; n = n->parent)
                ca_widget_reapply_css(n);
        }

        /* Re-scan for content dirty after input (widget state changes may
           have dirtied nodes that weren't dirty before).
           Also check for new layout dirty — click callbacks can change
           widget visibility (hidden), which requires a layout reflow
           before the paint pass runs. */
        {
            bool needs_layout_post_input = false;
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (!n->in_use) continue;
                if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN))
                    needs_layout_post_input = true;
                if (n->dirty & CA_DIRTY_CONTENT)
                    any_content = true;
            }
            if (needs_layout_post_input) {
                if (layout_and_invalidate(win))
                    any_content = true;
            }
        }

        /* Per-frame user callback — runs after input pass (scroll_y is
           updated) and before paint so any label changes are painted
           in the same frame.  Build context is activated so that
           widget creation (ca_div_clear + rebuild) works. */
        if (win->on_frame_fn) {
            ca_widget_ctx_enter(win);
            win->on_frame_fn(win->on_frame_data);
            ca_widget_ctx_leave();

            /* The callback may have dirtied nodes (label text, hidden, etc.)
               or triggered a layout change.  Re-scan so the paint pass below
               picks them up in the same frame. */
            bool needs_layout = false;
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (!n->in_use) continue;
                if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN))
                    needs_layout = true;
                if (n->dirty & CA_DIRTY_CONTENT)
                    any_content = true;
            }
            if (needs_layout) {
                if (layout_and_invalidate(win))
                    any_content = true;
            }
        }

        /* Title bar rebuild — runs after on_frame_fn so any menu changes
           from the frame callback are reflected in the same frame.        */
        if (win->titlebar_needs_rebuild) {
            ca_widget_ctx_enter(win);
            ca_title_bar_rebuild(win);
            ca_widget_ctx_leave();
            win->titlebar_needs_rebuild = false;

            bool needs_layout_tb = false;
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (!n->in_use) continue;
                if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN))
                    needs_layout_tb = true;
                if (n->dirty & CA_DIRTY_CONTENT)
                    any_content = true;
            }
            if (needs_layout_tb) {
                if (layout_and_invalidate(win))
                    any_content = true;
            }
        }

        /* Status bar rebuild — same pattern as title bar. */
        if (win->statusbar_needs_rebuild) {
            ca_widget_ctx_enter(win);
            ca_status_bar_rebuild(win);
            ca_widget_ctx_leave();
            win->statusbar_needs_rebuild = false;

            bool needs_layout_sb = false;
            for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j) {
                Ca_Node *n = &win->node_pool[j];
                if (!n->in_use) continue;
                if (n->dirty & (CA_DIRTY_LAYOUT | CA_DIRTY_CHILDREN))
                    needs_layout_sb = true;
                if (n->dirty & CA_DIRTY_CONTENT)
                    any_content = true;
            }
            if (needs_layout_sb) {
                if (layout_and_invalidate(win))
                    any_content = true;
            }
        }

        /* 6. Incremental paint pass — only dirty nodes are repainted;
              clean nodes reuse cached draw commands.
              The one-shot dbg_force_repaint flag forces a single paint pass
              when the debug overlay is toggled (so the panel appears/disappears). */

        /* Guard: if the cache pool is more than 75 % full, compact it
           so that cache_commands() doesn't silently drop entries (which
           would make clean nodes invisible on subsequent frames).
           Compaction reclaims dead/orphaned slots WITHOUT marking any
           node dirty — only nodes that genuinely changed will repaint. */
        if (win->paint_cache_used > CA_MAX_DRAW_CMDS_PER_WINDOW * 3 / 4) {
            ca_paint_cache_compact(win);
        }

        if (any_content || win->dbg_force_repaint) {
            /* Count dirty nodes for debug display */
            if (win->debug_overlay) {
                uint32_t dc = 0;
                for (uint32_t j = 0; j < CA_MAX_NODES_PER_WINDOW; ++j)
                    if (win->node_pool[j].in_use &&
                        (win->node_pool[j].dirty & CA_DIRTY_CONTENT))
                        dc++;
                win->dbg_dirty_count = dc;
            }
            ca_paint_pass(inst, win);
            win->dbg_force_repaint = false;
            win->needs_render = true;
        } else {
            win->dbg_dirty_count = 0;
        }

        /* 7. Request another tick for active transitions so we don't stall
              inside glfwWaitEvents() while animations are running.  */
        if (any_transitions)
            glfwPostEmptyEvent();
    }
}
