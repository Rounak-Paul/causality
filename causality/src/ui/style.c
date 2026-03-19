/* style.c — CSS style resolution, specificity calculation, and cascade */
#include "style.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

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
   SIMPLE SELECTOR MATCHING
   ============================================================ */

/* Match a simple selector against a single node */
static bool match_simple(const Ca_CssSimpleSel *sel,
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

    /* At least one of element, id, or class must be specified */
    if (sel->element[0] == '\0' && sel->id[0] == '\0' && sel->class_count == 0)
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
    if (!match_simple(&sel->parts[idx], elem_type, classes, node->id))
        return false;

    if (idx == 0) return true; /* Only one part, no ancestors to check */

    /* Walk backwards through the selector chain, matching against ancestors */
    Ca_Node *cur = node->parent;
    idx--;

    while (idx >= 0 && cur) {
        Ca_CssCombinator comb = sel->parts[idx + 1].combinator;

        if (comb == CA_CSS_COMB_CHILD) {
            /* Direct parent must match */
            if (!match_simple(&sel->parts[idx],
                              (Ca_ElementType)cur->elem_type, cur->classes, cur->id))
                return false;
            idx--;
            cur = cur->parent;
        } else {
            /* Descendant combinator — walk up until a match is found */
            bool found = false;
            while (cur) {
                if (match_simple(&sel->parts[idx],
                                 (Ca_ElementType)cur->elem_type, cur->classes, cur->id)) {
                    found = true;
                    cur = cur->parent;
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
   Format: (id_count << 20) | (class_count << 10) | element_count */
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

void ca_style_resolve(Ca_Stylesheet *ss,
                      Ca_Node *node,
                      Ca_ElementType elem_type,
                      const char *classes,
                      Ca_ResolvedStyle *out)
{
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

    /* Apply declarations in order (last wins) */
    for (int m = 0; m < match_count; ++m) {
        for (int d = 0; d < matched[m].decl_count; ++d) {
            const Ca_CssDecl *decl = &matched[m].decls[d];
            Ca_CssPropId prop = decl->prop;
            const Ca_CssValue *val = &decl->value;

            if (val->type == CA_CSS_VAL_NONE) continue;

            out->set_mask |= (1ULL << prop);

            switch (prop) {
                case CA_CSS_PROP_WIDTH:            out->width           = css_val_to_px(val); break;
                case CA_CSS_PROP_HEIGHT:           out->height          = css_val_to_px(val); break;
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
                default: break;
            }
        }
    }
}

/* ============================================================
   APPLY STYLE TO NODE DESC
   ============================================================ */

#define STYLE_SET(prop) (style->set_mask & (1ULL << (prop)))

void ca_style_apply_to_node(const Ca_ResolvedStyle *style,
                            Ca_NodeDesc *nd,
                            uint32_t *out_color)
{
    if (!style || style->set_mask == 0) return;

    /* Sizing — CSS fills if NodeDesc is 0 (auto) */
    if (nd->width  <= 0.0f && STYLE_SET(CA_CSS_PROP_WIDTH))  nd->width  = style->width;
    if (nd->height <= 0.0f && STYLE_SET(CA_CSS_PROP_HEIGHT)) nd->height = style->height;
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

    /* Gap */
    if (nd->gap <= 0.0f && STYLE_SET(CA_CSS_PROP_GAP)) nd->gap = style->gap;

    /* Border radius */
    if (nd->corner_radius <= 0.0f && STYLE_SET(CA_CSS_PROP_BORDER_RADIUS))
        nd->corner_radius = style->border_radius;

    /* Background color — 0 = transparent = not set */
    if (nd->background == 0 && STYLE_SET(CA_CSS_PROP_BACKGROUND_COLOR))
        nd->background = style->background_color;

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
            default:                     nd->overflow_x = 0; break;
        }
    }
    if (STYLE_SET(CA_CSS_PROP_OVERFLOW_Y)) {
        switch (style->overflow_y) {
            case CA_CSS_OVERFLOW_HIDDEN:  nd->overflow_y = 1; break;
            case CA_CSS_OVERFLOW_SCROLL:  nd->overflow_y = 2; break;
            case CA_CSS_OVERFLOW_AUTO:    nd->overflow_y = 3; break;
            default:                     nd->overflow_y = 0; break;
        }
    }

    /* Flex grow/shrink */
    if (nd->flex_grow  <= 0.0f && STYLE_SET(CA_CSS_PROP_FLEX_GROW))  nd->flex_grow  = style->flex_grow;
    if (nd->flex_shrink <= 0.0f && STYLE_SET(CA_CSS_PROP_FLEX_SHRINK)) nd->flex_shrink = style->flex_shrink;

    /* Flex wrap */
    if (nd->flex_wrap == 0 && STYLE_SET(CA_CSS_PROP_FLEX_WRAP)) {
        if (style->flex_wrap == CA_CSS_WRAP_WRAP) nd->flex_wrap = 1;
    }

    /* Text/foreground color — output separately */
    if (out_color && *out_color == 0 && STYLE_SET(CA_CSS_PROP_COLOR))
        *out_color = style->color;

    /* Store transition config on the node via nd (will be copied later) */
    /* The caller (apply_css in widget.c) reads these via the Ca_Node pointer. */
}
