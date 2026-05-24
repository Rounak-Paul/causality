// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* css.h — CSS parser and stylesheet data structures */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ca_api.h"

/* ============================================================
   CSS VALUE TYPES
   ============================================================ */

typedef enum {
    CA_CSS_VAL_NONE = 0,
    CA_CSS_VAL_PX,
    CA_CSS_VAL_PERCENT,
    CA_CSS_VAL_AUTO,
    CA_CSS_VAL_COLOR,
    CA_CSS_VAL_KEYWORD,
    CA_CSS_VAL_NUMBER,
    CA_CSS_VAL_VAR,        /* unresolved var(--name) — `keyword` is offset into
                              the stylesheet string pool holding the var name. */
} Ca_CssValType;

typedef struct {
    Ca_CssValType type;
    union {
        float    number;
        uint32_t color;
        int      keyword;   /* also: string-pool offset when type == CA_CSS_VAL_VAR */
    };
} Ca_CssValue;

/* ============================================================
   CSS PROPERTY IDS
   ============================================================ */

typedef enum {
    CA_CSS_PROP_NONE = 0,
    /* Sizing */
    CA_CSS_PROP_WIDTH,
    CA_CSS_PROP_HEIGHT,
    CA_CSS_PROP_MIN_WIDTH,
    CA_CSS_PROP_MAX_WIDTH,
    CA_CSS_PROP_MIN_HEIGHT,
    CA_CSS_PROP_MAX_HEIGHT,
    /* Padding */
    CA_CSS_PROP_PADDING_TOP,
    CA_CSS_PROP_PADDING_RIGHT,
    CA_CSS_PROP_PADDING_BOTTOM,
    CA_CSS_PROP_PADDING_LEFT,
    /* Margin */
    CA_CSS_PROP_MARGIN_TOP,
    CA_CSS_PROP_MARGIN_RIGHT,
    CA_CSS_PROP_MARGIN_BOTTOM,
    CA_CSS_PROP_MARGIN_LEFT,
    /* Gap */
    CA_CSS_PROP_GAP,
    /* Flex */
    CA_CSS_PROP_DISPLAY,
    CA_CSS_PROP_FLEX_DIRECTION,
    CA_CSS_PROP_FLEX_WRAP,
    CA_CSS_PROP_ALIGN_ITEMS,
    CA_CSS_PROP_JUSTIFY_CONTENT,
    CA_CSS_PROP_FLEX_GROW,
    CA_CSS_PROP_FLEX_SHRINK,
    /* Visual */
    CA_CSS_PROP_BACKGROUND_COLOR,
    CA_CSS_PROP_COLOR,
    CA_CSS_PROP_BORDER_RADIUS,
    CA_CSS_PROP_OPACITY,
    /* Typography */
    CA_CSS_PROP_FONT_SIZE,
    CA_CSS_PROP_FONT_WEIGHT,
    CA_CSS_PROP_TEXT_ALIGN,
    /* Overflow */
    CA_CSS_PROP_OVERFLOW,
    CA_CSS_PROP_OVERFLOW_X,
    CA_CSS_PROP_OVERFLOW_Y,
    /* Transitions */
    CA_CSS_PROP_TRANSITION,
    /* Border */
    CA_CSS_PROP_BORDER_WIDTH,
    CA_CSS_PROP_BORDER_COLOR,
    /* Box shadow (shorthand parsed into individual values) */
    CA_CSS_PROP_BOX_SHADOW,
    /* Z-index */
    CA_CSS_PROP_Z_INDEX,
    /* Text wrapping */
    CA_CSS_PROP_TEXT_WRAP,
    CA_CSS_PROP_COUNT
} Ca_CssPropId;

/* CSS keyword values */
typedef enum {
    /* display */
    CA_CSS_DISPLAY_FLEX = 0,
    CA_CSS_DISPLAY_BLOCK,
    CA_CSS_DISPLAY_NONE,
    /* flex-direction */
    CA_CSS_FLEX_ROW,
    CA_CSS_FLEX_COLUMN,
    CA_CSS_FLEX_ROW_REVERSE,
    CA_CSS_FLEX_COLUMN_REVERSE,
    /* flex-wrap */
    CA_CSS_WRAP_NOWRAP,
    CA_CSS_WRAP_WRAP,
    /* alignment */
    CA_CSS_ALIGN_FLEX_START,
    CA_CSS_ALIGN_CENTER,
    CA_CSS_ALIGN_FLEX_END,
    CA_CSS_ALIGN_STRETCH,
    CA_CSS_ALIGN_SPACE_BETWEEN,
    CA_CSS_ALIGN_SPACE_AROUND,
    CA_CSS_ALIGN_SPACE_EVENLY,
    /* overflow */
    CA_CSS_OVERFLOW_VISIBLE,
    CA_CSS_OVERFLOW_HIDDEN,
    CA_CSS_OVERFLOW_SCROLL,
    CA_CSS_OVERFLOW_AUTO,
    /* text-align */
    CA_CSS_TEXT_ALIGN_LEFT,
    CA_CSS_TEXT_ALIGN_CENTER,
    CA_CSS_TEXT_ALIGN_RIGHT,
} Ca_CssKeyword;

/* ============================================================
   CSS DECLARATION
   ============================================================ */

typedef struct {
    Ca_CssPropId prop;
    Ca_CssValue  value;
    bool         important;   /* set when declaration ends with `!important` */
    /* For custom properties (--foo) we hijack `prop = CA_CSS_PROP_NONE` and
       store the variable name in `var_name`. These declarations are NOT
       cascaded onto the node; instead the parser hoists them into the
       stylesheet's `vars` table when seen inside the `:root` rule. */
    char         var_name[64];
} Ca_CssDecl;

/* ============================================================
   CSS SELECTORS
   ============================================================ */

#define CA_CSS_MAX_CLASSES_SEL 8
#define CA_CSS_CLASS_NAME_MAX  64

typedef enum {
    CA_CSS_COMB_NONE = 0,
    CA_CSS_COMB_DESCENDANT,    /* whitespace */
    CA_CSS_COMB_CHILD,         /* '>'        */
    CA_CSS_COMB_NEXT_SIBLING,  /* '+'        */
    CA_CSS_COMB_SUBSEQ_SIBLING,/* '~'        */
} Ca_CssCombinator;

/* ============================================================
   PSEUDO-CLASSES
   ============================================================ */

typedef enum {
    CA_CSS_PSEUDO_NONE = 0,
    CA_CSS_PSEUDO_HOVER,
    CA_CSS_PSEUDO_ACTIVE,
    CA_CSS_PSEUDO_FOCUS,
    CA_CSS_PSEUDO_FOCUS_WITHIN,
    CA_CSS_PSEUDO_DISABLED,
    CA_CSS_PSEUDO_ENABLED,
    CA_CSS_PSEUDO_CHECKED,
    CA_CSS_PSEUDO_FIRST_CHILD,
    CA_CSS_PSEUDO_LAST_CHILD,
    CA_CSS_PSEUDO_ONLY_CHILD,
    CA_CSS_PSEUDO_FIRST_OF_TYPE,
    CA_CSS_PSEUDO_LAST_OF_TYPE,
    CA_CSS_PSEUDO_NTH_CHILD,      /* An+B */
    CA_CSS_PSEUDO_NTH_LAST_CHILD, /* An+B counted from end */
    CA_CSS_PSEUDO_NOT,            /* :not(simple) */
    CA_CSS_PSEUDO_ROOT,           /* :root — node with no parent */
    CA_CSS_PSEUDO_EMPTY,          /* :empty — no children */
} Ca_CssPseudoKind;

/* A single pseudo-class entry. For :not() we keep one negated simple selector
   inline (no recursion, no compound :not). For :nth-child(An+B) we keep the
   coefficients. */
typedef struct {
    Ca_CssPseudoKind kind;
    int  a, b;                              /* :nth-child(An+B) */
    /* :not(simple) payload */
    char not_element[32];
    char not_id[CA_CSS_CLASS_NAME_MAX];
    char not_class[CA_CSS_CLASS_NAME_MAX];
    Ca_CssPseudoKind not_pseudo;            /* e.g. :not(:hover) */
} Ca_CssPseudo;

#define CA_CSS_MAX_PSEUDOS_PER_PART 4

/* A single simple selector (e.g. div#my-id.foo.bar:hover:not(.disabled)) */
typedef struct {
    char     element[32];
    char     id[CA_CSS_CLASS_NAME_MAX]; /* e.g. #my-id → "my-id" */
    char     classes[CA_CSS_MAX_CLASSES_SEL][CA_CSS_CLASS_NAME_MAX];
    int      class_count;
    Ca_CssPseudo pseudos[CA_CSS_MAX_PSEUDOS_PER_PART];
    int      pseudo_count;
    Ca_CssCombinator combinator;  /* how this relates to PREVIOUS part */
} Ca_CssSimpleSel;

/* A compound selector = chain of simple selectors
   (read right-to-left: parts[part_count-1] is the subject) */
#define CA_CSS_MAX_CHAIN 8

typedef struct {
    Ca_CssSimpleSel parts[CA_CSS_MAX_CHAIN];
    int             part_count;
} Ca_CssSelector;

/* ============================================================
   CSS RULE
   ============================================================ */

#define CA_CSS_MAX_SELECTORS_PER_RULE 16
#define CA_CSS_MAX_DECLS_PER_RULE     64

typedef struct {
    Ca_CssSelector selectors[CA_CSS_MAX_SELECTORS_PER_RULE];
    int            selector_count;
    Ca_CssDecl     decls[CA_CSS_MAX_DECLS_PER_RULE];
    int            decl_count;
    int            source_order;
} Ca_CssRule;

/* ============================================================
   CSS STYLESHEET
   ============================================================ */

#define CA_CSS_MAX_RULES 1024

/* Custom-property storage. Root-scoped only (:root { --name: value; }).
   Per-element overrides would require per-node resolution tables — out of
   scope for this revision. */
#define CA_CSS_MAX_VARS       128
#define CA_CSS_VAR_NAME_MAX   64
#define CA_CSS_STR_POOL_BYTES 8192

typedef struct {
    char        name[CA_CSS_VAR_NAME_MAX];
    Ca_CssValue value;
} Ca_CssVar;

typedef struct Ca_Stylesheet {
    Ca_CssRule rules[CA_CSS_MAX_RULES];
    int        rule_count;
    /* Root-scoped custom properties (var(--name) targets). */
    Ca_CssVar  vars[CA_CSS_MAX_VARS];
    int        var_count;
    /* String pool for var-name references inside Ca_CssValue.keyword. */
    char       str_pool[CA_CSS_STR_POOL_BYTES];
    int        str_pool_used;
} Ca_Stylesheet;

/* Helpers — intern/resolve strings in the stylesheet pool. */
int         ca_css_intern(Ca_Stylesheet *ss, const char *s); /* returns offset, -1 on overflow */
const char *ca_css_str(const Ca_Stylesheet *ss, int offset);

/* ============================================================
   API
   ============================================================ */

CA_API Ca_Stylesheet *ca_css_parse(const char *css_text);
CA_API void           ca_css_destroy(Ca_Stylesheet *ss);
