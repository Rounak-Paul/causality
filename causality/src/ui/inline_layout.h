// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* inline_layout.h — internal header for the inline formatting context
   (Ca_DivDesc.inline_flow — see causality.h for the public contract).

   A sibling to layout.c's flexbox pass, not a replacement: a div with
   inline_flow set skips flex-line building entirely and instead packs
   its direct children (text-bearing labels and other inline-flagged
   divs) word-by-word onto lines that wrap at the container's width,
   mirroring how mixed inline content wraps inside an HTML paragraph. */
#pragma once

#include "ca_internal.h"

/* One resolved word/run placement within an inline container, produced
   by ca_inline_layout() and consumed by ca_inline_paint(). A "run" is
   either one word from a text-bearing child's string, or one whole
   non-text inline child treated as an unbreakable box (e.g. a small
   fixed-size image inline in text) — never split across lines. */
typedef struct Ca_InlineRun {
    Ca_Node    *child;       /* owning child node (for font/color lookup) */
    const char *text_start;  /* pointer into the child's own text buffer;
                                 NULL for a non-text child run */
    uint32_t    text_len;    /* byte length of this run's text slice      */
    float       x, y;        /* resolved position, relative to the
                                 container's content-box origin           */
    float       w, h;        /* resolved box size for this run            */
    uint32_t    line_index;  /* which wrapped line this run landed on     */
    float       baseline_y;  /* resolved baseline within the run's line,
                                 relative to the container's content-box
                                 origin — text runs paint here; a
                                 non-text run's box uses y/h instead      */
} Ca_InlineRun;

/* Resolved layout for one inline_flow container: every run in document
   order, plus the container's total content height (line_height ×
   line_count + inter-line gaps) so the caller can auto-size the
   container the same way content_size() does for flexbox children.
   Owned by the Ca_Node that produced it (see Ca_Node.inline_runs in
   ca_internal.h) — freed/regrown alongside normal layout invalidation,
   never held past the layout pass that produced it by any external
   caller. */
typedef struct Ca_InlineLayout {
    Ca_InlineRun *runs;
    uint32_t      run_count;
    uint32_t      run_capacity;
    uint32_t      line_count;
    float         content_h;
} Ca_InlineLayout;

/**
 * Lays out an inline_flow container's direct children as a wrapped
 * inline formatting context, writing the result into node->inline_layout
 * (growing/reusing its run array across frames).
 *
 * Text-bearing children (CA_WIDGET_LABEL with non-empty text) are split
 * into word runs by the same whitespace/newline rules as the existing
 * flexbox word-wrap (layout.c's measure_wrapped_text_height /
 * paint.c's paint_text_wrapped); other visible, in-flow children are
 * treated as one unbreakable inline box each, sized by their own
 * explicit width/height (0 defaults to a small fixed box — inline_flow
 * children are not expected to carry complex nested layouts of their
 * own in this first implementation).
 *
 * node       Container with desc.inline_flow set; its children are read,
 *            not mutated (positions are recorded in the Ca_InlineLayout,
 *            not written back to each child's own x/y/w/h — a child
 *            consumed into an inline run is not independently painted
 *            by the normal per-node paint dispatch; see
 *            paint_node_content's inline_flow branch in paint.c).
 * content_w  Available width (already reduced by the container's own
 *            padding) to wrap lines within.
 */
void ca_inline_layout(Ca_Node *node, float content_w);

/* Draw-command emission for a resolved inline layout (ca_inline_paint) is
   implemented directly in paint.c, not here — it needs paint.c's
   file-local ClipRect type and draw-command helpers (set_clip,
   ca_window_reserve_draw_commands), the same way every other text-paint
   helper (paint_text, paint_text_wrapped) already lives there rather than
   in a separate translation unit. This header only owns measurement/
   layout, which has no paint-side dependencies. */

/** Releases an inline layout's run array. Called from node destruction
    alongside the node's other owned buffers. Safe to call on a
    zero-initialized Ca_InlineLayout. */
void ca_inline_layout_destroy(Ca_InlineLayout *layout);
