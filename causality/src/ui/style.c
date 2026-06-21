// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* style.c — CSS style resolution, specificity calculation, and cascade */
#include "style.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char CA_SYSTEM_STYLES_CSS[] =
    ".ca-titlebar { background: #1e1e24; border-bottom-width: 1px; border-bottom-color: #4c4c58; }"
    ".ca-titlebar-menu { height: 100%; align-items: center; background: transparent; }"
    ".ca-titlebar-menu-item { height: 100%; padding: 0px 6px; align-items: center; color: #a0a0aa; background: transparent; font-size: 11px; }"
    ".ca-titlebar-menu-item:hover { background: #182e50; color: #c8c8cc; }"
    ".ca-titlebar-drag { height: 100%; align-items: center; justify-content: center; background: transparent; }"
    ".ca-titlebar-title { color: #747480; font-size: 11px; text-align: center; }"
    ".ca-titlebar-controls { height: 100%; gap: 4px; align-items: center; }"
    ".ca-titlebar-control { width: 24px; height: 20px; color: #c8c8cc; background: transparent; corner-radius: 2px; font-size: 10px; text-align: center; }"
    ".ca-titlebar-control:hover { background: #182e50; }"
    ".ca-titlebar-close { color: #a85c64; }"
    ".ca-titlebar-close:hover { background: #6f3038; color: #f0d8da; }"
    ".ca-menubar-popup { background: #1e1e24; color: #c8c8cc; corner-radius: 2px; }"
    ".ca-overlay-hover { background: #182e50; corner-radius: 1px; }"
    ".ca-overlay-selected { background: #243b5c; color: #c8c8cc; }";

/* Create the lower-priority stylesheet used for Causality-owned chrome. */
Ca_Stylesheet *ca_style_create_system_stylesheet(void)
{
    return ca_css_parse(CA_SYSTEM_STYLES_CSS);
}

/* ============================================================
   ELEMENT TYPE NAMES
   ============================================================ */

static const char *s_elem_names[CA_ELEM_COUNT] = {
    [CA_ELEM_DIV]       = "div",
    [CA_ELEM_TEXT]      = "text",
    [CA_ELEM_BUTTON]    = "button",
    [CA_ELEM_INPUT]     = "input",
    [CA_ELEM_H1]        = "h1",
    [CA_ELEM_H2]        = "h2",
    [CA_ELEM_H3]        = "h3",
    [CA_ELEM_H4]        = "h4",
    [CA_ELEM_H5]        = "h5",
    [CA_ELEM_H6]        = "h6",
    [CA_ELEM_HR]        = "hr",
    [CA_ELEM_SPACER]    = "spacer",
    [CA_ELEM_LIST]      = "list",
    [CA_ELEM_LI]        = "li",
    [CA_ELEM_CHECKBOX]  = "checkbox",
    [CA_ELEM_RADIO]     = "radio",
    [CA_ELEM_SLIDER]    = "slider",
    [CA_ELEM_TOGGLE]    = "toggle",
    [CA_ELEM_PROGRESS]  = "progress",
    [CA_ELEM_SELECT]    = "select",
    [CA_ELEM_TABBAR]    = "tabbar",
    [CA_ELEM_TAB]       = "tab",
    [CA_ELEM_TREE]      = "tree",
    [CA_ELEM_TREENODE]  = "treenode",
    [CA_ELEM_TABLE]     = "table",
    [CA_ELEM_TABLE_ROW] = "tr",
    [CA_ELEM_TABLE_CELL]= "td",
    [CA_ELEM_TOOLTIP]   = "tooltip",
    [CA_ELEM_CTXMENU]   = "contextmenu",
    [CA_ELEM_MODAL]     = "modal",
    [CA_ELEM_SPLITTER]  = "splitter",
    [CA_ELEM_IMAGE]     = "image",
};

const char *ca_elem_type_name(Ca_ElementType type)
{
    if (type >= 0 && type < CA_ELEM_COUNT)
        return s_elem_names[type];
    return "";
}

/* ============================================================
   CLASS MATCHING HELPERS
   ============================================================ */

/* Check if node's space-separated class string contains a given class */
static bool has_class(const char *class_str, const char *cls)
{
    if (!class_str || !cls || cls[0] == '\0') return false;

    int cls_len = (int)strlen(cls);
    const char *p = class_str;

    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* Find end of current token */
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        int token_len = (int)(p - start);

        if (token_len == cls_len && strncmp(start, cls, cls_len) == 0)
            return true;
    }
    return false;
}

/* ============================================================
   PSEUDO-CLASS STATE QUERIES
   ============================================================ */

static bool node_is_hovered(Ca_Node *n)
{
    /* CSS spec: :hover matches the hovered element AND all of its ancestors.
       Walk up from window->hovered_node looking for n. */
    if (!n || !n->window) return false;
    Ca_Node *h = n->window->hovered_node;
    while (h) { if (h == n) return true; h = h->parent; }
    return false;
}
static bool node_is_focused(Ca_Node *n)
{
    return n && n->window && n->window->focused_node == n;
}
static bool node_is_focus_within(Ca_Node *n)
{
    if (!n || !n->window) return false;
    Ca_Node *f = n->window->focused_node;
    while (f) { if (f == n) return true; f = f->parent; }
    return false;
}
static bool node_is_active(Ca_Node *n)
{
    /* CSS spec: :active matches the activated element and its ancestors.
       Approximated here as the drag-target chain. */
    if (!n || !n->window) return false;
    Ca_Node *d = n->window->drag_node;
    while (d) { if (d == n) return true; d = d->parent; }
    return false;
}
static bool node_is_disabled(Ca_Node *n)
{
    return n && n->desc.disabled;
}

/* Returns 1-based child index, or 0 if no parent. */
static int node_child_index(Ca_Node *n)
{
    if (!n || !n->parent) return 0;
    for (uint32_t i = 0; i < n->parent->child_count; ++i) {
        if (n->parent->children[i] == n) return (int)(i + 1);
    }
    return 0;
}
static int node_sibling_count(Ca_Node *n)
{
    if (!n || !n->parent) return 1;
    return (int)n->parent->child_count;
}

/* :nth-child(An+B) match for a 1-based index. */
static bool nth_matches(int idx, int a, int b)
{
    if (idx <= 0) return false;
    if (a == 0) return idx == b;
    int diff = idx - b;
    if ((diff % a) != 0) return false;
    return (diff / a) >= 0;
}

/* Forward decl for :not(simple) handling. */
static bool match_pseudo(const Ca_CssPseudo *ps,
                         Ca_Node *node,
                         Ca_ElementType elem_type);

static bool match_pseudo(const Ca_CssPseudo *ps,
                         Ca_Node *node,
                         Ca_ElementType elem_type)
{
    switch (ps->kind) {
        case CA_CSS_PSEUDO_NONE:         return true;
        case CA_CSS_PSEUDO_HOVER:        return node_is_hovered(node);
        case CA_CSS_PSEUDO_ACTIVE:       return node_is_active(node);
        case CA_CSS_PSEUDO_FOCUS:        return node_is_focused(node);
        case CA_CSS_PSEUDO_FOCUS_WITHIN: return node_is_focus_within(node);
        case CA_CSS_PSEUDO_DISABLED:     return node_is_disabled(node);
        case CA_CSS_PSEUDO_ENABLED:      return !node_is_disabled(node);
        case CA_CSS_PSEUDO_CHECKED:
            /* Widget-specific. Without dragging widget headers into style.c
               we expose this through a single bit on Ca_NodeDesc later;
               for now default to false. */
            return false;
        case CA_CSS_PSEUDO_FIRST_CHILD:  return node_child_index(node) == 1;
        case CA_CSS_PSEUDO_LAST_CHILD:
            return node_child_index(node) == node_sibling_count(node) &&
                   node_child_index(node) > 0;
        case CA_CSS_PSEUDO_ONLY_CHILD:   return node_sibling_count(node) == 1 && node->parent;
        case CA_CSS_PSEUDO_FIRST_OF_TYPE:
        case CA_CSS_PSEUDO_LAST_OF_TYPE:
        {
            /* Walk siblings of the same elem_type. */
            if (!node || !node->parent) return false;
            int first = -1, last = -1, my = -1;
            for (uint32_t i = 0; i < node->parent->child_count; ++i) {
                Ca_Node *c = node->parent->children[i];
                if (c && c->elem_type == (int)elem_type) {
                    if (first < 0) first = (int)i;
                    last = (int)i;
                    if (c == node) my = (int)i;
                }
            }
            if (ps->kind == CA_CSS_PSEUDO_FIRST_OF_TYPE) return my == first && my >= 0;
            return my == last && my >= 0;
        }
        case CA_CSS_PSEUDO_NTH_CHILD:
            return nth_matches(node_child_index(node), ps->a, ps->b);
        case CA_CSS_PSEUDO_NTH_LAST_CHILD: {
            int idx = node_child_index(node);
            int n   = node_sibling_count(node);
            if (idx <= 0) return false;
            return nth_matches(n - idx + 1, ps->a, ps->b);
        }
        case CA_CSS_PSEUDO_ROOT:  return node && node->parent == NULL;
        case CA_CSS_PSEUDO_EMPTY: return node && node->child_count == 0;
        case CA_CSS_PSEUDO_NOT: {
            /* Inline simple selector against this node. */
            const char *ename = ca_elem_type_name(elem_type);
            if (ps->not_element[0] && ps->not_element[0] != '*') {
                if (strcasecmp(ps->not_element, ename) != 0)
                    return true; /* doesn't match — :not is true */
            }
            if (ps->not_id[0]) {
                if (node->id[0] == '\0' || strcasecmp(ps->not_id, node->id) != 0)
                    return true;
            }
            if (ps->not_class[0]) {
                if (!has_class(node->classes, ps->not_class))
                    return true;
            }
            if (ps->not_pseudo != CA_CSS_PSEUDO_NONE) {
                Ca_CssPseudo inner = {0};
                inner.kind = ps->not_pseudo;
                if (!match_pseudo(&inner, node, elem_type))
                    return true;
            }
            /* If we got here, every simple component inside :not() matched
               — so the negation is false (only if at least one component
               was specified). */
            bool had_any = ps->not_element[0] || ps->not_id[0] ||
                           ps->not_class[0] || ps->not_pseudo != CA_CSS_PSEUDO_NONE;
            return !had_any ? true : false;
        }
    }
    return false;
}

/* ============================================================
   SIMPLE SELECTOR MATCHING
   ============================================================ */

/* Match a simple selector against a single node (including pseudo-classes). */
static bool match_simple(const Ca_CssSimpleSel *sel,
                         Ca_Node *node,
                         Ca_ElementType elem_type,
                         const char *classes,
                         const char *id)
{
    /* Element match */
    if (sel->element[0] != '\0' && sel->element[0] != '*') {
        const char *ename = ca_elem_type_name(elem_type);
        if (strcasecmp(sel->element, ename) != 0)
            return false;
    }

    /* ID match */
    if (sel->id[0] != '\0') {
        if (!id || id[0] == '\0' || strcasecmp(sel->id, id) != 0)
            return false;
    }

    /* All class selectors must match */
    for (int i = 0; i < sel->class_count; ++i) {
        if (!has_class(classes, sel->classes[i]))
            return false;
    }

    /* All pseudo-classes must match */
    for (int i = 0; i < sel->pseudo_count; ++i) {
        if (!match_pseudo(&sel->pseudos[i], node, elem_type))
            return false;
    }

    /* At least one of element, id, class, or pseudo must be specified */
    if (sel->element[0] == '\0' && sel->id[0] == '\0' &&
        sel->class_count == 0 && sel->pseudo_count == 0)
        return false;

    return true;
}

/* ============================================================
   COMPOUND SELECTOR MATCHING (with combinators)
   ============================================================ */

/* Match a compound selector against a node, walking up the parent chain.
   parts[0] is the leftmost (ancestor), parts[part_count-1] is the subject.
   The subject must match the given node. */
static bool match_selector(const Ca_CssSelector *sel,
                           Ca_Node *node,
                           Ca_ElementType elem_type,
                           const char *classes)
{
    if (sel->part_count == 0) return false;

    /* The subject is the last part — must match the current node */
    int idx = sel->part_count - 1;
    if (!match_simple(&sel->parts[idx], node, elem_type, classes, node->id))
        return false;

    if (idx == 0) return true; /* Only one part, no ancestors to check */

    /* Walk backwards through the selector chain, matching ancestors/siblings */
    Ca_Node *cur = node;
    idx--;

    while (idx >= 0) {
        Ca_CssCombinator comb = sel->parts[idx + 1].combinator;

        if (comb == CA_CSS_COMB_CHILD) {
            cur = cur->parent;
            if (!cur) return false;
            if (!match_simple(&sel->parts[idx], cur,
                              (Ca_ElementType)cur->elem_type, cur->classes, cur->id))
                return false;
            idx--;
        } else if (comb == CA_CSS_COMB_NEXT_SIBLING) {
            /* Immediately preceding sibling. */
            if (!cur->parent) return false;
            Ca_Node *prev = NULL;
            for (uint32_t i = 0; i < cur->parent->child_count; ++i) {
                if (cur->parent->children[i] == cur) break;
                prev = cur->parent->children[i];
            }
            if (!prev) return false;
            if (!match_simple(&sel->parts[idx], prev,
                              (Ca_ElementType)prev->elem_type, prev->classes, prev->id))
                return false;
            cur = prev;
            idx--;
        } else if (comb == CA_CSS_COMB_SUBSEQ_SIBLING) {
            /* Any preceding sibling. */
            if (!cur->parent) return false;
            bool found = false;
            Ca_Node *match = NULL;
            for (uint32_t i = 0; i < cur->parent->child_count; ++i) {
                Ca_Node *c = cur->parent->children[i];
                if (c == cur) break;
                if (match_simple(&sel->parts[idx], c,
                                 (Ca_ElementType)c->elem_type, c->classes, c->id)) {
                    found = true; match = c;
                }
            }
            if (!found) return false;
            cur = match;
            idx--;
        } else {
            /* Descendant combinator (default) — walk up until a match. */
            cur = cur->parent;
            bool found = false;
            while (cur) {
                if (match_simple(&sel->parts[idx], cur,
                                 (Ca_ElementType)cur->elem_type, cur->classes, cur->id)) {
                    found = true;
                    break;
                }
                cur = cur->parent;
            }
            if (!found) return false;
            idx--;
        }
    }

    return (idx < 0);
}

/* ============================================================
   SPECIFICITY
   ============================================================ */

/* Returns specificity as a single comparable integer.
   Format: (id_count << 20) | ((class_count + pseudo_count) << 10) | element_count
   This is the standard CSS (a,b,c) tuple packed into bits. */
static int calc_specificity(const Ca_CssSelector *sel)
{
    int ids      = 0;
    int elements = 0;
    int classes  = 0;

    for (int i = 0; i < sel->part_count; ++i) {
        const Ca_CssSimpleSel *part = &sel->parts[i];
        if (part->element[0] != '\0' && part->element[0] != '*')
            elements++;
        if (part->id[0] != '\0')
            ids++;
        classes += part->class_count;
        classes += part->pseudo_count;
    }

    return (ids << 20) | (classes << 10) | elements;
}

/* ============================================================
   CASCADE — resolve matching rules
   ============================================================ */

/* A matched rule entry for sorting */
typedef struct {
    int specificity;
    int source_order;
    const Ca_CssDecl *decls;
    int decl_count;
} MatchedRule;

#define MAX_MATCHED_RULES 128

static int compare_matched(const void *a, const void *b)
{
    const MatchedRule *ma = (const MatchedRule *)a;
    const MatchedRule *mb = (const MatchedRule *)b;
    /* Lower specificity first, then lower source_order first */
    if (ma->specificity != mb->specificity)
        return ma->specificity - mb->specificity;
    return ma->source_order - mb->source_order;
}

static float css_val_to_px(const Ca_CssValue *v)
{
    if (v->type == CA_CSS_VAL_PX || v->type == CA_CSS_VAL_NUMBER)
        return v->number;
    if (v->type == CA_CSS_VAL_PERCENT)
        return v->number; /* stored as percentage, layout will interpret */
    return 0.0f;
}

/* Resolve a CSS value, substituting var() against the stylesheet's vars. */
static Ca_CssValue resolve_value(const Ca_Stylesheet *ss, const Ca_CssValue *in)
{
    if (in->type != CA_CSS_VAL_VAR) return *in;
    if (!ss) { Ca_CssValue z = {0}; return z; }
    const char *name = ca_css_str(ss, in->keyword);
    if (!name) { Ca_CssValue z = {0}; return z; }
    for (int i = 0; i < ss->var_count; ++i) {
        if (strcmp(ss->vars[i].name, name) == 0) {
            /* The variable's value itself could be a var() — follow one
               level of indirection to keep things bounded. */
            const Ca_CssValue *v = &ss->vars[i].value;
            if (v->type == CA_CSS_VAL_VAR) {
                const char *n2 = ca_css_str(ss, v->keyword);
                if (n2) {
                    for (int j = 0; j < ss->var_count; ++j) {
                        if (strcmp(ss->vars[j].name, n2) == 0)
                            return ss->vars[j].value;
                    }
                }
                Ca_CssValue z = {0}; return z;
            }
            return *v;
        }
    }
    Ca_CssValue z = {0};
    return z;
}

/* Resolve one cascade origin, optionally preserving an earlier origin. */
static void style_resolve_sheet(Ca_Stylesheet *ss,
                                Ca_Node *node,
                                Ca_ElementType elem_type,
                                const char *classes,
                                Ca_ResolvedStyle *out,
                                bool clear_output)
{
    if (clear_output)
        memset(out, 0, sizeof(*out));
    if (!ss) return;

    /* Collect all matching rules */
    MatchedRule matched[MAX_MATCHED_RULES];
    int match_count = 0;

    for (int r = 0; r < ss->rule_count; ++r) {
        Ca_CssRule *rule = &ss->rules[r];

        for (int s = 0; s < rule->selector_count; ++s) {
            if (match_selector(&rule->selectors[s], node, elem_type, classes)) {
                if (match_count < MAX_MATCHED_RULES) {
                    matched[match_count].specificity  = calc_specificity(&rule->selectors[s]);
                    matched[match_count].source_order = rule->source_order;
                    matched[match_count].decls        = rule->decls;
                    matched[match_count].decl_count   = rule->decl_count;
                    match_count++;
                }
                break; /* One match per rule is enough */
            }
        }
    }

    if (match_count == 0) return;

    /* Sort by specificity (ascending) then source order (ascending).
       Later entries override earlier ones, which is correct CSS cascade. */
    qsort(matched, match_count, sizeof(MatchedRule), compare_matched);

    /* Two passes:
       1. Apply non-important declarations.
       2. Apply important declarations (which override).
       Within each pass, the matched-rule order (spec asc, source asc) makes
       later applications win. This matches the CSS cascade rules for
       non-stratified origin (author-only sheets). */
    for (int pass = 0; pass < 2; ++pass) {
    for (int m = 0; m < match_count; ++m) {
        for (int d = 0; d < matched[m].decl_count; ++d) {
            const Ca_CssDecl *decl = &matched[m].decls[d];
            if ((pass == 0 && decl->important) ||
                (pass == 1 && !decl->important))
                continue;
            Ca_CssPropId prop = decl->prop;
            Ca_CssValue resolved = resolve_value(ss, &decl->value);
            const Ca_CssValue *val = &resolved;

            if (val->type == CA_CSS_VAL_NONE) continue;

            if ((int)prop < 64)
                out->set_mask  |= (1ULL << (int)prop);
            else
                out->set_mask2 |= (1ULL << ((int)prop - 64));

            switch (prop) {
                case CA_CSS_PROP_WIDTH:
                    out->width = css_val_to_px(val);
                    out->width_pct = (val->type == CA_CSS_VAL_PERCENT);
                    break;
                case CA_CSS_PROP_HEIGHT:
                    out->height = css_val_to_px(val);
                    out->height_pct = (val->type == CA_CSS_VAL_PERCENT);
                    break;
                case CA_CSS_PROP_MIN_WIDTH:        out->min_width       = css_val_to_px(val); break;
                case CA_CSS_PROP_MAX_WIDTH:        out->max_width       = css_val_to_px(val); break;
                case CA_CSS_PROP_MIN_HEIGHT:       out->min_height      = css_val_to_px(val); break;
                case CA_CSS_PROP_MAX_HEIGHT:       out->max_height      = css_val_to_px(val); break;
                case CA_CSS_PROP_PADDING_TOP:      out->padding[0]      = css_val_to_px(val); break;
                case CA_CSS_PROP_PADDING_RIGHT:    out->padding[1]      = css_val_to_px(val); break;
                case CA_CSS_PROP_PADDING_BOTTOM:   out->padding[2]      = css_val_to_px(val); break;
                case CA_CSS_PROP_PADDING_LEFT:     out->padding[3]      = css_val_to_px(val); break;
                case CA_CSS_PROP_MARGIN_TOP:       out->margin[0]       = css_val_to_px(val); break;
                case CA_CSS_PROP_MARGIN_RIGHT:     out->margin[1]       = css_val_to_px(val); break;
                case CA_CSS_PROP_MARGIN_BOTTOM:    out->margin[2]       = css_val_to_px(val); break;
                case CA_CSS_PROP_MARGIN_LEFT:      out->margin[3]       = css_val_to_px(val); break;
                case CA_CSS_PROP_GAP:              out->gap             = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_RADIUS:    out->border_radius   = css_val_to_px(val); break;
                case CA_CSS_PROP_OPACITY:          out->opacity         = val->number;        break;
                case CA_CSS_PROP_FONT_SIZE:        out->font_size       = css_val_to_px(val); break;
                case CA_CSS_PROP_FONT_WEIGHT:
                    if (val->type == CA_CSS_VAL_NUMBER) {
                        out->font_weight = (int)val->number;
                        out->font_bold   = (val->number >= 700.0f);
                    } else if (val->type == CA_CSS_VAL_KEYWORD) {
                        out->font_bold   = (val->keyword != 0);
                        out->font_weight = out->font_bold ? 700 : 400;
                    }
                    break;
                case CA_CSS_PROP_TEXT_ALIGN:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->text_align = val->keyword;
                    break;
                case CA_CSS_PROP_FLEX_GROW:        out->flex_grow       = val->number;        break;
                case CA_CSS_PROP_FLEX_SHRINK:      out->flex_shrink     = val->number;        break;
                case CA_CSS_PROP_BACKGROUND_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR)
                        out->background_color = val->color;
                    break;
                case CA_CSS_PROP_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR)
                        out->color = val->color;
                    break;
                case CA_CSS_PROP_DISPLAY:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->display = val->keyword;
                    break;
                case CA_CSS_PROP_FLEX_DIRECTION:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->flex_direction = val->keyword;
                    break;
                case CA_CSS_PROP_FLEX_WRAP:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->flex_wrap = val->keyword;
                    break;
                case CA_CSS_PROP_ALIGN_ITEMS:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->align_items = val->keyword;
                    break;
                case CA_CSS_PROP_JUSTIFY_CONTENT:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->justify_content = val->keyword;
                    break;
                case CA_CSS_PROP_OVERFLOW_X:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->overflow_x = val->keyword;
                    break;
                case CA_CSS_PROP_OVERFLOW_Y:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->overflow_y = val->keyword;
                    break;
                case CA_CSS_PROP_OVERFLOW:
                    if (val->type == CA_CSS_VAL_KEYWORD) {
                        out->overflow_x = val->keyword;
                        out->overflow_y = val->keyword;
                        out->set_mask |= (1ULL << (int)CA_CSS_PROP_OVERFLOW_X) |
                                         (1ULL << (int)CA_CSS_PROP_OVERFLOW_Y);
                    }
                    break;
                case CA_CSS_PROP_SCROLLBAR_WIDTH:
                    out->scrollbar_width = css_val_to_px(val); break;
                case CA_CSS_PROP_SCROLLBAR_TRACK_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->scrollbar_track_color = val->color;
                    break;
                case CA_CSS_PROP_SCROLLBAR_THUMB_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->scrollbar_thumb_color = val->color;
                    break;
                case CA_CSS_PROP_SCROLLBAR_THUMB_ACTIVE_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->scrollbar_thumb_active_color = val->color;
                    break;
                case CA_CSS_PROP_SCROLLBAR_RADIUS:
                    out->scrollbar_radius = css_val_to_px(val); break;
                case CA_CSS_PROP_TRANSITION: {
                    int tprop = val->keyword;
                    float dur = val->number;
                    out->transition_duration = dur;
                    if (tprop == (int)CA_CSS_PROP_COUNT)
                        out->transition_props = ~0ULL; /* all */
                    else if (tprop > 0)
                        out->transition_props |= (1ULL << tprop);
                    break;
                }
                case CA_CSS_PROP_BORDER_WIDTH:
                    out->border_width = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR)
                        out->border_color = val->color;
                    break;
                case CA_CSS_PROP_BORDER_TOP_WIDTH:
                    out->border_top_w = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_TOP_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->border_top_c = val->color;
                    break;
                case CA_CSS_PROP_BORDER_RIGHT_WIDTH:
                    out->border_right_w = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_RIGHT_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->border_right_c = val->color;
                    break;
                case CA_CSS_PROP_BORDER_BOTTOM_WIDTH:
                    out->border_bottom_w = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_BOTTOM_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->border_bottom_c = val->color;
                    break;
                case CA_CSS_PROP_BORDER_LEFT_WIDTH:
                    out->border_left_w = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_LEFT_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->border_left_c = val->color;
                    break;
                case CA_CSS_PROP_BOX_SHADOW:
                    /* Two decls are emitted per box-shadow shorthand:
                       Decl A (keyword=0): type=COLOR — color=shadow_color,
                                           keyword packs offset_x (upper 16 bits) and
                                           offset_y (lower 16 bits) as signed int16.
                       Decl B (keyword=1): type=NUMBER — number=blur radius. */
                    if (val->keyword == 1) {
                        out->shadow_blur = val->number;
                    } else {
                        out->shadow_color    = val->color;
                        int16_t ox = (int16_t)((uint32_t)val->keyword >> 16);
                        int16_t oy = (int16_t)((uint32_t)val->keyword & 0xFFFF);
                        out->shadow_offset_x = (float)ox;
                        out->shadow_offset_y = (float)oy;
                    }
                    break;
                case CA_CSS_PROP_Z_INDEX:
                    out->z_index = (int)val->number; break;
                case CA_CSS_PROP_TEXT_WRAP:
                    if (val->type == CA_CSS_VAL_KEYWORD)
                        out->text_wrap = (val->keyword == CA_CSS_WRAP_WRAP) ? 1 : 0;
                    break;

                /* New CSS3 properties */
                case CA_CSS_PROP_ROW_GAP:    out->row_gap    = css_val_to_px(val); break;
                case CA_CSS_PROP_COLUMN_GAP: out->column_gap = css_val_to_px(val); break;
                case CA_CSS_PROP_ALIGN_SELF:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->align_self = val->keyword;
                    break;
                case CA_CSS_PROP_ALIGN_CONTENT:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->align_content = val->keyword;
                    break;
                case CA_CSS_PROP_JUSTIFY_SELF:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->justify_self = val->keyword;
                    break;
                case CA_CSS_PROP_FLEX_BASIS:  out->flex_basis = css_val_to_px(val); break;
                case CA_CSS_PROP_ORDER:       out->flex_order = (int)val->number;   break;
                case CA_CSS_PROP_BORDER_TOP_LEFT_RADIUS:
                    out->border_radius_tl = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_TOP_RIGHT_RADIUS:
                    out->border_radius_tr = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS:
                    out->border_radius_br = css_val_to_px(val); break;
                case CA_CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS:
                    out->border_radius_bl = css_val_to_px(val); break;
                case CA_CSS_PROP_VISIBILITY:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->visibility = val->keyword;
                    break;
                case CA_CSS_PROP_FONT_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->font_style = val->keyword;
                    break;
                case CA_CSS_PROP_LINE_HEIGHT:  out->line_height   = css_val_to_px(val); break;
                case CA_CSS_PROP_LETTER_SPACING: out->letter_spacing = css_val_to_px(val); break;
                case CA_CSS_PROP_WORD_SPACING:   out->word_spacing  = css_val_to_px(val); break;
                case CA_CSS_PROP_TEXT_DECORATION:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->text_decoration = val->keyword;
                    break;
                case CA_CSS_PROP_TEXT_TRANSFORM:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->text_transform = val->keyword;
                    break;
                case CA_CSS_PROP_WHITE_SPACE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->white_space = val->keyword;
                    break;
                case CA_CSS_PROP_BORDER_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->border_style = val->keyword;
                    break;
                case CA_CSS_PROP_BORDER_TOP_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->border_top_style = val->keyword;
                    break;
                case CA_CSS_PROP_BORDER_RIGHT_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->border_right_style = val->keyword;
                    break;
                case CA_CSS_PROP_BORDER_BOTTOM_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->border_bottom_style = val->keyword;
                    break;
                case CA_CSS_PROP_BORDER_LEFT_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->border_left_style = val->keyword;
                    break;
                case CA_CSS_PROP_OUTLINE_WIDTH:  out->outline_width  = css_val_to_px(val); break;
                case CA_CSS_PROP_OUTLINE_COLOR:
                    if (val->type == CA_CSS_VAL_COLOR) out->outline_color = val->color;
                    break;
                case CA_CSS_PROP_OUTLINE_STYLE:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->outline_style = val->keyword;
                    break;
                case CA_CSS_PROP_OUTLINE_OFFSET: out->outline_offset = css_val_to_px(val); break;
                case CA_CSS_PROP_ASPECT_RATIO:   out->aspect_ratio   = val->number;        break;
                case CA_CSS_PROP_BOX_SIZING:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->box_sizing = val->keyword;
                    break;
                case CA_CSS_PROP_CURSOR:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->cursor = val->keyword;
                    break;
                case CA_CSS_PROP_POINTER_EVENTS:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->pointer_events = val->keyword;
                    break;
                case CA_CSS_PROP_USER_SELECT:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->user_select = val->keyword;
                    break;
                case CA_CSS_PROP_SCROLL_BEHAVIOR:
                    if (val->type == CA_CSS_VAL_KEYWORD) out->scroll_behavior = val->keyword;
                    break;
                case CA_CSS_PROP_BACKGROUND:
                    /* Gradient start color; draw_mode encoded in keyword field (2=linear, 3=radial) */
                    if (val->type == CA_CSS_VAL_COLOR) {
                        out->background_color = val->color;
                        out->gradient_type = (uint8_t)val->keyword;
                    }
                    break;
                case CA_CSS_PROP_GRADIENT_COLOR2:
                    if (val->type == CA_CSS_VAL_COLOR) out->gradient_color2 = val->color;
                    break;
                case CA_CSS_PROP_GRADIENT_ANGLE:
                    if (val->type == CA_CSS_VAL_NUMBER) out->gradient_angle = val->number;
                    break;
                case CA_CSS_PROP_GRADIENT_CX:
                    if (val->type == CA_CSS_VAL_NUMBER) out->gradient_cx = val->number;
                    break;
                case CA_CSS_PROP_GRADIENT_CY:
                    if (val->type == CA_CSS_VAL_NUMBER) out->gradient_cy = val->number;
                    break;
                default: break;
            }
        }
    }
    } /* end pass loop */
}

void ca_style_resolve(Ca_Stylesheet *ss,
                      Ca_Node *node,
                      Ca_ElementType elem_type,
                      const char *classes,
                      Ca_ResolvedStyle *out)
{
    style_resolve_sheet(ss, node, elem_type, classes, out, true);
}

void ca_style_resolve_layers(Ca_Stylesheet *defaults,
                             Ca_Stylesheet *author,
                             Ca_Node *node,
                             Ca_ElementType elem_type,
                             const char *classes,
                             Ca_ResolvedStyle *out)
{
    style_resolve_sheet(defaults, node, elem_type, classes, out, true);
    style_resolve_sheet(author, node, elem_type, classes, out, false);
}

/* ============================================================
   APPLY STYLE TO NODE DESC
   ============================================================ */

#define STYLE_SET(prop) \
    (((int)(prop) < 64) \
        ? (style->set_mask  & (1ULL << (int)(prop))) \
        : (style->set_mask2 & (1ULL << ((int)(prop) - 64))))

void ca_style_apply_to_node(const Ca_ResolvedStyle *style,
                            Ca_NodeDesc *nd,
                            uint32_t *out_color)
{
    if (!style || (style->set_mask == 0 && style->set_mask2 == 0)) return;

    /* display: none collapses the node immediately */
    if (STYLE_SET(CA_CSS_PROP_DISPLAY) && style->display == CA_CSS_DISPLAY_NONE) {
        nd->hidden = true;
        return;
    }

    /* Sizing — CSS fills if NodeDesc is 0 (auto) */
    if (nd->width  <= 0.0f && STYLE_SET(CA_CSS_PROP_WIDTH))  { nd->width  = style->width;  nd->width_pct  = style->width_pct;  }
    if (nd->height <= 0.0f && STYLE_SET(CA_CSS_PROP_HEIGHT)) { nd->height = style->height; nd->height_pct = style->height_pct; }
    if (nd->min_w  <= 0.0f && STYLE_SET(CA_CSS_PROP_MIN_WIDTH))  nd->min_w = style->min_width;
    if (nd->max_w  <= 0.0f && STYLE_SET(CA_CSS_PROP_MAX_WIDTH))  nd->max_w = style->max_width;
    if (nd->min_h  <= 0.0f && STYLE_SET(CA_CSS_PROP_MIN_HEIGHT)) nd->min_h = style->min_height;
    if (nd->max_h  <= 0.0f && STYLE_SET(CA_CSS_PROP_MAX_HEIGHT)) nd->max_h = style->max_height;

    /* Padding — CSS fills if zero */
    if (nd->padding_top    <= 0.0f && STYLE_SET(CA_CSS_PROP_PADDING_TOP))    nd->padding_top    = style->padding[0];
    if (nd->padding_right  <= 0.0f && STYLE_SET(CA_CSS_PROP_PADDING_RIGHT))  nd->padding_right  = style->padding[1];
    if (nd->padding_bottom <= 0.0f && STYLE_SET(CA_CSS_PROP_PADDING_BOTTOM)) nd->padding_bottom = style->padding[2];
    if (nd->padding_left   <= 0.0f && STYLE_SET(CA_CSS_PROP_PADDING_LEFT))   nd->padding_left   = style->padding[3];

    /* Margin — CSS fills if zero */
    if (nd->margin_top    <= 0.0f && STYLE_SET(CA_CSS_PROP_MARGIN_TOP))    nd->margin_top    = style->margin[0];
    if (nd->margin_right  <= 0.0f && STYLE_SET(CA_CSS_PROP_MARGIN_RIGHT))  nd->margin_right  = style->margin[1];
    if (nd->margin_bottom <= 0.0f && STYLE_SET(CA_CSS_PROP_MARGIN_BOTTOM)) nd->margin_bottom = style->margin[2];
    if (nd->margin_left   <= 0.0f && STYLE_SET(CA_CSS_PROP_MARGIN_LEFT))   nd->margin_left   = style->margin[3];

    /* Opacity — 0 means not set, default is 1.0 */
    if (nd->opacity <= 0.0f && STYLE_SET(CA_CSS_PROP_OPACITY))
        nd->opacity = style->opacity;

    /* Gap — prefer row/column, fall back to uniform gap */
    if (nd->gap        <= 0.0f && STYLE_SET(CA_CSS_PROP_GAP))        nd->gap        = style->gap;
    if (nd->row_gap    <= 0.0f && STYLE_SET(CA_CSS_PROP_ROW_GAP))    nd->row_gap    = style->row_gap;
    if (nd->column_gap <= 0.0f && STYLE_SET(CA_CSS_PROP_COLUMN_GAP)) nd->column_gap = style->column_gap;

    /* Border radius — uniform and per-corner */
    if (nd->corner_radius <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_RADIUS))
        nd->corner_radius = style->border_radius;
    if (nd->border_radius_tl <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_TOP_LEFT_RADIUS))
        nd->border_radius_tl = style->border_radius_tl;
    if (nd->border_radius_tr <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_TOP_RIGHT_RADIUS))
        nd->border_radius_tr = style->border_radius_tr;
    if (nd->border_radius_br <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS))
        nd->border_radius_br = style->border_radius_br;
    if (nd->border_radius_bl <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS))
        nd->border_radius_bl = style->border_radius_bl;
    /* If any per-corner was set, derive uniform from max for the GPU shader */
    {
        float mx = 0.0f;
        if (nd->border_radius_tl > mx) mx = nd->border_radius_tl;
        if (nd->border_radius_tr > mx) mx = nd->border_radius_tr;
        if (nd->border_radius_br > mx) mx = nd->border_radius_br;
        if (nd->border_radius_bl > mx) mx = nd->border_radius_bl;
        if (mx > 0.0f && nd->corner_radius <= 0.0f)
            nd->corner_radius = mx;
    }

    /* Background color — 0 = transparent = not set.
       Gradient background (CA_CSS_PROP_BACKGROUND) also writes background_color
       for the start stop, so both properties feed nd->background. */
    if (nd->background == 0) {
        if (STYLE_SET(CA_CSS_PROP_BACKGROUND_COLOR))
            nd->background = style->background_color;
        else if (STYLE_SET(CA_CSS_PROP_BACKGROUND) && style->gradient_type != 0)
            nd->background = style->background_color;
    }

    /* Gradient parameters */
    if (nd->gradient_type == 0 && style->gradient_type != 0) {
        nd->gradient_type   = style->gradient_type;
        nd->gradient_color2 = style->gradient_color2;
        nd->gradient_angle  = style->gradient_angle;
        nd->gradient_cx     = style->gradient_cx;
        nd->gradient_cy     = style->gradient_cy;
    }

    /* Direction from flex-direction */
    if (STYLE_SET(CA_CSS_PROP_FLEX_DIRECTION)) {
        switch (style->flex_direction) {
            case CA_CSS_FLEX_ROW:
            case CA_CSS_FLEX_ROW_REVERSE:
                nd->direction = CA_DIR_ROW; break;
            case CA_CSS_FLEX_COLUMN:
            case CA_CSS_FLEX_COLUMN_REVERSE:
                nd->direction = CA_DIR_COLUMN; break;
        }
    }

    /* Alignment */
    if (STYLE_SET(CA_CSS_PROP_ALIGN_ITEMS)) {
        switch (style->align_items) {
            case CA_CSS_ALIGN_FLEX_START: nd->align_items = CA_ALIGN_START;   break;
            case CA_CSS_ALIGN_CENTER:     nd->align_items = CA_ALIGN_CENTER;  break;
            case CA_CSS_ALIGN_FLEX_END:   nd->align_items = CA_ALIGN_END;     break;
            case CA_CSS_ALIGN_STRETCH:    nd->align_items = CA_ALIGN_STRETCH; break;
        }
    }
    if (STYLE_SET(CA_CSS_PROP_ALIGN_SELF)) {
        switch (style->align_self) {
            case CA_CSS_ALIGN_FLEX_START: nd->align_self = CA_ALIGN_START;   break;
            case CA_CSS_ALIGN_CENTER:     nd->align_self = CA_ALIGN_CENTER;  break;
            case CA_CSS_ALIGN_FLEX_END:   nd->align_self = CA_ALIGN_END;     break;
            case CA_CSS_ALIGN_STRETCH:    nd->align_self = CA_ALIGN_STRETCH; break;
        }
    }
    if (STYLE_SET(CA_CSS_PROP_ALIGN_CONTENT))
        nd->align_content = (Ca_Align)style->align_content;
    if (STYLE_SET(CA_CSS_PROP_JUSTIFY_CONTENT)) {
        switch (style->justify_content) {
            case CA_CSS_ALIGN_FLEX_START: nd->justify_content = CA_ALIGN_START;  break;
            case CA_CSS_ALIGN_CENTER:     nd->justify_content = CA_ALIGN_CENTER; break;
            case CA_CSS_ALIGN_FLEX_END:   nd->justify_content = CA_ALIGN_END;    break;
            default:                      nd->justify_content = (Ca_Align)style->justify_content; break;
        }
    }

    /* Overflow — map CSS keyword enum to internal 0-3 values */
    if (STYLE_SET(CA_CSS_PROP_OVERFLOW_X)) {
        switch (style->overflow_x) {
            case CA_CSS_OVERFLOW_HIDDEN:  nd->overflow_x = 1; break;
            case CA_CSS_OVERFLOW_SCROLL:  nd->overflow_x = 2; break;
            case CA_CSS_OVERFLOW_AUTO:    nd->overflow_x = 3; break;
            default:                      nd->overflow_x = 0; break;
        }
    }
    if (STYLE_SET(CA_CSS_PROP_OVERFLOW_Y)) {
        switch (style->overflow_y) {
            case CA_CSS_OVERFLOW_HIDDEN:  nd->overflow_y = 1; break;
            case CA_CSS_OVERFLOW_SCROLL:  nd->overflow_y = 2; break;
            case CA_CSS_OVERFLOW_AUTO:    nd->overflow_y = 3; break;
            default:                      nd->overflow_y = 0; break;
        }
    }

    if (STYLE_SET(CA_CSS_PROP_SCROLLBAR_WIDTH)) {
        nd->scrollbar_width = style->scrollbar_width;
        nd->scrollbar_width_set = true;
    }
    if (STYLE_SET(CA_CSS_PROP_SCROLLBAR_TRACK_COLOR)) {
        nd->scrollbar_track_color = style->scrollbar_track_color;
        nd->scrollbar_track_color_set = true;
    }
    if (STYLE_SET(CA_CSS_PROP_SCROLLBAR_THUMB_COLOR)) {
        nd->scrollbar_thumb_color = style->scrollbar_thumb_color;
        nd->scrollbar_thumb_color_set = true;
    }
    if (STYLE_SET(CA_CSS_PROP_SCROLLBAR_THUMB_ACTIVE_COLOR)) {
        nd->scrollbar_thumb_active_color = style->scrollbar_thumb_active_color;
        nd->scrollbar_thumb_active_color_set = true;
    }
    if (STYLE_SET(CA_CSS_PROP_SCROLLBAR_RADIUS)) {
        nd->scrollbar_radius = style->scrollbar_radius;
        nd->scrollbar_radius_set = true;
    }

    /* Flex grow/shrink/basis */
    if (nd->flex_grow   <= 0.0f && STYLE_SET(CA_CSS_PROP_FLEX_GROW))   nd->flex_grow   = style->flex_grow;
    if (nd->flex_shrink <= 0.0f && STYLE_SET(CA_CSS_PROP_FLEX_SHRINK)) nd->flex_shrink = style->flex_shrink;
    if (nd->flex_basis  <= 0.0f && STYLE_SET(CA_CSS_PROP_FLEX_BASIS))  nd->flex_basis  = style->flex_basis;
    if (nd->flex_order  == 0    && STYLE_SET(CA_CSS_PROP_ORDER))        nd->flex_order  = style->flex_order;

    /* Flex wrap */
    if (nd->flex_wrap == 0 && STYLE_SET(CA_CSS_PROP_FLEX_WRAP)) {
        if (style->flex_wrap == CA_CSS_WRAP_WRAP) nd->flex_wrap = 1;
    }

    /* Font */
    if (nd->font_size     <= 0.0f && STYLE_SET(CA_CSS_PROP_FONT_SIZE))     nd->font_size     = style->font_size;
    if (nd->line_height   <= 0.0f && STYLE_SET(CA_CSS_PROP_LINE_HEIGHT))   nd->line_height   = style->line_height;
    if (nd->letter_spacing == 0.0f && STYLE_SET(CA_CSS_PROP_LETTER_SPACING)) nd->letter_spacing = style->letter_spacing;
    if (nd->word_spacing   == 0.0f && STYLE_SET(CA_CSS_PROP_WORD_SPACING))   nd->word_spacing   = style->word_spacing;
    if (STYLE_SET(CA_CSS_PROP_FONT_WEIGHT)) {
        nd->font_bold   = style->font_bold;
        nd->font_weight = (uint8_t)(style->font_weight > 255 ? 255 : style->font_weight);
    }
    if (STYLE_SET(CA_CSS_PROP_FONT_STYLE)) nd->font_style = (uint8_t)style->font_style;

    /* Text */
    if (STYLE_SET(CA_CSS_PROP_TEXT_ALIGN)) {
        switch (style->text_align) {
            case CA_CSS_TEXT_ALIGN_LEFT:   nd->text_align = 0; break;
            case CA_CSS_TEXT_ALIGN_CENTER: nd->text_align = 1; break;
            case CA_CSS_TEXT_ALIGN_RIGHT:  nd->text_align = 2; break;
        }
    }
    if (STYLE_SET(CA_CSS_PROP_TEXT_DECORATION)) nd->text_decoration = (uint8_t)style->text_decoration;
    if (STYLE_SET(CA_CSS_PROP_TEXT_TRANSFORM))  nd->text_transform  = (uint8_t)style->text_transform;
    if (STYLE_SET(CA_CSS_PROP_WHITE_SPACE))     nd->white_space     = (uint8_t)style->white_space;
    if (nd->text_wrap == 0 && STYLE_SET(CA_CSS_PROP_TEXT_WRAP))
        nd->text_wrap = (uint8_t)style->text_wrap;

    /* Text/foreground color — output separately */
    if (out_color && *out_color == 0 && STYLE_SET(CA_CSS_PROP_COLOR))
        *out_color = style->color;

    /* Visibility */
    if (STYLE_SET(CA_CSS_PROP_VISIBILITY))
        nd->visibility_hidden = (style->visibility != 0);

    /* Aspect ratio */
    if (nd->aspect_ratio <= 0.0f && STYLE_SET(CA_CSS_PROP_ASPECT_RATIO))
        nd->aspect_ratio = style->aspect_ratio;

    /* Box sizing */
    if (STYLE_SET(CA_CSS_PROP_BOX_SIZING))
        nd->box_sizing = (uint8_t)style->box_sizing;

    /* Interaction */
    if (STYLE_SET(CA_CSS_PROP_CURSOR))         nd->cursor         = (uint8_t)style->cursor;
    if (STYLE_SET(CA_CSS_PROP_POINTER_EVENTS)) nd->pointer_events = (uint8_t)style->pointer_events;
    if (STYLE_SET(CA_CSS_PROP_USER_SELECT))    nd->user_select    = (uint8_t)style->user_select;

    /* Border — uniform */
    if (nd->border_width <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_WIDTH))
        nd->border_width = style->border_width;
    if (nd->border_color == 0 && STYLE_SET(CA_CSS_PROP_BORDER_COLOR))
        nd->border_color = style->border_color;
    /* Border — per-side */
    if (nd->border_top_w    <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_TOP_WIDTH))    nd->border_top_w    = style->border_top_w;
    if (nd->border_top_c    == 0    && STYLE_SET(CA_CSS_PROP_BORDER_TOP_COLOR))    nd->border_top_c    = style->border_top_c;
    if (nd->border_right_w  <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_RIGHT_WIDTH))  nd->border_right_w  = style->border_right_w;
    if (nd->border_right_c  == 0    && STYLE_SET(CA_CSS_PROP_BORDER_RIGHT_COLOR))  nd->border_right_c  = style->border_right_c;
    if (nd->border_bottom_w <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_BOTTOM_WIDTH)) nd->border_bottom_w = style->border_bottom_w;
    if (nd->border_bottom_c == 0    && STYLE_SET(CA_CSS_PROP_BORDER_BOTTOM_COLOR)) nd->border_bottom_c = style->border_bottom_c;
    if (nd->border_left_w   <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_LEFT_WIDTH))   nd->border_left_w   = style->border_left_w;
    if (nd->border_left_c   == 0    && STYLE_SET(CA_CSS_PROP_BORDER_LEFT_COLOR))   nd->border_left_c   = style->border_left_c;
    /* Per-side fallback to uniform border */
    if (style->border_width > 0.0f) {
        if (nd->border_top_w    <= 0.0f) nd->border_top_w    = style->border_width;
        if (nd->border_right_w  <= 0.0f) nd->border_right_w  = style->border_width;
        if (nd->border_bottom_w <= 0.0f) nd->border_bottom_w = style->border_width;
        if (nd->border_left_w   <= 0.0f) nd->border_left_w   = style->border_width;
    }
    if (style->border_color != 0) {
        if (nd->border_top_c    == 0) nd->border_top_c    = style->border_color;
        if (nd->border_right_c  == 0) nd->border_right_c  = style->border_color;
        if (nd->border_bottom_c == 0) nd->border_bottom_c = style->border_color;
        if (nd->border_left_c   == 0) nd->border_left_c   = style->border_color;
    }

    /* Outline */
    if (nd->outline_width  <= 0.0f && STYLE_SET(CA_CSS_PROP_OUTLINE_WIDTH))  nd->outline_width  = style->outline_width;
    if (nd->outline_color  == 0    && STYLE_SET(CA_CSS_PROP_OUTLINE_COLOR))  nd->outline_color  = style->outline_color;
    if (nd->outline_offset == 0.0f && STYLE_SET(CA_CSS_PROP_OUTLINE_OFFSET)) nd->outline_offset = style->outline_offset;

    /* Box shadow */
    if (nd->shadow_color == 0 && STYLE_SET(CA_CSS_PROP_BOX_SHADOW)) {
        nd->shadow_offset_x = style->shadow_offset_x;
        nd->shadow_offset_y = style->shadow_offset_y;
        nd->shadow_blur     = style->shadow_blur;
        nd->shadow_color    = style->shadow_color;
    }

    /* Z-index */
    if (nd->z_index == 0 && STYLE_SET(CA_CSS_PROP_Z_INDEX))
        nd->z_index = (int16_t)style->z_index;
}
