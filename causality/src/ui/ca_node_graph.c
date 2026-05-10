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
 *      Header     (row, ng-hdr CSS → align-items:center)
 *      Body       (column)
 *        Input rows   ●dot  label      (ng-hdr)
 *        Separator    1px hr
 *        Output rows  label  ●dot      (ng-hdr + ng-pin-row-out)
 *    Wire segments (absolute, multi-segment Manhattan routing)
 *
 * Zoom behaviour
 * ==============
 *  - All node/pin/header dimensions multiply by ng->zoom.
 *  - Grid effective spacing doubles when it would go below NG_GRID_MIN_SCR_PX.
 *  - Labels hidden below NG_ZOOM_HIDE_TEXT; pin rows hidden below NG_ZOOM_HIDE_PINS.
 *  - Node drag divides screen delta by zoom → correct canvas-space movement.
 *  - Backward wire connections route above both nodes (5-segment path).
 */

#include "ca_node_graph.h"
#include "ca_components.h"

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
#define NG_WIRE_STUB         36.0f  /* horizontal stub length from each pin */
#define NG_BACK_CLEAR        40.0f  /* clearance above nodes for backwards */
#define NG_GRID_SPACING      50.0f  /* logical px between grid lines       */
#define NG_GRID_THICK         1.0f  /* grid line visual thickness          */

/* Grid adaptivity */
#define NG_GRID_MIN_SCR_PX   20.0f  /* min screen px between lines        */
#define NG_GRID_MAX_LINES    40     /* hard cap per axis; 40+40+nodes+wires < CA_MAX_NODE_CHILDREN */

/* Level-of-detail thresholds */
#define NG_ZOOM_HIDE_TEXT    0.50f  /* below: hide pin labels + title      */
#define NG_ZOOM_HIDE_PINS    0.28f  /* below: hide all pin rows            */

/* ============================================================
   DEFAULT COLOURS
   ============================================================ */

#define NG_COL_BG           ca_color(0.09f, 0.09f, 0.11f, 1.00f)
#define NG_COL_GRID         ca_color(0.22f, 0.23f, 0.29f, 1.00f)
#define NG_COL_NODE_BG      ca_color(0.14f, 0.14f, 0.17f, 1.00f)
#define NG_COL_NODE_BORDER  ca_color(0.26f, 0.26f, 0.32f, 1.00f)
#define NG_COL_NODE_SEL     ca_color(0.45f, 0.55f, 1.00f, 1.00f)
#define NG_COL_HDR_DEFAULT  ca_color(0.24f, 0.26f, 0.38f, 1.00f)
#define NG_COL_TEXT         ca_color(0.87f, 0.87f, 0.91f, 1.00f)
#define NG_COL_TEXT_DIM     ca_color(0.55f, 0.55f, 0.62f, 1.00f)
#define NG_COL_PIN_DEFAULT  ca_color(0.72f, 0.72f, 0.78f, 1.00f)
#define NG_COL_WIRE         ca_color(0.50f, 0.52f, 0.65f, 1.00f)
#define NG_COL_SEP          ca_color(0.22f, 0.22f, 0.27f, 1.00f)

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
        if (strncmp(ng->nodes[i].key, key, CA_NG_KEY_LEN) == 0)
            return &ng->nodes[i];
    }

    if (ng->node_count >= CA_NG_MAX_NODES) return NULL;

    Ca_NgNodeState *state = &ng->nodes[ng->node_count++];
    snprintf(state->key, CA_NG_KEY_LEN, "%s", key);
    state->canvas_x = init_x;
    state->canvas_y = init_y;
    state->valid    = true;
    state->_ng      = ng;
    return state;
}

/* Centre-Y of an input pin relative to node top-left, in screen px. */
static float ng_input_pin_y(int pin_idx, float zoom)
{
    return (NG_HEADER_H + NG_BODY_PAD_V
            + (float)pin_idx * NG_PIN_ROW_H
            + NG_PIN_ROW_H * 0.5f) * zoom;
}

/* Centre-Y of an output pin relative to node top-left, in screen px.
 * sep_h = NG_GRID_THICK (1px) when there are inputs, otherwise 0. */
static float ng_output_pin_y(const Ca_NgNodeState *state, int pin_idx, float zoom)
{
    int   inp   = state->input_count;
    float sep_h = (inp > 0) ? NG_GRID_THICK : 0.0f;
    return (NG_HEADER_H + NG_BODY_PAD_V
            + (float)inp * NG_PIN_ROW_H + sep_h
            + (float)pin_idx * NG_PIN_ROW_H
            + NG_PIN_ROW_H * 0.5f) * zoom;
}

/* ============================================================
   WIRE SEGMENT PRIMITIVES
   ============================================================ */

static void emit_wire_h(float x, float y, float w, uint32_t color)
{
    if (w < 0.5f) return;
    ca_div_begin(&(Ca_DivDesc){
        .position   = CA_POSITION_ABSOLUTE,
        .pos_x      = x,
        .pos_y      = y - NG_WIRE_T * 0.5f,
        .width      = w,
        .height     = NG_WIRE_T,
        .background = color,
    });
    ca_div_end();
}

static void emit_wire_v(float x, float y, float h, uint32_t color)
{
    if (h < 0.5f) return;
    ca_div_begin(&(Ca_DivDesc){
        .position   = CA_POSITION_ABSOLUTE,
        .pos_x      = x - NG_WIRE_T * 0.5f,
        .pos_y      = y,
        .width      = NG_WIRE_T,
        .height     = h,
        .background = color,
    });
    ca_div_end();
}

/*
 * Multi-segment Manhattan routing.
 * Always exits source horizontally (right stub) and enters dest horizontally
 * (left stub).
 *
 * Forward  (lx ≤ rx): 3 segments — H-V-H midpoint between stubs.
 * Backward (lx >  rx): 5 segments — routes ABOVE both nodes to avoid overlap:
 *   out-stub  →  up  →  bridge-across  →  down  →  in-stub
 */
static void emit_route(float sx, float sy, float dx, float dy,
                       float stub, float zoom, uint32_t color)
{
    float lx = sx + stub;   /* right end of source stub  */
    float rx = dx - stub;   /* left  end of dest   stub  */
    float wt = NG_WIRE_T;

    if (lx <= rx) {
        /* ---- Forward: 3-segment H-V-H ---- */
        float mid = (lx + rx) * 0.5f;
        emit_wire_h(sx,  sy,           mid - sx,             color);
        if (fabsf(dy - sy) > 0.5f)
            emit_wire_v(mid, fminf(sy, dy), fabsf(dy - sy) + wt, color);
        emit_wire_h(mid, dy,           dx  - mid,            color);
    } else {
        /* ---- Backward: 5-segment, route above both nodes ---- */
        float clear = NG_BACK_CLEAR * zoom;
        float ry    = fminf(sy, dy) - clear;   /* bridge row Y */
        /* ry is always < min(sy,dy), so sy-ry and dy-ry are always > 0 */
        emit_wire_h(sx, sy, stub,          color);   /* right stub out */
        emit_wire_v(lx, ry, sy - ry + wt, color);   /* up to bridge   */
        emit_wire_h(rx, ry, lx - rx,      color);   /* bridge across  */
        emit_wire_v(rx, ry, dy - ry + wt, color);   /* down to dest   */
        emit_wire_h(rx, dy, stub,         color);   /* left stub in   */
    }
}

/* ============================================================
   DRAG CALLBACKS — canvas panning
   ============================================================ */

static void canvas_drag_start(const Ca_DragEvent *ev, void *ud)
{
    (void)ev;
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    ng->_pan_drag_start_x = ng->pan_x;
    ng->_pan_drag_start_y = ng->pan_y;
}

static void canvas_drag(const Ca_DragEvent *ev, void *ud)
{
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    ng->pan_x = ng->_pan_drag_start_x + ev->dx;
    ng->pan_y = ng->_pan_drag_start_y + ev->dy;
    if (ng->_host_div) ca_div_invalidate(ng->_host_div);
}

/* ============================================================
   DRAG CALLBACKS — node movement + selection
   ============================================================
   Causality picks the SMALLEST draggable div under the cursor, so
   node divs (smaller area) always win over the canvas background div.
   drag_fn_start fires on mouse-down, making it suitable for selection.
   ============================================================ */

static void node_drag_start(const Ca_DragEvent *ev, void *ud)
{
    (void)ev;
    Ca_NgNodeState *state = (Ca_NgNodeState *)ud;
    Ca_NodeGraph   *ng    = state->_ng;
    if (!ng) return;

    state->_drag_start_x = state->canvas_x;
    state->_drag_start_y = state->canvas_y;

    /* Selection — fires immediately on mouse-down */
    int idx = (int)(state - ng->nodes);
    ng->selected_node = idx;
    if (ng->_on_node_select)
        ng->_on_node_select(ng, idx, ng->_select_data);
    if (ng->_host_div) ca_div_invalidate(ng->_host_div);
}

static void node_drag(const Ca_DragEvent *ev, void *ud)
{
    Ca_NgNodeState *state = (Ca_NgNodeState *)ud;
    Ca_NodeGraph   *ng    = state->_ng;
    if (!ng) return;

    /* Convert screen-pixel delta to canvas-space delta by dividing by zoom.
     * This ensures the node moves exactly as far as the cursor, regardless
     * of current zoom level. */
    float inv = (ng->zoom > 0.001f) ? (1.0f / ng->zoom) : 1.0f;
    state->canvas_x = state->_drag_start_x + ev->dx * inv;
    state->canvas_y = state->_drag_start_y + ev->dy * inv;
    if (ng->_host_div) ca_div_invalidate(ng->_host_div);
}

/* ============================================================
   PUBLIC API
   ============================================================ */

static void canvas_scroll(double dx, double dy, void *ud)
{
    (void)dx;
    Ca_NodeGraph *ng = (Ca_NodeGraph *)ud;
    float factor = powf(1.08f, (float)dy);
    ng->zoom *= factor;
    if (ng->zoom < 0.15f) ng->zoom = 0.15f;
    if (ng->zoom > 4.0f)  ng->zoom = 4.0f;
    if (ng->_host_div) ca_div_invalidate(ng->_host_div);
}

void ca_node_graph_init(Ca_NodeGraph *ng)
{
    assert(ng);
    memset(ng, 0, sizeof(*ng));
    ng->selected_node  = -1;
    ng->_cur_node_idx  = -1;
    ng->zoom           = 1.0f;
}

void ca_node_graph_begin(Ca_NodeGraph *ng, Ca_Div *host_div,
                          const Ca_NodeGraphDesc *desc)
{
    assert(ng);
    ng->_host_div     = host_div;
    ng->_cur_node_idx = -1;
    ng->_cur_in_idx   = 0;
    ng->_cur_out_idx  = 0;

    /* Copy selection callback so node drag_start can invoke it */
    if (desc) {
        ng->_on_node_select = desc->on_node_select;
        ng->_select_data    = desc->select_data;
    }

    uint32_t bg   = (desc && desc->bg_color)   ? desc->bg_color   : NG_COL_BG;
    uint32_t grid = (desc && desc->grid_color)  ? desc->grid_color : NG_COL_GRID;
    float    w    = desc ? desc->width  : 0.0f;
    float    h    = desc ? desc->height : 0.0f;

    /* Canvas container — clips overflow, handles pan drag + scroll zoom */
    ca_div_begin(&(Ca_DivDesc){
        .width         = w,
        .height        = h,
        .background    = bg,
        .id            = desc ? desc->id    : NULL,
        .style         = desc ? desc->style : NULL,
        .on_drag_start = canvas_drag_start,
        .on_drag       = canvas_drag,
        .drag_data     = ng,
        .on_scroll     = canvas_scroll,
        .scroll_data   = ng,
    });

    /* --- Grid lines ---
     * Adapt grid spacing so lines are never closer than NG_GRID_MIN_SCR_PX on
     * screen (doubles each step).  Then compute how many lines cover ~3200 px
     * in each direction, capped at NG_GRID_MAX_LINES for performance. */
    float eff_gs = NG_GRID_SPACING * ng->zoom;
    while (eff_gs < NG_GRID_MIN_SCR_PX) eff_gs *= 2.0f;

    float off_x = fmodf(ng->pan_x, eff_gs);
    float off_y = fmodf(ng->pan_y, eff_gs);

    int nv = (int)(3200.0f / eff_gs) + 4;
    int nh = (int)(3200.0f / eff_gs) + 4;
    if (nv > NG_GRID_MAX_LINES) nv = NG_GRID_MAX_LINES;
    if (nh > NG_GRID_MAX_LINES) nh = NG_GRID_MAX_LINES;

    for (int i = 0; i < nv; ++i) {
        float x = off_x + (float)(i - 1) * eff_gs;
        ca_div_begin(&(Ca_DivDesc){
            .position   = CA_POSITION_ABSOLUTE,
            .pos_x      = x,
            .pos_y      = -eff_gs,
            .width      = NG_GRID_THICK,
            .height     = (float)(nh + 2) * eff_gs,
            .background = grid,
        });
        ca_div_end();
    }

    for (int i = 0; i < nh; ++i) {
        float y = off_y + (float)(i - 1) * eff_gs;
        ca_div_begin(&(Ca_DivDesc){
            .position   = CA_POSITION_ABSOLUTE,
            .pos_x      = -eff_gs,
            .pos_y      = y,
            .width      = (float)(nv + 2) * eff_gs,
            .height     = NG_GRID_THICK,
            .background = grid,
        });
        ca_div_end();
    }
}

void ca_node_graph_end(Ca_NodeGraph *ng)
{
    assert(ng);
    ca_div_end(); /* canvas container */
}

void ca_ng_node_begin(Ca_NodeGraph *ng, const Ca_NgNodeDesc *desc)
{
    assert(ng && desc && desc->key);

    Ca_NgNodeState *state = ng_find_or_create(ng, desc->key, desc->x, desc->y);
    if (!state) return;

    int idx  = (int)(state - ng->nodes);
    ng->_cur_node_idx = idx;
    ng->_cur_in_idx   = 0;
    ng->_cur_out_idx  = 0;

    bool selected = (idx == ng->selected_node) || desc->selected;

    float zs   = ng->zoom;
    float nw   = NG_NODE_W   * zs;
    float hdrh = NG_HEADER_H * zs;
    float nx   = state->canvas_x * zs + ng->pan_x;
    float ny   = state->canvas_y * zs + ng->pan_y;

    uint32_t hdr_col    = desc->header_color ? desc->header_color : NG_COL_HDR_DEFAULT;
    uint32_t border_col = selected ? NG_COL_NODE_SEL : NG_COL_NODE_BORDER;

    /* Node outer — all dimensions scaled by zoom */
    ca_div_begin(&(Ca_DivDesc){
        .position        = CA_POSITION_ABSOLUTE,
        .pos_x           = nx,
        .pos_y           = ny,
        .width           = nw,
        .background      = NG_COL_NODE_BG,
        .direction       = CA_VERTICAL,
        .corner_radius   = 5.0f * zs,
        .border_width    = selected ? 2.0f : 1.0f,
        .border_color    = border_col,
        .shadow_offset_x = 2.0f,
        .shadow_offset_y = 3.0f,
        .shadow_blur     = 8.0f * zs,
        .shadow_color    = ca_color(0.0f, 0.0f, 0.0f, 0.45f),
        .id              = desc->key,
        .z_index         = selected ? 1 : 0,
        .on_drag_start   = node_drag_start,
        .on_drag         = node_drag,
        .drag_data       = state,
    });

    /* Header — vertically centred via ng-hdr (align-items:center) */
    ca_div_begin(&(Ca_DivDesc){
        .width      = nw,
        .height     = hdrh,
        .background = hdr_col,
        .direction  = CA_HORIZONTAL,
        .style      = "ng-hdr",
        .padding    = {0.0f, NG_PIN_PAD_R * zs, 0.0f, NG_PIN_PAD_L * zs},
    });
    if (zs >= NG_ZOOM_HIDE_TEXT)
        ca_text(&(Ca_TextDesc){ .text = desc->title, .color = 0xFFFFFFFFu, .style = "ng-node-title" });
    ca_div_end(); /* header */

    /* Body — scaled padding */
    ca_div_begin(&(Ca_DivDesc){
        .direction = CA_VERTICAL,
        .width     = nw,
        .padding   = {NG_BODY_PAD_V * zs, 0.0f, NG_BODY_PAD_V * zs, 0.0f},
        .gap       = 0.0f,
    });
}

void ca_ng_node_end(Ca_NodeGraph *ng)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;

    /* Record final pin counts for wire routing on subsequent frames */
    Ca_NgNodeState *state = &ng->nodes[ng->_cur_node_idx];
    state->input_count  = ng->_cur_in_idx;
    state->output_count = ng->_cur_out_idx;

    ca_div_end(); /* body */
    ca_div_end(); /* node outer container */

    ng->_cur_node_idx = -1;
}

void ca_ng_input_pin(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;

    float    zs      = ng->zoom;
    float    nw      = NG_NODE_W * zs;
    float    rowh    = NG_PIN_ROW_H * zs;
    uint32_t dot_col = (desc && desc->color) ? desc->color : NG_COL_PIN_DEFAULT;

    if (zs >= NG_ZOOM_HIDE_TEXT) {
        /* Full pin row: ●dot  label */
        float dotd = NG_PIN_DOT_D * zs;
        float padl = NG_PIN_PAD_L * zs;
        float padr = NG_PIN_PAD_R * zs;
        float gap  = NG_PIN_GAP   * zs;
        const char *lbl = (desc && desc->label) ? desc->label : "";

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .height    = rowh,
            .width     = nw,
            .style     = "ng-hdr",
            .padding   = {0.0f, padr, 0.0f, padl},
            .gap       = gap,
        });
        ca_div_begin(&(Ca_DivDesc){
            .width         = dotd,
            .height        = dotd,
            .background    = dot_col,
            .corner_radius = dotd * 0.5f,
        });
        ca_div_end();
        ca_text(&(Ca_TextDesc){ .text = lbl, .color = dot_col, .style = "ng-pin-label" });
        ca_div_end();
    } else {
        /* Stub row: thin placeholder line so the body always has flow-children
         * and Causality's auto-height shrinks the node correctly. */
        float padl  = NG_PIN_PAD_L * zs;
        float stubw = nw * 0.55f;
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .height    = rowh,
            .width     = nw,
            .style     = "ng-hdr",
            .padding   = {0.0f, 0.0f, 0.0f, padl},
        });
        ca_div_begin(&(Ca_DivDesc){
            .width         = stubw,
            .height        = 2.0f,
            .background    = dot_col,
            .corner_radius = 1.0f,
        });
        ca_div_end();
        ca_div_end();
    }

    ++ng->_cur_in_idx;
}

void ca_ng_output_pin(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc)
{
    assert(ng);
    if (ng->_cur_node_idx < 0) return;

    float    zs      = ng->zoom;
    float    nw      = NG_NODE_W * zs;
    float    rowh    = NG_PIN_ROW_H * zs;
    uint32_t dot_col = (desc && desc->color) ? desc->color : NG_COL_PIN_DEFAULT;

    /* Separator before first output (always, so it's visible at any zoom) */
    if (ng->_cur_out_idx == 0 && ng->_cur_in_idx > 0) {
        ca_div_begin(&(Ca_DivDesc){
            .height     = NG_GRID_THICK,
            .width      = nw,
            .background = NG_COL_SEP,
        });
        ca_div_end();
    }

    if (zs >= NG_ZOOM_HIDE_TEXT) {
        /* Full pin row: label  ●dot — right-aligned */
        float dotd = NG_PIN_DOT_D * zs;
        float padl = NG_PIN_PAD_L * zs;
        float padr = NG_PIN_PAD_R * zs;
        float gap  = NG_PIN_GAP   * zs;
        const char *lbl = (desc && desc->label) ? desc->label : "";

        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .height    = rowh,
            .width     = nw,
            .style     = "ng-hdr ng-pin-row-out",
            .padding   = {0.0f, padr, 0.0f, padl},
            .gap       = gap,
        });
        ca_text(&(Ca_TextDesc){ .text = lbl, .color = dot_col, .style = "ng-pin-label ng-out-label" });
        ca_div_begin(&(Ca_DivDesc){
            .width         = dotd,
            .height        = dotd,
            .background    = dot_col,
            .corner_radius = dotd * 0.5f,
        });
        ca_div_end();
        ca_div_end();
    } else {
        /* Stub row: thin placeholder line, right-aligned */
        float padr  = NG_PIN_PAD_R * zs;
        float stubw = nw * 0.55f;
        ca_div_begin(&(Ca_DivDesc){
            .direction = CA_HORIZONTAL,
            .height    = rowh,
            .width     = nw,
            .style     = "ng-hdr ng-pin-row-out",
            .padding   = {0.0f, padr, 0.0f, 0.0f},
        });
        ca_div_begin(&(Ca_DivDesc){
            .width         = stubw,
            .height        = 2.0f,
            .background    = dot_col,
            .corner_radius = 1.0f,
        });
        ca_div_end();
        ca_div_end();
    }

    ++ng->_cur_out_idx;
}

void ca_ng_wire(Ca_NodeGraph *ng, const Ca_NgWireDesc *desc)
{
    assert(ng);
    if (!desc || !desc->src_node || !desc->dst_node) return;

    Ca_NgNodeState *src = NULL, *dst = NULL;
    for (int i = 0; i < ng->node_count; ++i) {
        if (!src && strncmp(ng->nodes[i].key, desc->src_node, CA_NG_KEY_LEN) == 0)
            src = &ng->nodes[i];
        if (!dst && strncmp(ng->nodes[i].key, desc->dst_node, CA_NG_KEY_LEN) == 0)
            dst = &ng->nodes[i];
    }
    if (!src || !dst) return;

    uint32_t color = desc->color ? desc->color : NG_COL_WIRE;
    float    zs    = ng->zoom;
    float    stub  = NG_WIRE_STUB * zs;

    /* Output dot centre (right edge of src node) — matches inline dot position */
    float sx = (src->canvas_x + NG_NODE_W - NG_PIN_PAD_R - NG_PIN_DOT_D * 0.5f) * zs
               + ng->pan_x;
    float sy = src->canvas_y * zs + ng->pan_y
               + ng_output_pin_y(src, desc->src_pin, zs);

    /* Input dot centre (left edge of dst node) */
    float dx = (dst->canvas_x + NG_PIN_PAD_L + NG_PIN_DOT_D * 0.5f) * zs
               + ng->pan_x;
    float dy = dst->canvas_y * zs + ng->pan_y
               + ng_input_pin_y(desc->dst_pin, zs);

    emit_route(sx, sy, dx, dy, stub, zs, color);
}
