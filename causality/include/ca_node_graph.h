// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/*
 * ca_node_graph.h — Reusable interactive node graph component.
 *
 * Builds a node graph canvas entirely from Causality div primitives:
 * absolute-positioned node divs, pin dot divs, and rounded orthogonal wire
 * segment divs. No raw GPU rendering required.
 *
 * USAGE (inside a ca_div_set_builder callback):
 *
 *   static Ca_NodeGraph g_ng;
 *
 *   // One-time setup:
 *   ca_node_graph_init(&g_ng);
 *
 *   // In your builder function (the builder div is already the current parent):
 *   void my_builder(Ca_Div *div, void *ud) {
 *       Ca_NodeGraph *ng = ud;
 *
 *       ca_node_graph_begin(ng, div, &(Ca_NodeGraphDesc){0});
 *
 *       ca_ng_node_begin(ng, &(Ca_NgNodeDesc){
 *           .key = "my_node", .title = "My Node", .x = 50, .y = 80,
 *           .header_color = ca_color(0.2,0.3,0.5,1),
 *       });
 *           ca_ng_input_pin(ng,  &(Ca_NgPinDesc){ .label = "color_in" });
 *           ca_ng_output_pin(ng, &(Ca_NgPinDesc){ .label = "color_out" });
 *       ca_ng_node_end(ng);
 *
 *       ca_ng_wire(ng, &(Ca_NgWireDesc){
 *           .src_node = "prev_node", .src_pin = 0,
 *           .dst_node = "my_node",   .dst_pin = 0,
 *       });
 *
 *       ca_node_graph_end(ng);
 *   }
 *
 * The host_div passed to ca_node_graph_begin is the stable div whose builder
 * is invalidated whenever node positions or pan change (drag interactions).
 * Pass NULL if your frame callback already runs every tick (non-reactive mode).
 *
 * STYLING: every part carries a CSS class whose system-sheet defaults read the
 * --ca-* palette, so themes apply without code. Non-zero descriptor colours win.
 *   .ca-ng-canvas  .ca-ng-grid(.major)  .ca-ng-node(.selected, :hover)
 *   .ca-ng-header  .ca-ng-header-fill  .ca-ng-title  .ca-ng-title-stub
 *   .ca-ng-body  .ca-ng-pin-row(.ca-ng-pin-row-out)  .ca-ng-pin
 *   .ca-ng-pin-label  .ca-ng-out-label  .ca-ng-pin-stub  .ca-ng-separator
 *   .ca-ng-wire(.active = touches the selected node, .dimmed = others)
 * Labels keep the CSS font size at every zoom; when a zoomed row is too short
 * for that font's line height, its text is drawn as a stub line instead.
 *
 * INTERACTION: drag the canvas to pan, scroll to zoom about the cursor, drag
 * a node to move and select it, click empty canvas to clear the selection.
 */

#pragma once

#include "causality.h"
#include "ca_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   LIMITS
   ============================================================ */

#define CA_NG_KEY_LEN     64

/* ============================================================
   PER-NODE PERSISTENT STATE
   (zero-initialise the containing Ca_NodeGraph at startup)
   ============================================================ */

typedef struct Ca_NodeGraph Ca_NodeGraph;

typedef struct Ca_NgNodeState {
    char  key[CA_NG_KEY_LEN]; /* stable unique identifier */
    float canvas_x, canvas_y; /* current position in canvas space */
    float _drag_start_x, _drag_start_y; /* position at drag start */
    int   input_count;        /* recorded after first build */
    int   output_count;
    bool  valid;
    Ca_NodeGraph *_ng;        /* back-pointer to owning graph (set on first use) */
    int   _index;
} Ca_NgNodeState;

/* ============================================================
   MAIN STATE STRUCT  (user-allocated, zero-init at startup)
   ============================================================ */

struct Ca_NodeGraph {
    float pan_x, pan_y;                  /* current canvas pan */
    float zoom;                          /* canvas zoom factor (1.0 = 100%) */
    float _pan_drag_start_x;             /* pan at canvas drag start */
    float _pan_drag_start_y;
    Ca_DynArray _nodes;                  /* stable Ca_NgNodeState pointers */
    int   node_count;
    int   selected_node;                 /* index into nodes[], -1 = none */

    Ca_Div *_host_div;                   /* stable div to invalidate on state change */
    Ca_Div *_canvas;                     /* canvas div from the latest build */
    float   _canvas_w, _canvas_h;        /* canvas logical size (0 until first layout) */
    bool    _fit_pending;                /* frame all nodes once the canvas is sized */

    /* Selection callback (copied from Ca_NodeGraphDesc on each begin call) */
    void (*_on_node_select)(Ca_NodeGraph *ng, int node_idx, void *user_data);
    void  *_select_data;

    /* Build-time transients (reset per begin/end cycle) */
    Ca_Window *_window;
    float _title_font_px;                /* CSS font size of titles (0 = font default) */
    float _pin_font_px;                  /* CSS font size of pin labels (0 = font default) */
    bool  _show_title;                   /* zoomed header fits the title line height */
    bool  _show_pin_text;                /* zoomed pin row fits the label line height */
    int   _cur_node_idx;
    int   _cur_in_idx;
    int   _cur_out_idx;
    int   _cur_node_z;
};

/* ============================================================
   DESCRIPTORS
   ============================================================ */

typedef struct Ca_NodeGraphDesc {
    float       width, height;   /* canvas size — 0 = fill parent */
    const char *id;              /* CSS id for the canvas container */
    const char *style;           /* extra CSS class(es) for the canvas, after ca-ng-canvas */

    /* Called when a node is selected (mouse-down on it) or the selection is
       cleared by clicking empty canvas (node_idx = -1). node_idx can be
       resolved with ca_node_graph_state. Pass NULL to ignore. */
    void (*on_node_select)(Ca_NodeGraph *ng, int node_idx, void *user_data);
    void  *select_data;
} Ca_NodeGraphDesc;

typedef struct Ca_NgNodeDesc {
    const char *key;             /* stable unique id — must not change across frames */
    const char *title;           /* displayed in the header bar */
    float       x, y;           /* initial canvas position (used only on first appearance) */
    uint32_t    header_color;   /* 0 = .ca-ng-header-fill */
    bool        selected;        /* force-select visual highlight */
} Ca_NgNodeDesc;

typedef struct Ca_NgPinDesc {
    const char *label;           /* displayed beside the pin dot */
    uint32_t    color;           /* dot, label and stub colour (0 = CSS) */
} Ca_NgPinDesc;

typedef struct Ca_NgWireDesc {
    const char *src_node;        /* key of the source node */
    int         src_pin;         /* output pin index on source node */
    const char *dst_node;        /* key of the destination node */
    int         dst_pin;         /* input pin index on destination node */
    uint32_t    color;           /* wire colour (0 = .ca-ng-wire) */
} Ca_NgWireDesc;

/* ============================================================
   API
   ============================================================ */

/* Zero-initialises the Ca_NodeGraph struct and resets selected_node to -1.
   Call once before the first ca_node_graph_begin. */
CA_API bool ca_node_graph_init(Ca_NodeGraph *ng);

/** Releases all node state owned by a graph and resets it for reuse. */
CA_API void ca_node_graph_destroy(Ca_NodeGraph *ng);

/** Returns persistent state for a node index, or NULL when out of range. */
CA_API Ca_NgNodeState *ca_node_graph_state(Ca_NodeGraph *ng, int node_idx);

/** Finds or creates persistent graph-node state for a stable key. */
CA_API Ca_NgNodeState *ca_node_graph_add_state(Ca_NodeGraph *ng,
                                                const char *key,
                                                float initial_x,
                                                float initial_y);

/* Frames every node in the canvas (zoom capped at 100%) on the next build
   once the canvas has a laid-out size, and invalidates the host div. */
CA_API void ca_node_graph_request_fit(Ca_NodeGraph *ng);

/* Begin building the node graph canvas.
   host_div: the stable Ca_Div whose builder will be invalidated when node
             positions or pan change.  Pass NULL if no reactive invalidation
             is needed (e.g. the frame callback already runs every tick).
   Creates a canvas child div inside the current parent. */
CA_API void ca_node_graph_begin(Ca_NodeGraph *ng,
                                Ca_Div *host_div,
                                const Ca_NodeGraphDesc *desc);

/* Finalise the canvas and close all pending divs. */
CA_API void ca_node_graph_end(Ca_NodeGraph *ng);

/* Begin a node.  Must be called between ca_node_graph_begin and
   ca_node_graph_end.  Nested with ca_ng_node_end. */
CA_API void ca_ng_node_begin(Ca_NodeGraph *ng, const Ca_NgNodeDesc *desc);

/* Close the current node.  Must match every ca_ng_node_begin. */
CA_API void ca_ng_node_end(Ca_NodeGraph *ng);

/* Add an input pin row inside the current node (between begin and end). */
CA_API void ca_ng_input_pin(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc);

/* Add an output pin row inside the current node (between begin and end). */
CA_API void ca_ng_output_pin(Ca_NodeGraph *ng, const Ca_NgPinDesc *desc);

/* Draw a wire between two nodes.  Call after all ca_ng_node_end calls but
   still inside the same ca_node_graph_begin / ca_node_graph_end block. */
CA_API void ca_ng_wire(Ca_NodeGraph *ng, const Ca_NgWireDesc *desc);

#ifdef __cplusplus
}
#endif
