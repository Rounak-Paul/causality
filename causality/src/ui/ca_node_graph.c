// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/*
 * ca_node_graph.c — Reusable node graph canvas built entirely from
 * Causality div primitives.  No raw GPU rendering required.
 *
 * Layout overview
 * ===============
 *  Canvas div (overflow:hidden, dark bg, drag=pan, scroll=zoom)
 *    Grid lines   (absolute, emitted first; density adapts to zoom)
 *    Node outer   (absolute, ALL dimensions × zoom)
 *      Header     (row, centred; .ca-ng-header)
 *      Body       (column)
 *        Input rows   ●dot  label
 *        Separator    1px hr
 *        Output rows  label  ●dot      (right-aligned)
 *    Wire pieces (absolute bars: orthogonal legs + rounded corner arcs)
 *
 * Zoom behaviour
 * ==============
 *  - All node/pin/header dimensions multiply by ng->zoom.
 *  - Grid effective spacing doubles when it would go below NG_GRID_MIN_SCR_PX.
 *  - Text keeps its CSS font size; a header or pin row too short for the
 *    font's line height draws a stub line sized to the label instead.
 *  - Scroll zooms about the cursor; drags convert screen deltas to canvas space.
 *  - Wires touching the selected node are thicker; the rest take .dimmed.
 *
 * Geometry
 * ========
 *  Node width, height and pin centres are recorded by the latest build (rows
 *  can be custom), so wires and link drops use real positions. Canvas space
 *  is layout px divided by zoom; the canvas origin is its top-left corner.
 */

#include "ca_node_graph.h"
#include "ca_components.h"
#include "style.h"
#include "../core/ca_internal.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>

/* ============================================================
   LAYOUT CONSTANTS  (unscaled logical px; multiply by zoom at use-site)
   ============================================================ */

#define NG_NODE_W           180.0f  /* node width                          */
#define NG_HEADER_H          28.0f  /* header height                       */
#define NG_PIN_ROW_H         22.0f  /* pin row height                      */
#define NG_PIN_DOT_D          8.0f  /* pin dot diameter                    */
#define NG_PIN_PAD_L          8.0f  /* left padding in pin row             */
#define NG_PIN_PAD_R          8.0f  /* right padding in pin row            */
#define NG_PIN_GAP            5.0f  /* gap between dot and label           */
#define NG_BODY_PAD_V         4.0f  /* top/bottom body padding             */
#define NG_WIRE_T             2.0f  /* wire thickness                      */
#define NG_WIRE_ACTIVE_T      3.5f  /* thickness of wires on the selected node */
#define NG_WIRE_STUB         24.0f  /* min run out of a pin before turning */
#define NG_WIRE_CLEAR        24.0f  /* lane clearance around nodes         */
#define NG_WIRE_RADIUS       10.0f  /* corner radius                       */
#define NG_WIRE_MAX_LEGS      5     /* straight legs of the longest route  */
#define NG_WIRE_ARC_PIECES    4     /* bars per rounded corner             */
#define NG_WIRE_PIECES       (NG_WIRE_MAX_LEGS + (NG_WIRE_MAX_LEGS - 1) * NG_WIRE_ARC_PIECES)
#define NG_GRID_SPACING      50.0f  /* logical px between grid lines       */
#define NG_GRID_THICK         1.0f  /* grid line visual thickness          */
#define NG_GRID_MAJOR_EVERY   4     /* every Nth grid line is .major       */

/* Grid adaptivity */
#define NG_GRID_MIN_SCR_PX   20.0f  /* min screen px between lines        */

/* View */
#define NG_ZOOM_MIN           0.15f
#define NG_ZOOM_MAX           4.0f
#define NG_ZOOM_STEP          1.08f  /* zoom factor per scroll unit         */
#define NG_FIT_MARGIN        40.0f   /* canvas px kept around fitted nodes  */
#define NG_CLICK_SLOP         3.0f   /* max drag distance treated as a click */

/* Level of detail: a row shows text only when it is this much taller than
   the font's line height, so adjacent lines keep visible leading. */
#define NG_TEXT_LEADING       1.15f
#define NG_PIN_STUB_T         2.0f   /* pin label stub thickness            */
#define NG_TITLE_STUB_T       3.0f   /* title stub thickness                */
#define NG_NODE_SELECTED_Z      10   /* selected node stack level           */
#define NG_ROW_MIN_ZOOM       0.9f   /* host widgets keep their size above this zoom */
#define NG_PIN_HIT_D         16.0f   /* draggable pin area                  */
#define NG_LINK_Z             20     /* wire being dragged, above nodes     */
#define NG_DROP_REACH          6.0f  /* drop tolerance beyond a node's edge */

/* ============================================================
   INTERNAL HELPERS
   ============================================================ */

static Ca_NgNodeState *ng_find_or_create(Ca_NodeGraph *ng,
                                          const char *key,
                                          float init_x, float init_y)
{
    if (!ng || !key) return NULL;

    /* Prefer an existing slot with matching key */
    for (int i = 0; i < ng->node_count; ++i) {
        Ca_NgNodeState *state = ca_node_graph_state(ng, i);
        if (state && strncmp(state->key, key, CA_NG_KEY_LEN) == 0)
            return state;
    }

    Ca_NgNodeState *state = CA_CALLOC(1, sizeof(*state));
    if (!state || !ca_dyn_array_push(&ng->_nodes, &state)) {
        CA_FREE(state);
        return NULL;
    }
    state->_index = ng->node_count++;
    snprintf(state->key, CA_NG_KEY_LEN, "%s", key);
    state->canvas_x = init_x;
    state->canvas_y = init_y;
    state->valid    = true;
    state->_ng      = ng;
    return state;
}

static void ng_set_node_z(Ca_Node *node, int z)
{
    if (!node) return;
    if (node->desc.z_index == z) return;
    node->desc.z_index = (int16_t)z;
    node->dirty |= CA_DIRTY_CONTENT;
}

static void ng_set_label_z(Ca_Label *label, int z)
{
    if (!label) return;
    ng_set_node_z(label->node, z);
}

typedef struct NgWireRecord {
    int src, src_pin;
    int dst, dst_pin;
} NgWireRecord;

/* Logical height of a node: recorded by its latest build, estimated for pins
   alone before the first one. */
static float ng_node_h(const Ca_NgNodeState *state)
{
    if (state->height > 0.0f) return state->height;
    float sep = (state->input_count > 0 && state->output_count > 0) ? NG_GRID_THICK : 0.0f;
    return NG_HEADER_H + 2.0f * NG_BODY_PAD_V + sep
         + (float)(state->input_count + state->output_count) * NG_PIN_ROW_H;
}

static float ng_node_w(const Ca_NgNodeState *state)
{
    return state->width > 0.0f ? state->width : NG_NODE_W;
}

/* Centre-Y of a pin below the node top, in logical px. */
static float ng_pin_y(const Ca_NgNodeState *state, int pin_idx, bool output)
{
    if (pin_idx >= 0 && pin_idx < CA_NG_MAX_PINS) {
        float y = output ? state->out_y[pin_idx] : state->in_y[pin_idx];
        if (y > 0.0f) return y;
    }
    float base = NG_HEADER_H + NG_BODY_PAD_V + NG_PIN_ROW_H * 0.5f;
    if (!output) return base + (float)pin_idx * NG_PIN_ROW_H;
    float sep = state->input_count > 0 ? NG_GRID_THICK : 0.0f;
    return base + (float)state->input_count * NG_PIN_ROW_H + sep + (float)pin_idx * NG_PIN_ROW_H;
}

/* Line height in layout px of labels with `classes`, or INFINITY without a font.
   Writes the resolved CSS font size (0 = font default) to out_font_px. */
static float ng_label_line_h(Ca_Window *win, const char *classes, float *out_font_px)
{
    float font_px = 0.0f;
    Ca_Instance *instance = win->instance;
    if (instance->system_stylesheet || instance->stylesheet) {
        Ca_ResolvedStyle rs;
        ca_style_resolve_layers(instance->system_stylesheet, instance->stylesheet,
                                NULL, CA_ELEM_TEXT, classes, &rs);
        font_px = rs.font_size > 0.0f ? rs.font_size : 0.0f;
    }
    *out_font_px = font_px;
    float ascent = 0.0f, descent = 0.0f;
    if (!ca_font_line_metrics(win, font_px, &ascent, &descent)) return INFINITY;
    return ascent - descent;
}

/* Stub line standing in for `text`: the label's canvas-space width at this
   zoom, clamped to the room left in its row. */
static void emit_text_stub(const Ca_NodeGraph *ng, const char *text, float font_px,
                           float avail, float thickness, uint32_t color,
                           const char *style, int z)
{
    float ui_s = ng->_window->ui_scale > 0.0f ? ng->_window->ui_scale : 1.0f;
    float text_w = ca_measure_text_px(ng->_window, text, font_px) / ui_s;
    float w = fminf(text_w * ng->zoom, fmaxf(avail, 0.0f));
    ca_div_begin(&(Ca_DivDesc){
        .hidden        = w < 0.5f,
        .width         = w,
        .height        = thickness,
        .background    = color,
        .corner_radius = thickness * 0.5f,
        .style         = style,
        .z_index       = z,
    });
    ca_div_end();
}

/* ============================================================
   WIRES
   ============================================================ */

typedef struct NgPoint {
    float x, y;
} NgPoint;

/* One wire piece from a to b as a rotated bar. Always emits exactly one div,
 * hidden when empty or off-canvas, so the canvas keeps a stable child count:
 * Causality matches children by sequential index, and a shifting count would
 * move node divs onto wrong slots and break their drag callbacks. */
static void emit_wire_piece(const Ca_NodeGraph *ng, NgPoint a, NgPoint b,
                            float thickness, uint32_t color, const char *style, int z)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    bool visible = isfinite(len) && len >= 0.5f;
    if (visible && ng->_canvas_w > 0.0f) {
        visible = fmaxf(a.x, b.x) + thickness >= 0.0f &&
                  fminf(a.x, b.x) - thickness <= ng->_canvas_w &&
                  fmaxf(a.y, b.y) + thickness >= 0.0f &&
                  fminf(a.y, b.y) - thickness <= ng->_canvas_h;
    }
    ca_div_begin(&(Ca_DivDesc){
        .hidden     = !visible,
        .position   = CA_POSITION_ABSOLUTE,
        .pos_x      = visible ? (a.x + b.x - len) * 0.5f : 0.0f,
        .pos_y      = visible ? (a.y + b.y - thickness) * 0.5f : 0.0f,
        .width      = visible ? len : 0.0f,
        .height     = thickness,
        .background = color,
        .rotation   = visible ? atan2f(dy, dx) * (180.0f / 3.14159265f) : 0.0f,
        .no_hover   = true,
        .style      = style,
        .z_index    = z,
    });
    ca_div_end();
}

static float ng_dist(NgPoint a, NgPoint b)
{
    return fabsf(b.x - a.x) + fabsf(b.y - a.y);
}

/* Unit axis direction from a to b (legs are axis-aligned). */
static NgPoint ng_dir(NgPoint a, NgPoint b)
{
    float len = ng_dist(a, b);
    return len > 0.0f ? (NgPoint){ (b.x - a.x) / len, (b.y - a.y) / len } : (NgPoint){ 0.0f, 0.0f };
}

/* Emits an orthogonal polyline as exactly NG_WIRE_PIECES bars. Collinear and
   zero-length legs are merged first, so every remaining corner is a 90 degree
   turn. A corner rounds with radius min(radius, half of each adjacent leg);
   below the thickness it is square, owned by the incoming leg, so no two
   pieces overlap and translucent wires never double-blend. */
static void emit_orthogonal_wire(const Ca_NodeGraph *ng, const NgPoint *route, int route_count,
                                 float radius, float thickness, uint32_t color,
                                 const char *style, int z)
{
    NgPoint pts[NG_WIRE_MAX_LEGS + 1];
    int n = 0;
    for (int i = 0; i < route_count; ++i) {
        if (n > 0 && ng_dist(pts[n - 1], route[i]) < 0.01f) continue;
        if (n > 1) {
            NgPoint d0 = ng_dir(pts[n - 2], pts[n - 1]);
            NgPoint d1 = ng_dir(pts[n - 1], route[i]);
            if (d0.x * d1.x + d0.y * d1.y > 0.5f) { pts[n - 1] = route[i]; continue; }
        }
        pts[n++] = route[i];
    }

    float corner_r[NG_WIRE_MAX_LEGS + 1] = {0};
    for (int i = 1; i < n - 1; ++i) {
        float r = fminf(radius, 0.5f * fminf(ng_dist(pts[i - 1], pts[i]), ng_dist(pts[i], pts[i + 1])));
        corner_r[i] = r >= thickness ? r : 0.0f;
    }

    int emitted = 0;
    float half_t = 0.5f * thickness;
    for (int i = 0; i + 1 < n; ++i) {
        NgPoint d = ng_dir(pts[i], pts[i + 1]);
        NgPoint a = pts[i], b = pts[i + 1];
        if (i > 0) {
            float trim = corner_r[i] > 0.0f ? corner_r[i] : half_t;
            a = (NgPoint){ a.x + d.x * trim, a.y + d.y * trim };
        }
        if (i + 2 < n) {
            float trim = corner_r[i + 1] > 0.0f ? -corner_r[i + 1] : half_t;
            b = (NgPoint){ b.x + d.x * trim, b.y + d.y * trim };
        }
        if ((b.x - a.x) * d.x + (b.y - a.y) * d.y <= 0.0f) b = a;
        emit_wire_piece(ng, a, b, thickness, color, style, z);
        ++emitted;
    }
    for (int i = 1; i + 1 < n; ++i) {
        float r = corner_r[i];
        NgPoint u = ng_dir(pts[i - 1], pts[i]);
        NgPoint v = ng_dir(pts[i], pts[i + 1]);
        NgPoint c = { pts[i].x - u.x * r + v.x * r, pts[i].y - u.y * r + v.y * r };
        NgPoint prev = { pts[i].x - u.x * r, pts[i].y - u.y * r };
        for (int k = 1; k <= NG_WIRE_ARC_PIECES; ++k) {
            float t = (float)k / (float)NG_WIRE_ARC_PIECES * 1.57079632679f;
            NgPoint next = { c.x + (-v.x * cosf(t) + u.x * sinf(t)) * r,
                             c.y + (-v.y * cosf(t) + u.y * sinf(t)) * r };
            emit_wire_piece(ng, prev, r > 0.0f ? next : prev, thickness, color, style, z);
            prev = next;
            ++emitted;
        }
    }
    for (; emitted < NG_WIRE_PIECES; ++emitted)
        emit_wire_piece(ng, (NgPoint){0}, (NgPoint){0}, thickness, color, style, z);
}

/* Frames every node inside the canvas; false while the canvas is unsized or empty. */
static bool ng_fit(Ca_NodeGraph *ng)
{
    if (ng->_canvas_w <= 0.0f || ng->_canvas_h <= 0.0f) return false;
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (int i = 0; i < ng->node_count; ++i) {
        const Ca_NgNodeState *state = ca_node_graph_state(ng, i);
        if (!state || !state->valid) continue;
        min_x = fminf(min_x, state->canvas_x);
        min_y = fminf(min_y, state->canvas_y);
        max_x = fmaxf(max_x, state->canvas_x + ng_node_w(state));
        max_y = fmaxf(max_y, state->canvas_y + ng_node_h(state));
    }
    if (!(min_x <= max_x)) return false;
    float avail_w = fmaxf(ng->_canvas_w - 2.0f * NG_FIT_MARGIN, 1.0f);
    float avail_h = fmaxf(ng->_canvas_h - 2.0f * NG_FIT_MARGIN, 1.0f);
    float zoom = fminf(avail_w / (max_x - min_x), avail_h / (max_y - min_y));
    zoom = fmaxf(NG_ZOOM_MIN, fminf(zoom, 1.0f));
    ng->zoom  = zoom;
    ng->pan_x = ng->_canvas_w * 0.5f - (min_x + max_x) * 0.5f * zoom;
    ng->pan_y = ng->_canvas_h * 0.5f - (min_y + max_y) * 0.5f * zoom;
    return true;
}

/* Layout px per logical px for the window delivering an event. */
static float ng_event_scale(const Ca_DragEvent *ev)
{
    return ev->window && ev->window->ui_scale > 0.0f ? ev->window->ui_scale : 1.0f;
}


/* Canvas-space cursor position of a drag event. */
static bool ng_cursor_canvas(const Ca_NodeGraph *ng, const Ca_DragEvent *ev, float *cx, float *cy)
{
    const Ca_Node *canvas = (const Ca_Node *)ng->_canvas;
    if (!canvas) return false;
    float ui_s = ng_event_scale(ev);
    float zoom = fmaxf(ng->zoom, NG_ZOOM_MIN);
    *cx = ((ev->x - canvas->x) / ui_s - ng->pan_x) / zoom;
    *cy = ((ev->y - canvas->y) / ui_s - ng->pan_y) / zoom;
    return true;
}

static void ng_invalidate(Ca_NodeGraph *ng)
{
    if (ng->_host_div) ca_div_invalidate(ng->_host_div);
}

/* ============================================================
   SELECTION
   ============================================================ */

static bool ng_node_selected(const Ca_NodeGraph *ng, const Ca_NgNodeState *state)
{
    return state->valid && (state->selected || state->_index == ng->selected_node);
}

static void ng_notify_select(Ca_NodeGraph *ng)
{
    if (ng->_on_node_select) ng->_on_node_select(ng, ng->selected_node, ng->_select_data);
}

static void ng_clear_flags(Ca_NodeGraph *ng)
{
    for (int i = 0; i < ng->node_count; ++i) {
        Ca_NgNodeState *state = ca_node_graph_state(ng, i);
        if (state) state->selected = false;
    }
    ng->selected_node = -1;
}

bool ca_node_graph_is_selected(const Ca_NodeGraph *ng, int node_idx)
{
    if (!ng || node_idx < 0 || node_idx >= ng->node_count) return false;
    return ng_node_selected(ng, ca_node_graph_state((Ca_NodeGraph *)ng, node_idx));
}

int ca_node_graph_selection_count(const Ca_NodeGraph *ng)
{
    int count = 0;
    for (int i = 0; ng && i < ng->node_count; ++i)
        if (ca_node_graph_is_selected(ng, i)) ++count;
    return count;
}

void ca_node_graph_select(Ca_NodeGraph *ng, int node_idx, bool additive)
{
    if (!ng) return;
    if (!additive) ng_clear_flags(ng);
    Ca_NgNodeState *state = ca_node_graph_state(ng, node_idx);
    if (state) {
        state->selected = true;
        ng->selected_node = node_idx;
    }
    ng_invalidate(ng);
}

void ca_node_graph_clear_selection(Ca_NodeGraph *ng)
{
    if (!ng) return;
    ng_clear_flags(ng);
    ng_invalidate(ng);
}

/* Removes one node from the selection; the primary falls to another member. */
static void ng_deselect(Ca_NodeGraph *ng, int node_idx)
{
    Ca_NgNodeState *state = ca_node_graph_state(ng, node_idx);
    if (!state) return;
    state->selected = false;
    if (ng->selected_node != node_idx) return;
    ng->selected_node = -1;
    for (int i = 0; i < ng->node_count; ++i) {
        const Ca_NgNodeState *other = ca_node_graph_state(ng, i);
        if (other && other->selected) { ng->selected_node = i; break; }
    }
}

/* ============================================================
   DRAG CALLBACKS - canvas panning and box selection
   ============================================================ */

static void canvas_drag_start(const Ca_DragEvent *ev, void *ud)
{
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    ng->_pan_drag_start_x = ng->pan_x;
    ng->_pan_drag_start_y = ng->pan_y;
    ng->_box_active = false;
    if ((ca_window_mouse_mods(ev->window) & (CA_MOD_SHIFT | CA_MOD_CONTROL)) == 0) return;
    float cx, cy;
    if (!ng_cursor_canvas(ng, ev, &cx, &cy)) return;
    ng->_box_active = true;
    ng->_box_x0 = ng->_box_x1 = cx;
    ng->_box_y0 = ng->_box_y1 = cy;
}

static void canvas_drag(const Ca_DragEvent *ev, void *ud)
{
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    if (ng->_box_active) {
        float cx, cy;
        if (ng_cursor_canvas(ng, ev, &cx, &cy)) { ng->_box_x1 = cx; ng->_box_y1 = cy; }
        ng_invalidate(ng);
        return;
    }
    float inv = 1.0f / ng_event_scale(ev);
    ng->pan_x = ng->_pan_drag_start_x + ev->dx * inv;
    ng->pan_y = ng->_pan_drag_start_y + ev->dy * inv;
    ng_invalidate(ng);
}

/* A click (not a pan) on empty canvas clears the selection; a box adds the
   nodes it touches. */
static void canvas_drag_end(const Ca_DragEvent *ev, void *ud)
{
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    if (ng->_box_active) {
        ng->_box_active = false;
        float x0 = fminf(ng->_box_x0, ng->_box_x1), x1 = fmaxf(ng->_box_x0, ng->_box_x1);
        float y0 = fminf(ng->_box_y0, ng->_box_y1), y1 = fmaxf(ng->_box_y0, ng->_box_y1);
        for (int i = 0; i < ng->node_count; ++i) {
            Ca_NgNodeState *state = ca_node_graph_state(ng, i);
            if (!state || !state->valid) continue;
            bool hit = state->canvas_x <= x1 && state->canvas_x + ng_node_w(state) >= x0 &&
                       state->canvas_y <= y1 && state->canvas_y + ng_node_h(state) >= y0;
            if (!hit) continue;
            state->selected = true;
            ng->selected_node = i;
        }
        ng_notify_select(ng);
        ng_invalidate(ng);
        return;
    }
    float slop = NG_CLICK_SLOP * ng_event_scale(ev);
    if (fabsf(ev->dx) > slop || fabsf(ev->dy) > slop || ca_node_graph_selection_count(ng) == 0)
        return;
    ng_clear_flags(ng);
    ng_notify_select(ng);
    ng_invalidate(ng);
}

/* ============================================================
   DRAG CALLBACKS — node movement + selection
   ============================================================
   Causality picks the SMALLEST draggable div under the cursor, so
   pin areas win over their node, and node divs (smaller area) always
   win over the canvas background div.
   drag_fn_start fires on mouse-down, making it suitable for selection.
   ============================================================ */

static void node_drag_start(const Ca_DragEvent *ev, void *ud)
{
    Ca_NgNodeState *state = (Ca_NgNodeState *)ud;
    Ca_NodeGraph   *ng    = state->_ng;
    if (!ng) return;

    int  idx = state->_index;
    bool additive = (ca_window_mouse_mods(ev->window) & (CA_MOD_SHIFT | CA_MOD_CONTROL)) != 0;
    bool was_selected = ng_node_selected(ng, state);
    ng->_drag_additive = additive;
    ng->_moved_distance = 0.0f;

    if (additive && was_selected) {
        ng_deselect(ng, idx);
    } else if (additive || !was_selected) {
        ca_node_graph_select(ng, idx, additive);
    } else {
        ng->selected_node = idx;   /* keeps the group; this one leads it */
    }

    for (int i = 0; i < ng->node_count; ++i) {
        Ca_NgNodeState *node = ca_node_graph_state(ng, i);
        if (!node || !ng_node_selected(ng, node)) continue;
        node->_drag_start_x = node->canvas_x;
        node->_drag_start_y = node->canvas_y;
    }
    ng_notify_select(ng);
    ng_invalidate(ng);
}

static void node_drag(const Ca_DragEvent *ev, void *ud)
{
    Ca_NgNodeState *state = (Ca_NgNodeState *)ud;
    Ca_NodeGraph   *ng    = state->_ng;
    if (!ng || !ng_node_selected(ng, state)) return;

    /* Convert screen-pixel delta to canvas-space delta by dividing by zoom.
     * This ensures the node moves exactly as far as the cursor, regardless
     * of current zoom level. */
    float inv = 1.0f / (fmaxf(ng->zoom, NG_ZOOM_MIN) * ng_event_scale(ev));
    float dx = ev->dx * inv, dy = ev->dy * inv;
    ng->_moved_distance = fmaxf(ng->_moved_distance, fmaxf(fabsf(ev->dx), fabsf(ev->dy)));
    for (int i = 0; i < ng->node_count; ++i) {
        Ca_NgNodeState *node = ca_node_graph_state(ng, i);
        if (!node || !ng_node_selected(ng, node)) continue;
        node->canvas_x = node->_drag_start_x + dx;
        node->canvas_y = node->_drag_start_y + dy;
    }
    ng_invalidate(ng);
}

static void node_drag_end(const Ca_DragEvent *ev, void *ud)
{
    Ca_NgNodeState *state = (Ca_NgNodeState *)ud;
    Ca_NodeGraph   *ng    = state->_ng;
    if (!ng) return;
    if (ng->_moved_distance > NG_CLICK_SLOP * ng_event_scale(ev)) {
        if (ng->_on_nodes_moved) ng->_on_nodes_moved(ng, ng->_moved_data);
        return;
    }
    /* A plain click inside a multi-selection narrows it to that node. */
    if (!ng->_drag_additive && ng_node_selected(ng, state) && ca_node_graph_selection_count(ng) > 1) {
        ca_node_graph_select(ng, state->_index, false);
        ng_notify_select(ng);
    }
}

/* ============================================================
   DRAG CALLBACKS — links
   ============================================================ */

/* Pin of the wanted kind under a canvas point, topmost node first. */
static bool ng_pin_at(const Ca_NodeGraph *ng, float cx, float cy, bool want_output,
                      int exclude_node, int *out_node, int *out_pin)
{
    for (int i = ng->node_count - 1; i >= 0; --i) {
        if (i == exclude_node) continue;
        const Ca_NgNodeState *state = ca_node_graph_state((Ca_NodeGraph *)ng, i);
        if (!state || !state->valid) continue;
        float w = ng_node_w(state);
        if (cx < state->canvas_x - NG_DROP_REACH || cx > state->canvas_x + w + NG_DROP_REACH) continue;
        int count = want_output ? state->output_count : state->input_count;
        for (int pin = 0; pin < count && pin < CA_NG_MAX_PINS; ++pin) {
            float y = state->canvas_y + ng_pin_y(state, pin, want_output);
            if (fabsf(cy - y) > NG_PIN_ROW_H * 0.5f) continue;
            *out_node = i;
            *out_pin = pin;
            return true;
        }
    }
    return false;
}

static Ca_NgPinRef ng_pin_ref(const Ca_NodeGraph *ng, int node, int pin)
{
    const Ca_NgNodeState *state = ca_node_graph_state((Ca_NodeGraph *)ng, node);
    return (Ca_NgPinRef){ state ? state->key : NULL, pin };
}

/* Whether `output` may feed `input`, as the host judges. */
static bool ng_can_link(Ca_NodeGraph *ng, int out_node, int out_pin, int in_node, int in_pin)
{
    if (out_node == in_node) return false;
    if (!ng->_can_link) return true;
    Ca_NgPinRef from = ng_pin_ref(ng, out_node, out_pin);
    Ca_NgPinRef to   = ng_pin_ref(ng, in_node, in_pin);
    return ng->_can_link(ng, &from, &to, ng->_link_data);
}

static void pin_drag_start(const Ca_DragEvent *ev, void *ud)
{
    const Ca_NgPinHandle *pin = (const Ca_NgPinHandle *)ud;
    Ca_NodeGraph *ng = pin->ng;
    if (!ng) return;
    ng->_link_active = true;
    ng->_link_node = pin->node;
    ng->_link_pin = pin->pin;
    ng->_link_output = pin->output;
    ng->_link_detached_node = -1;
    ng->_link_detached_pin = -1;
    if (!pin->output) {
        /* A connected input hands its wire over: the drag continues from the source. */
        for (size_t i = 0; i < ng->_wires.count; ++i) {
            const NgWireRecord *wire = ca_dyn_array_at(&ng->_wires, i);
            if (wire->dst != pin->node || wire->dst_pin != pin->pin) continue;
            ng->_link_detached_node = pin->node;
            ng->_link_detached_pin = pin->pin;
            ng->_link_node = wire->src;
            ng->_link_pin = wire->src_pin;
            ng->_link_output = true;
            break;
        }
    }
    ng_cursor_canvas(ng, ev, &ng->_cursor_x, &ng->_cursor_y);
    ng_invalidate(ng);
}

static void pin_drag(const Ca_DragEvent *ev, void *ud)
{
    Ca_NodeGraph *ng = ((const Ca_NgPinHandle *)ud)->ng;
    if (!ng || !ng->_link_active) return;
    ng_cursor_canvas(ng, ev, &ng->_cursor_x, &ng->_cursor_y);
    ng_invalidate(ng);
}

static void pin_drag_end(const Ca_DragEvent *ev, void *ud)
{
    Ca_NodeGraph *ng = ((const Ca_NgPinHandle *)ud)->ng;
    if (!ng || !ng->_link_active) return;
    ng->_link_active = false;
    ng_invalidate(ng);

    float cx, cy;
    if (!ng_cursor_canvas(ng, ev, &cx, &cy)) return;
    float slop = NG_CLICK_SLOP * ng_event_scale(ev);
    bool moved = fabsf(ev->dx) > slop || fabsf(ev->dy) > slop;
    if (!moved || !ng->_on_link) return;

    Ca_NgLinkEvent event = { .canvas_x = cx, .canvas_y = cy };
    int node, pin;
    if (ng->_link_output) {
        event.from = ng_pin_ref(ng, ng->_link_node, ng->_link_pin);
        if (ng_pin_at(ng, cx, cy, false, ng->_link_node, &node, &pin) &&
            ng_can_link(ng, ng->_link_node, ng->_link_pin, node, pin))
            event.to = ng_pin_ref(ng, node, pin);
    } else {
        event.to = ng_pin_ref(ng, ng->_link_node, ng->_link_pin);
        if (ng_pin_at(ng, cx, cy, true, ng->_link_node, &node, &pin) &&
            ng_can_link(ng, node, pin, ng->_link_node, ng->_link_pin))
            event.from = ng_pin_ref(ng, node, pin);
    }
    if (ng->_link_detached_node >= 0)
        event.detached = ng_pin_ref(ng, ng->_link_detached_node, ng->_link_detached_pin);
    if (event.to.node && event.detached.node &&
        strcmp(event.to.node, event.detached.node) == 0 && event.to.pin == event.detached.pin)
        return;   /* dropped back where it was picked up */
    ng->_on_link(ng, &event, ng->_link_data);
}

/* ============================================================
   PUBLIC API
   ============================================================ */

/* Zooms about the cursor so the canvas point under it stays put. */
static void canvas_scroll(double dx, double dy, void *ud)
{
    (void)dx;
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    float old_zoom = ng->zoom;
    float zoom = fmaxf(NG_ZOOM_MIN, fminf(old_zoom * powf(NG_ZOOM_STEP, (float)dy), NG_ZOOM_MAX));
    if (zoom == old_zoom) return;
    const Ca_Node *canvas = (const Ca_Node *)ng->_canvas;
    if (canvas && canvas->window) {
        float ui_s = canvas->window->ui_scale > 0.0f ? canvas->window->ui_scale : 1.0f;
        float cx = ((float)canvas->window->mouse_x - canvas->x) / ui_s;
        float cy = ((float)canvas->window->mouse_y - canvas->y) / ui_s;
        ng->pan_x = cx - (cx - ng->pan_x) * (zoom / old_zoom);
        ng->pan_y = cy - (cy - ng->pan_y) * (zoom / old_zoom);
    }
    ng->zoom = zoom;
    ng_invalidate(ng);
}

void ca_node_graph_request_fit(Ca_NodeGraph *ng)
{
    if (!ng) return;
    ng->_fit_pending = true;
    ng_invalidate(ng);
}

bool ca_node_graph_init(Ca_NodeGraph *ng)
{
    if (!ng) return false;
    memset(ng, 0, sizeof(*ng));
    if (!ca_dyn_array_init(&ng->_nodes, sizeof(Ca_NgNodeState *)))
        return false;
    if (!ca_dyn_array_init(&ng->_wires, sizeof(NgWireRecord))) {
        ca_dyn_array_destroy(&ng->_nodes);
        return false;
    }
    ng->selected_node  = -1;
    ng->_cur_node_idx  = -1;
    ng->_cur_node_z    = 0;
    ng->_link_detached_node = -1;
    ng->zoom           = 1.0f;
    return true;
}

void ca_node_graph_destroy(Ca_NodeGraph *ng)
{
    if (!ng) return;
    if (ca_dyn_array_valid(&ng->_nodes)) {
        for (size_t i = 0; i < ng->_nodes.count; ++i)
            CA_FREE(*(Ca_NgNodeState **)ca_dyn_array_at(&ng->_nodes, i));
    }
    ca_dyn_array_destroy(&ng->_nodes);
    ca_dyn_array_destroy(&ng->_wires);
    memset(ng, 0, sizeof(*ng));
    ng->selected_node = -1;
    ng->_cur_node_idx = -1;
    ng->_link_detached_node = -1;
}

Ca_NgNodeState *ca_node_graph_state(Ca_NodeGraph *ng, int node_idx)
{
    if (!ng || node_idx < 0 || (size_t)node_idx >= ng->_nodes.count)
        return NULL;
    return *(Ca_NgNodeState **)ca_dyn_array_at(&ng->_nodes,
                                               (size_t)node_idx);
}

Ca_NgNodeState *ca_node_graph_add_state(Ca_NodeGraph *ng, const char *key,
                                         float initial_x, float initial_y)
{
    return ng_find_or_create(ng, key, initial_x, initial_y);
}

bool ca_node_graph_view_center(const Ca_NodeGraph *ng, float *out_x, float *out_y)
{
    if (!ng || ng->_canvas_w <= 0.0f || ng->_canvas_h <= 0.0f) return false;
    float zoom = fmaxf(ng->zoom, NG_ZOOM_MIN);
    if (out_x) *out_x = (ng->_canvas_w * 0.5f - ng->pan_x) / zoom;
    if (out_y) *out_y = (ng->_canvas_h * 0.5f - ng->pan_y) / zoom;
    return true;
}

bool ca_node_graph_hovered(const Ca_NodeGraph *ng)
{
    const Ca_Node *canvas = ng ? (const Ca_Node *)ng->_canvas : NULL;
    if (!canvas || !canvas->window) return false;
    const Ca_Window *win = canvas->window;
    return win->mouse_x >= canvas->x && win->mouse_x < canvas->x + canvas->w &&
           win->mouse_y >= canvas->y && win->mouse_y < canvas->y + canvas->h;
}

void ca_node_graph_begin(Ca_NodeGraph *ng, Ca_Div *host_div,
                          const Ca_NodeGraphDesc *desc)
{
    assert(ng);
    ng->_host_div     = host_div;
    ng->_cur_node_idx = -1;
    ng->_cur_in_idx   = 0;
    ng->_cur_out_idx  = 0;
    ng->_cur_node_z   = 0;
    ng->_build_epoch++;
    ca_dyn_array_clear(&ng->_wires);

    if (desc) {
        ng->_on_node_select = desc->on_node_select;
        ng->_select_data    = desc->select_data;
        ng->_on_link        = desc->on_link;
        ng->_can_link       = desc->can_link;
        ng->_link_data      = desc->link_data;
        ng->_on_nodes_moved = desc->on_nodes_moved;
        ng->_moved_data     = desc->moved_data;
    }

    char classes[CA_NODE_CLASS_MAX];
    if (desc && desc->style && desc->style[0])
        snprintf(classes, sizeof(classes), "ca-ng-canvas %s", desc->style);
    else
        snprintf(classes, sizeof(classes), "ca-ng-canvas");

    /* Canvas container — clips overflow, handles pan drag + scroll zoom */
    Ca_Div *canvas = ca_div_begin(&(Ca_DivDesc){
        .width         = desc ? desc->width  : 0.0f,
        .height        = desc ? desc->height : 0.0f,
        .id            = desc ? desc->id     : NULL,
        .style         = classes,
        .clip_content  = true,
        .on_drag_start = canvas_drag_start,
        .on_drag       = canvas_drag,
        .on_drag_end   = canvas_drag_end,
        .drag_data     = ng,
        .on_scroll     = canvas_scroll,
        .scroll_data   = ng,
    });

    Ca_Node *canvas_node = (Ca_Node *)canvas;
    Ca_Window *win = canvas_node->window;
    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    ng->_canvas = canvas;

    Ca_Signal *size_signal = ca_div_size_signal(canvas);
    const Ca_DivSize *size = size_signal ? ca_signal_get(size_signal) : NULL;
    ng->_canvas_w = size ? size->width  : canvas_node->w / ui_s;
    ng->_canvas_h = size ? size->height : canvas_node->h / ui_s;
    if (ng->_fit_pending && ng_fit(ng)) ng->_fit_pending = false;

    float title_line = ng_label_line_h(win, "ca-ng-title", &ng->_title_font_px);
    float pin_line   = ng_label_line_h(win, "ca-ng-pin-label", &ng->_pin_font_px);
    float out_font_px;
    pin_line = fmaxf(pin_line, ng_label_line_h(win, "ca-ng-pin-label ca-ng-out-label",
                                               &out_font_px));
    ng->_window        = win;
    ng->_show_title    = title_line * NG_TEXT_LEADING <= NG_HEADER_H  * ng->zoom * ui_s;
    ng->_show_pin_text = pin_line   * NG_TEXT_LEADING <= NG_PIN_ROW_H * ng->zoom * ui_s;

    /* --- Grid lines ---
     * Adapt grid spacing so lines are never closer than NG_GRID_MIN_SCR_PX on
     * screen (doubles each step).  Then compute how many lines cover ~3200 px
     * in each direction. */
    float eff_gs = NG_GRID_SPACING * ng->zoom;
    while (eff_gs < NG_GRID_MIN_SCR_PX) eff_gs *= 2.0f;

    float off_x = fmodf(ng->pan_x, eff_gs);
    float off_y = fmodf(ng->pan_y, eff_gs);

    int nv = (int)(3200.0f / eff_gs) + 4;
    int nh = (int)(3200.0f / eff_gs) + 4;

    /* Canvas-space line index of the first emitted line, for .major. */
    int first_x = (int)lroundf((off_x - eff_gs - ng->pan_x) / eff_gs);
    int first_y = (int)lroundf((off_y - eff_gs - ng->pan_y) / eff_gs);

    for (int i = 0; i < nv; ++i) {
        float x = off_x + (float)(i - 1) * eff_gs;
        bool major = ((first_x + i) % NG_GRID_MAJOR_EVERY + NG_GRID_MAJOR_EVERY) % NG_GRID_MAJOR_EVERY == 0;
        ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE,
            .pos_x    = x,
            .pos_y    = -eff_gs,
            .width    = NG_GRID_THICK,
            .height   = (float)(nh + 2) * eff_gs,
            .no_hover = true,
            .style    = major ? "ca-ng-grid major" : "ca-ng-grid",
        });
        ca_div_end();
    }

    for (int i = 0; i < nh; ++i) {
        float y = off_y + (float)(i - 1) * eff_gs;
        bool major = ((first_y + i) % NG_GRID_MAJOR_EVERY + NG_GRID_MAJOR_EVERY) % NG_GRID_MAJOR_EVERY == 0;
        ca_div_begin(&(Ca_DivDesc){
            .position = CA_POSITION_ABSOLUTE,
            .pos_x    = -eff_gs,
            .pos_y    = y,
            .width    = (float)(nv + 2) * eff_gs,
            .height   = NG_GRID_THICK,
            .no_hover = true,
            .style    = major ? "ca-ng-grid major" : "ca-ng-grid",
        });
        ca_div_end();
    }
}

/* Screen-space (canvas div local) position of a pin's dot centre. */
static NgPoint ng_pin_point(const Ca_NodeGraph *ng, const Ca_NgNodeState *state, int pin, bool output)
{
    float zs = ng->zoom;
    float x = output ? state->canvas_x + ng_node_w(state) - NG_PIN_PAD_R - NG_PIN_DOT_D * 0.5f
                     : state->canvas_x + NG_PIN_PAD_L + NG_PIN_DOT_D * 0.5f;
    return (NgPoint){ x * zs + ng->pan_x,
                      (state->canvas_y + ng_pin_y(state, pin, output)) * zs + ng->pan_y };
}

/* The wire carried by the cursor while a link drag is active, and the
   selection box. Both always emit the same divs, hidden when idle, so the
   canvas keeps a stable child count. */
static void ng_emit_interaction(Ca_NodeGraph *ng)
{
    float zs = ng->zoom;
    const Ca_NgNodeState *origin = ng->_link_active ? ca_node_graph_state(ng, ng->_link_node) : NULL;
    NgPoint cursor = { ng->_cursor_x * zs + ng->pan_x, ng->_cursor_y * zs + ng->pan_y };
    if (origin) {
        NgPoint pin = ng_pin_point(ng, origin, ng->_link_pin, ng->_link_output);
        NgPoint p0 = ng->_link_output ? pin : cursor;
        NgPoint p3 = ng->_link_output ? cursor : pin;
        float stub = NG_WIRE_STUB * zs;
        NgPoint route[4] = { p0, { p0.x + stub, p0.y }, { p0.x + stub, p3.y }, p3 };
        if (p3.x - p0.x >= 2.0f * stub) {
            float mid = 0.5f * (p0.x + p3.x);
            route[1] = (NgPoint){ mid, p0.y };
            route[2] = (NgPoint){ mid, p3.y };
        }
        emit_orthogonal_wire(ng, route, 4, NG_WIRE_RADIUS * zs, fmaxf(1.0f, zs) * NG_WIRE_ACTIVE_T,
                             0u, "ca-ng-link", NG_LINK_Z);
    } else {
        emit_orthogonal_wire(ng, NULL, 0, 0.0f, NG_WIRE_T, 0u, "ca-ng-link", NG_LINK_Z);
    }

    float x0 = fminf(ng->_box_x0, ng->_box_x1), x1 = fmaxf(ng->_box_x0, ng->_box_x1);
    float y0 = fminf(ng->_box_y0, ng->_box_y1), y1 = fmaxf(ng->_box_y0, ng->_box_y1);
    ca_div_begin(&(Ca_DivDesc){
        .hidden       = !ng->_box_active,
        .position     = CA_POSITION_ABSOLUTE,
        .pos_x        = x0 * zs + ng->pan_x,
        .pos_y        = y0 * zs + ng->pan_y,
        .width        = (x1 - x0) * zs,
        .height       = (y1 - y0) * zs,
        .border_width = 1.0f,
        .no_hover     = true,
        .style        = "ca-ng-box",
        .z_index      = NG_LINK_Z,
    });
    ca_div_end();
}

void ca_node_graph_end(Ca_NodeGraph *ng)
{
    assert(ng);
    /* Nodes the host stopped emitting leave the graph's geometry and selection. */
    for (int i = 0; i < ng->node_count; ++i) {
        Ca_NgNodeState *state = ca_node_graph_state(ng, i);
        if (!state || state->_epoch == ng->_build_epoch) continue;
        state->valid = false;
        state->selected = false;
        if (ng->selected_node == i) ng->selected_node = -1;
        if (ng->_link_active && (ng->_link_node == i || ng->_link_detached_node == i))
            ng->_link_active = false;
    }
    ng_emit_interaction(ng);
    ca_div_end(); /* canvas container */
    ng->_window = NULL;
}

void ca_ng_node_begin(Ca_NodeGraph *ng, const Ca_NgNodeDesc *desc)
{
    assert(ng && desc && desc->key);

    Ca_NgNodeState *state = ng_find_or_create(ng, desc->key, desc->x, desc->y);
    if (!state) return;
    state->valid  = true;
    state->_epoch = ng->_build_epoch;

    int idx  = state->_index;
    ng->_cur_node_idx = idx;
    ng->_cur_in_idx   = 0;
    ng->_cur_out_idx  = 0;

    bool selected = ng_node_selected(ng, state) || desc->selected;
    int  node_z   = selected ? NG_NODE_SELECTED_Z : 0;
    ng->_cur_node_z = node_z;

    float width = desc->width > 0.0f ? desc->width : NG_NODE_W;
    state->width = width;
    ng->_cur_width = width;
    ng->_cur_y = NG_HEADER_H + NG_BODY_PAD_V;

    float zs   = ng->zoom;
    float nw   = width * zs;
    float hdrh = NG_HEADER_H * zs;
    float nx   = state->canvas_x * zs + ng->pan_x;
    float ny   = state->canvas_y * zs + ng->pan_y;
    float radius = 5.0f * zs;
    float border_w = selected ? 2.0f : 1.0f;

    /* Node outer — all dimensions scaled by zoom.  Selected nodes get a real
       z-index, and every child emitted below inherits that same stack level. */
    ca_div_begin(&(Ca_DivDesc){
        .position        = CA_POSITION_ABSOLUTE,
        .pos_x           = nx,
        .pos_y           = ny,
        .width           = nw,
        .direction       = CA_VERTICAL,
        .corner_radius   = radius,
        .border_width    = border_w,
        .border_color    = desc->border_color,
        .shadow_offset_x = 2.0f,
        .shadow_offset_y = 3.0f,
        .shadow_blur     = 8.0f * zs,
        .style           = selected ? "ca-ng-node selected" : "ca-ng-node",
        .z_index         = node_z,
        .id              = desc->key,
        .on_drag_start   = node_drag_start,
        .on_drag         = node_drag,
        .on_drag_end     = node_drag_end,
        .drag_data       = state,
    });

    float header_x = border_w;
    float header_y = border_w;
    float header_w = fmaxf(0.0f, nw - border_w * 2.0f);
    float header_h = fmaxf(0.0f, hdrh - border_w);
    float fill_y   = header_y + fminf(radius, header_h);
    float fill_h   = fmaxf(0.0f, header_h - fminf(radius, header_h));

    /* Header — rounded top background plus square lower fill. */
    ca_div_begin(&(Ca_DivDesc){
        .width      = nw,
        .height     = hdrh,
        .direction  = CA_HORIZONTAL,
        .style      = "ca-ng-header",
        .inline_style = "align-items: center;",
        .z_index    = node_z,
    });
    ca_div_begin(&(Ca_DivDesc){
        .position      = CA_POSITION_ABSOLUTE,
        .pos_x         = header_x,
        .pos_y         = header_y,
        .width         = header_w,
        .height        = header_h,
        .background    = desc->header_color,
        .corner_radius = radius,
        .style         = "ca-ng-header-fill",
        .z_index       = node_z,
    });
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){
        .hidden     = fill_h < 0.5f,
        .position   = CA_POSITION_ABSOLUTE,
        .pos_x      = header_x,
        .pos_y      = fill_y,
        .width      = header_w,
        .height     = fill_h,
        .background = desc->header_color,
        .style      = "ca-ng-header-fill",
        .z_index    = node_z,
    });
    ca_div_end();

    float pad_l = border_w + NG_PIN_PAD_L * zs;
    float pad_r = border_w + NG_PIN_PAD_R * zs;
    ca_div_begin(&(Ca_DivDesc){
        .width        = nw,
        .height       = hdrh,
        .direction    = CA_HORIZONTAL,
        .inline_style = "align-items: center;",
        .padding      = {0.0f, pad_r, 0.0f, pad_l},
        .clip_content = true,
        .z_index      = node_z,
    });
    const char *title = desc->title ? desc->title : "";
    bool has_badge = desc->badge && desc->badge[0];
    if (ng->_show_title) {
        Ca_Label *label = ca_text(&(Ca_TextDesc){
            .text = title, .style = "ca-ng-title",
            .inline_style = has_badge ? "flex-grow: 1;" : NULL,
        });
        ng_set_label_z(label, node_z);
        if (has_badge) {
            Ca_Label *badge = ca_text(&(Ca_TextDesc){
                .text = desc->badge, .color = desc->badge_color, .style = "ca-ng-badge",
            });
            ng_set_label_z(badge, node_z);
        }
    } else {
        emit_text_stub(ng, title, ng->_title_font_px, nw - pad_l - pad_r,
                       NG_TITLE_STUB_T, 0u, "ca-ng-title-stub", node_z);
    }
    ca_div_end();
    ca_div_end(); /* header */

    /* Body — scaled padding */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .width     = nw,
        .padding   = {NG_BODY_PAD_V * zs, 0.0f, NG_BODY_PAD_V * zs, 0.0f},
        .gap       = 0.0f,
        .style     = "ca-ng-body",
        .z_index   = node_z,
    });
}

void ca_ng_node_end(Ca_NodeGraph *ng)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;

    /* Record final pin counts and size for wire routing and link drops */
    Ca_NgNodeState *state =
        ca_node_graph_state(ng, ng->_cur_node_idx);
    if (!state) return;
    state->input_count  = ng->_cur_in_idx;
    state->output_count = ng->_cur_out_idx;
    state->height       = ng->_cur_y + NG_BODY_PAD_V;

    ca_div_end(); /* body */
    ca_div_end(); /* node outer container */

    ng->_cur_node_idx = -1;
    ng->_cur_node_z   = 0;
}

/* One pin row: dot on the node edge, then the label or its stub line. */
static void ng_pin_row(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc, bool output)
{
    float       zs    = ng->zoom;
    float       nw    = ng->_cur_width * zs;
    float       dotd  = NG_PIN_DOT_D * zs;
    float       pad_l = NG_PIN_PAD_L * zs;
    float       pad_r = NG_PIN_PAD_R * zs;
    float       gap   = NG_PIN_GAP   * zs;
    uint32_t    color = desc ? desc->color : 0u;
    const char *label = (desc && desc->label) ? desc->label : "";
    int         z     = ng->_cur_node_z;
    int         idx   = output ? ng->_cur_out_idx : ng->_cur_in_idx;
    bool        open  = desc && desc->open;
    bool        links = ng->_on_link != NULL;

    Ca_NgNodeState *state = ca_node_graph_state(ng, ng->_cur_node_idx);
    float row_top = ng->_cur_y;
    if (state && idx < CA_NG_MAX_PINS) {
        (output ? state->out_y : state->in_y)[idx] = row_top + NG_PIN_ROW_H * 0.5f;
        Ca_NgPinHandle *handle = &state->_pins[output ? 1 : 0][idx];
        *handle = (Ca_NgPinHandle){ ng, ng->_cur_node_idx, idx, output };
    }
    ng->_cur_y += NG_PIN_ROW_H;

    /* A pin the wire being dragged would land on and the host accepts. */
    bool target = false;
    if (ng->_link_active && state && ng->_link_output != output && ng->_link_node != ng->_cur_node_idx) {
        float y = state->canvas_y + row_top + NG_PIN_ROW_H * 0.5f;
        bool near = fabsf(ng->_cursor_y - y) <= NG_PIN_ROW_H * 0.5f &&
                    ng->_cursor_x >= state->canvas_x - NG_DROP_REACH &&
                    ng->_cursor_x <= state->canvas_x + ng->_cur_width + NG_DROP_REACH;
        target = near && (output ? ng_can_link(ng, ng->_cur_node_idx, idx, ng->_link_node, ng->_link_pin)
                                 : ng_can_link(ng, ng->_link_node, ng->_link_pin, ng->_cur_node_idx, idx));
    }

    ca_div_begin(&(Ca_DivDesc){
        .direction    = CA_HORIZONTAL,
        .height       = NG_PIN_ROW_H * zs,
        .width        = nw,
        .style        = output ? "ca-ng-pin-row ca-ng-pin-row-out" : "ca-ng-pin-row",
        .inline_style = output ? "align-items: center; justify-content: flex-end;"
                               : "align-items: center;",
        .padding      = {0.0f, pad_r, 0.0f, pad_l},
        .gap          = gap,
        .clip_content = true,
        .z_index      = z,
    });
    Ca_DivDesc dot = {
        .width         = dotd,
        .height        = dotd,
        .background    = open ? 0u : color,
        .corner_radius = dotd * 0.5f,
        .border_width  = open ? 1.5f : 0.0f,
        .border_color  = open ? color : 0u,
        .style         = target ? "ca-ng-pin target" : (open ? "ca-ng-pin open" : "ca-ng-pin"),
        .z_index       = z,
    };
    if (!output) { ca_div_begin(&dot); ca_div_end(); }
    if (ng->_show_pin_text) {
        Ca_Label *text = ca_text(&(Ca_TextDesc){
            .text  = label,
            .color = color,
            .style = output ? "ca-ng-pin-label ca-ng-out-label" : "ca-ng-pin-label",
            .inline_style = output ? "flex-grow: 1; text-align: right;" : NULL,
        });
        ng_set_label_z(text, z);
    } else {
        emit_text_stub(ng, label, ng->_pin_font_px, nw - pad_l - pad_r - dotd - gap,
                       NG_PIN_STUB_T, color, "ca-ng-pin-stub", z);
    }
    if (output) { ca_div_begin(&dot); ca_div_end(); }
    if (links && state && idx < CA_NG_MAX_PINS) {
        /* Draggable area around the dot; smaller than the node, so it wins the drag. */
        float hit = NG_PIN_HIT_D * zs;
        float cx = output ? nw - pad_r - dotd * 0.5f : pad_l + dotd * 0.5f;
        ca_div_begin(&(Ca_DivDesc){
            .position      = CA_POSITION_ABSOLUTE,
            .pos_x         = cx - hit * 0.5f,
            .pos_y         = (NG_PIN_ROW_H * zs - hit) * 0.5f,
            .width         = hit,
            .height        = hit,
            .style         = "ca-ng-pin-hit",
            .z_index       = z,
            .on_drag_start = pin_drag_start,
            .on_drag       = pin_drag,
            .on_drag_end   = pin_drag_end,
            .drag_data     = &state->_pins[output ? 1 : 0][idx],
        });
        ca_div_end();
    }
    ca_div_end();
}

void ca_ng_input_pin(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;
    ng_pin_row(ng, desc, false);
    ++ng->_cur_in_idx;
}

void ca_ng_output_pin(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;

    /* Separator before first output (always, so it's visible at any zoom) */
    if (ng->_cur_out_idx == 0 && ng->_cur_in_idx > 0) {
        ca_div_begin(&(Ca_DivDesc){
            .height  = NG_GRID_THICK,
            .width   = ng->_cur_width * ng->zoom,
            .style   = "ca-ng-separator",
            .z_index = ng->_cur_node_z,
        });
        ca_div_end();
        ng->_cur_y += NG_GRID_THICK;
    }

    ng_pin_row(ng, desc, true);
    ++ng->_cur_out_idx;
}

bool ca_ng_row_begin(Ca_NodeGraph *ng, float height)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return false;
    float zs = ng->zoom;
    ca_div_begin(&(Ca_DivDesc){
        .direction    = CA_HORIZONTAL,
        .width        = ng->_cur_width * zs,
        .height       = height * zs,
        .padding      = {0.0f, NG_PIN_PAD_R * zs, 0.0f, NG_PIN_PAD_L * zs},
        .gap          = NG_PIN_GAP * zs,
        .clip_content = true,
        .style        = "ca-ng-row",
        .inline_style = "align-items: center;",
        .z_index      = ng->_cur_node_z,
    });
    ng->_cur_y += height;
    return zs >= NG_ROW_MIN_ZOOM;
}

void ca_ng_row_end(Ca_NodeGraph *ng)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;
    ca_div_end();
}

void ca_ng_wire(Ca_NodeGraph *ng, const Ca_NgWireDesc *desc)
{
    assert(ng);
    if (!desc || !desc->src_node || !desc->dst_node) return;

    Ca_NgNodeState *src = NULL, *dst = NULL;
    for (int i = 0; i < ng->node_count; ++i) {
        Ca_NgNodeState *state = ca_node_graph_state(ng, i);
        if (!state) continue;
        if (!src && strncmp(state->key, desc->src_node, CA_NG_KEY_LEN) == 0)
            src = state;
        if (!dst && strncmp(state->key, desc->dst_node, CA_NG_KEY_LEN) == 0)
            dst = state;
    }
    if (!src || !dst || !src->valid || !dst->valid) return;

    NgWireRecord record = { src->_index, desc->src_pin, dst->_index, desc->dst_pin };
    ca_dyn_array_push(&ng->_wires, &record);

    /* The wire a link drag has picked up is carried by the cursor instead. */
    if (ng->_link_active && ng->_link_detached_node == dst->_index &&
        ng->_link_detached_pin == desc->dst_pin) {
        emit_orthogonal_wire(ng, NULL, 0, 0.0f, NG_WIRE_T, 0u, "ca-ng-wire", 0);
        return;
    }

    float zs = ng->zoom;
    NgPoint p0 = ng_pin_point(ng, src, desc->src_pin, true);
    NgPoint p3 = ng_pin_point(ng, dst, desc->dst_pin, false);
    float stub = NG_WIRE_STUB * zs;
    NgPoint route[NG_WIRE_MAX_LEGS + 1];
    int route_count;
    if (p3.x - p0.x >= 2.0f * stub) {
        float mid = 0.5f * (p0.x + p3.x);
        route[0] = p0;
        route[1] = (NgPoint){ mid, p0.y };
        route[2] = (NgPoint){ mid, p3.y };
        route[3] = p3;
        route_count = 4;
    } else {
        /* Backward or overlapping: leave each pin by a stub, then cross on a
           lane that clears both nodes, above or below, whichever is shorter. */
        float src_top = src->canvas_y * zs + ng->pan_y;
        float dst_top = dst->canvas_y * zs + ng->pan_y;
        float above = fminf(src_top, dst_top) - NG_WIRE_CLEAR * zs;
        float below = fmaxf(src_top + ng_node_h(src) * zs,
                            dst_top + ng_node_h(dst) * zs) + NG_WIRE_CLEAR * zs;
        float lane = fabsf(p0.y - above) + fabsf(p3.y - above) <=
                     fabsf(p0.y - below) + fabsf(p3.y - below) ? above : below;
        route[0] = p0;
        route[1] = (NgPoint){ p0.x + stub, p0.y };
        route[2] = (NgPoint){ p0.x + stub, lane };
        route[3] = (NgPoint){ p3.x - stub, lane };
        route[4] = (NgPoint){ p3.x - stub, p3.y };
        route[5] = p3;
        route_count = 6;
    }

    bool active = ng_node_selected(ng, src) || ng_node_selected(ng, dst);
    bool any_selected = ng->selected_node >= 0 || ca_node_graph_selection_count(ng) > 0;
    bool dimmed = any_selected && !active;
    float thickness = fmaxf(1.0f, zs) * (active ? NG_WIRE_ACTIVE_T : NG_WIRE_T);
    const char *style = active ? "ca-ng-wire active"
                      : dimmed ? "ca-ng-wire dimmed" : "ca-ng-wire";

    emit_orthogonal_wire(ng, route, route_count, NG_WIRE_RADIUS * zs, thickness,
                         desc->color, style, 0);
}
