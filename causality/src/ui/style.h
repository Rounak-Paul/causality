// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* style.h — CSS style resolution and cascade */
#pragma once

#include "css.h"
#include "ca_internal.h"

/* ============================================================
   ELEMENT TYPES — for CSS selector matching
   ============================================================ */

typedef enum {
    CA_ELEM_DIV = 0,
    CA_ELEM_TEXT,
    CA_ELEM_BUTTON,
    CA_ELEM_INPUT,
    CA_ELEM_H1, CA_ELEM_H2, CA_ELEM_H3,
    CA_ELEM_H4, CA_ELEM_H5, CA_ELEM_H6,
    CA_ELEM_HR,
    CA_ELEM_SPACER,
    CA_ELEM_LIST,
    CA_ELEM_LI,
    CA_ELEM_CHECKBOX,
    CA_ELEM_RADIO,
    CA_ELEM_SLIDER,
    CA_ELEM_TOGGLE,
    CA_ELEM_PROGRESS,
    CA_ELEM_SELECT,
    CA_ELEM_TABBAR,
    CA_ELEM_TAB,
    CA_ELEM_TREE,
    CA_ELEM_TREENODE,
    CA_ELEM_TABLE,
    CA_ELEM_TABLE_ROW,
    CA_ELEM_TABLE_CELL,
    CA_ELEM_TOOLTIP,
    CA_ELEM_CTXMENU,
    CA_ELEM_MODAL,
    CA_ELEM_SPLITTER,
    CA_ELEM_IMAGE,
    CA_ELEM_COUNT
} Ca_ElementType;

/* ============================================================
   OVERFLOW
   ============================================================ */

typedef enum {
    CA_OVERFLOW_VISIBLE = 0,
    CA_OVERFLOW_HIDDEN,
    CA_OVERFLOW_SCROLL,
    CA_OVERFLOW_AUTO,
} Ca_Overflow;

/* ============================================================
   RESOLVED STYLE
   ============================================================
   Ca_ResolvedStyle itself lives in ca_resolved_style.h — it has zero
   dependency on Ca_Node, so that header is also included directly by
   ca_internal.h to give Ca_Node a resolved-style cache field without a
   circular include (ca_internal.h <- style.h already). */
#include "ca_resolved_style.h"

/* ============================================================
   API
   ============================================================ */

/** Get element type name string for CSS selector matching. */
const char *ca_elem_type_name(Ca_ElementType type);

/** Create the Causality-owned lower-priority system chrome stylesheet.
    @return Parsed stylesheet owned by the caller, or NULL on failure. */
Ca_Stylesheet *ca_style_create_system_stylesheet(void);

/** Resolve all matching CSS rules for a node, producing a merged style.
    Walks the parent chain for descendant/child selector matching.
    @param ss        Parsed stylesheet (may be NULL — returns zero style).
    @param node      Target UI node.
    @param elem_type Element type enum for element-selector matching.
    @param classes   Space-separated class string for class-selector matching.
    @param out       Output resolved style (zeroed on entry). */
void ca_style_resolve(Ca_Stylesheet *ss,
                      Ca_Node *node,
                      Ca_ElementType elem_type,
                      const char *classes,
                      Ca_ResolvedStyle *out);

/** Resolve system defaults followed by author CSS.
    Author declarations override matching default declarations regardless of
    selector specificity, matching CSS user-agent and author origin ordering.
    @param defaults  System default stylesheet, or NULL.
    @param author    Application stylesheet, or NULL.
    @param node      Target UI node.
    @param elem_type Element type enum for selector matching.
    @param classes   Space-separated class string.
    @param out       Output resolved style. */
void ca_style_resolve_layers(Ca_Stylesheet *defaults,
                             Ca_Stylesheet *author,
                             Ca_Node *node,
                             Ca_ElementType elem_type,
                             const char *classes,
                             Ca_ResolvedStyle *out);

/** Apply resolved style to a Ca_NodeDesc.
    Only fills in properties where the NodeDesc value is still at default (0).
    Non-zero NodeDesc values are treated as inline styles and take precedence.
    @param style     Resolved style (from ca_style_resolve).
    @param nd        Target node descriptor to update in-place.
    @param out_color Optional output for the text/foreground color. */
void ca_style_apply_to_node(const Ca_ResolvedStyle *style,
                            Ca_NodeDesc *nd,
                            uint32_t *out_color);

/** Scans the stylesheet once for structural/combinator ("position-
    dependent") selectors and records which classes they target — see
    the position-dependent selector classification notes in css.h. Called once by
    ca_css_parse() right after parsing; not normally called directly. */
void ca_style_classify_position_dependent(Ca_Stylesheet *ss);

/** True if a node with this class string is safe for apply_css() to serve
    from its per-node resolved-style cache — i.e. none of its classes are
    targeted by a position-dependent selector in `ss` (or `ss` has no such
    selectors at all). NULL ss is always cacheable (nothing to match). */
bool ca_style_node_is_cacheable(const Ca_Stylesheet *ss, const char *classes);
