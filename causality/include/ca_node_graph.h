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
 * Shift/Ctrl-click a node toggles it in the selection; Shift-drag on empty
 * canvas box-selects (adding to the selection); dragging a selected node moves
 * the whole selection.
 *
 * LINKS: with Ca_NodeGraphDesc.on_link set, pins are draggable. Dragging from
 * an output pin, or from an unconnected input pin, draws a wire that follows
 * the cursor; dropping it on a pin of the other kind (accepted by can_link)
 * reports the link. Dragging from a connected input pin picks the wire up from
 * its source. The component stores no links: the host owns them and redraws
 * them with ca_ng_wire on the next build.
 *
 * ROWS: ca_ng_row_begin/end open a fixed-height row inside a node for host
 * widgets (value fields, pickers).
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
#define CA_NG_MAX_PINS    16   /* inputs or outputs per node that can be linked */

/* ============================================================
   PER-NODE PERSISTENT STATE
   (zero-initialise the containing Ca_NodeGraph at startup)
   ============================================================ */

typedef struct Ca_NodeGraph Ca_NodeGraph;

/* A pin named by its node key. */
typedef struct Ca_NgPinRef {
    const char *node;
    int         pin;
} Ca_NgPinRef;

/* A wire released by a link drag. */
typedef struct Ca_NgLinkEvent {
    Ca_NgPinRef from;       /* output pin the wire leaves; node NULL when none was hit */
    Ca_NgPinRef to;         /* input pin it was dropped on; node NULL on empty canvas */
    Ca_NgPinRef detached;   /* input pin the wire was picked up from; node NULL for a new wire */
    float       canvas_x, canvas_y; /* drop position in canvas space */
} Ca_NgLinkEvent;


/* Drag context of one pin (internal). */
typedef struct Ca_NgPinHandle {
    Ca_NodeGraph *ng;
    int  node;                /* index into the graph's nodes */
    int  pin;
    bool output;
} Ca_NgPinHandle;

typedef struct Ca_NgNodeState {
    char  key[CA_NG_KEY_LEN]; /* stable unique identifier */
    float canvas_x, canvas_y; /* current position in canvas space */
    float _drag_start_x, _drag_start_y; /* position at drag start */
    int   input_count;        /* recorded after first build */
    int   output_count;
    float width, height;      /* logical size recorded by the latest build */
    float in_y[CA_NG_MAX_PINS];  /* pin centres below the node top, logical px */
    float out_y[CA_NG_MAX_PINS];
    bool  valid;              /* emitted by the latest build */
    bool  selected;           /* member of the multi-selection */
    uint32_t _epoch;          /* build that last emitted this node */
    Ca_NodeGraph *_ng;        /* back-pointer to owning graph (set on first use) */
    int   _index;
    Ca_NgPinHandle _pins[2][CA_NG_MAX_PINS]; /* [0] inputs, [1] outputs */
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

    /* Callbacks (copied from Ca_NodeGraphDesc on each begin call) */
    void (*_on_node_select)(Ca_NodeGraph *ng, int node_idx, void *user_data);
    void  *_select_data;
    void (*_on_link)(Ca_NodeGraph *ng, const Ca_NgLinkEvent *event, void *user_data);
    bool (*_can_link)(Ca_NodeGraph *ng, const Ca_NgPinRef *from,
                      const Ca_NgPinRef *to, void *user_data);
    void  *_link_data;
    void (*_on_nodes_moved)(Ca_NodeGraph *ng, void *user_data);
    void  *_moved_data;

    Ca_DynArray _wires;                  /* wires of the latest build (Ca_NgWireRecord) */
    uint32_t _build_epoch;               /* counts builds; nodes not emitted in one are dropped */

    /* Link drag in progress */
    bool  _link_active;
    int   _link_node, _link_pin;         /* pin the wire hangs from */
    bool  _link_output;                  /* that pin is an output */
    int   _link_detached_node, _link_detached_pin; /* input the wire was picked from, or -1 */
    float _cursor_x, _cursor_y;          /* cursor in canvas space */

    /* Box selection in progress */
    bool  _box_active;
    float _box_x0, _box_y0, _box_x1, _box_y1; /* canvas space */

    float _moved_distance;               /* largest node drag delta of the current gesture */
    bool  _drag_additive;                /* the node gesture began with shift/ctrl */
    float _cur_y;                        /* logical y below the node top during a build */
    float _cur_width;                    /* logical width of the node being built */

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

    /* A wire was released. `event->from` is NULL-noded when the drag started on
       an unconnected input and found no output (reverse drags report the
       output they landed on). Setting this makes pins draggable. */
    void (*on_link)(Ca_NodeGraph *ng, const Ca_NgLinkEvent *event, void *user_data);
    /* Whether an output pin may feed an input pin; drives the drop-target
       highlight and filters drops. NULL accepts every pairing. */
    bool (*can_link)(Ca_NodeGraph *ng, const Ca_NgPinRef *from, const Ca_NgPinRef *to,
                     void *user_data);
    void  *link_data;
    /* Nodes were moved by a drag that ended; read positions from the node states. */
    void (*on_nodes_moved)(Ca_NodeGraph *ng, void *user_data);
    void  *moved_data;

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
    float       width;          /* logical width; 0 = default */
    uint32_t    header_color;   /* 0 = .ca-ng-header-fill */
    uint32_t    border_color;   /* 0 = CSS; set to flag an error */
    const char *badge;          /* short right-aligned header text, or NULL */
    uint32_t    badge_color;    /* 0 = CSS */
    bool        selected;        /* force-select visual highlight */
} Ca_NgNodeDesc;

typedef struct Ca_NgPinDesc {
    const char *label;           /* displayed beside the pin dot */
    uint32_t    color;           /* dot, label and stub colour (0 = CSS) */
    bool        open;            /* nothing reaches this pin: the dot is drawn hollow */
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

/* Opens a fixed-height row inside the current node for host widgets. Returns
   false while the zoom is too small for widgets to keep their size; the row is
   still emitted (empty) and must be closed with ca_ng_row_end. */
CA_API bool ca_ng_row_begin(Ca_NodeGraph *ng, float height);
CA_API void ca_ng_row_end(Ca_NodeGraph *ng);

/* ---- Selection ---- */

CA_API bool ca_node_graph_is_selected(const Ca_NodeGraph *ng, int node_idx);
CA_API int  ca_node_graph_selection_count(const Ca_NodeGraph *ng);

/* Selects one node, replacing the selection unless `additive`. -1 clears. */
CA_API void ca_node_graph_select(Ca_NodeGraph *ng, int node_idx, bool additive);
CA_API void ca_node_graph_clear_selection(Ca_NodeGraph *ng);

/* ---- View ---- */

/* Canvas-space point at the centre of the visible canvas. */
CA_API bool ca_node_graph_view_center(const Ca_NodeGraph *ng, float *out_x, float *out_y);

/* Canvas-space point under a window-space point (e.g. a context menu's
   screen position); false before the first layout. */
CA_API bool ca_node_graph_screen_to_canvas(const Ca_NodeGraph *ng, float screen_x, float screen_y,
                                           float *out_x, float *out_y);

/* Selects every node the latest build emitted. */
CA_API void ca_node_graph_select_all(Ca_NodeGraph *ng);

/* Whether the cursor is over the canvas. */
CA_API bool ca_node_graph_hovered(const Ca_NodeGraph *ng);

#ifdef __cplusplus
}
#endif
