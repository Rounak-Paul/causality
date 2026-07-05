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
    CA_CSS_VAL_EM,         /* relative to current font-size */
    CA_CSS_VAL_REM,        /* relative to root font-size (16px default) */
    CA_CSS_VAL_VW,         /* viewport width percent */
    CA_CSS_VAL_VH,         /* viewport height percent */
    CA_CSS_VAL_CALC,       /* calc() — resolved to px at apply time */
    CA_CSS_VAL_INHERIT,       /* inherit from parent */
    CA_CSS_VAL_CURRENT_COLOR, /* currentColor — resolved from color property */
    CA_CSS_VAL_INITIAL,       /* initial / unset */
    /* Gradient value: color=start_color, keyword=gradient_type (linear/radial),
       number=angle (linear) or 0 (radial). A second decl carries color=end_color. */
    CA_CSS_VAL_GRADIENT,
} Ca_CssValType;

typedef struct {
    Ca_CssValType type;
    union {
        float    number;     /* CA_CSS_VAL_PX, CA_CSS_VAL_PERCENT, CA_CSS_VAL_NUMBER, EM, REM, VW, VH, CALC */
        uint32_t color;      /* CA_CSS_VAL_COLOR — RRGGBBAA packed */
    };
    int keyword;             /* CA_CSS_VAL_KEYWORD; string-pool offset for CA_CSS_VAL_VAR;
                                secondary integer payload for shorthands (e.g. transition
                                prop-id alongside number=duration). Separate from the union
                                so shorthands can carry both a numeric value and an int. */
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
    CA_CSS_PROP_ROW_GAP,
    CA_CSS_PROP_COLUMN_GAP,
    /* Flex */
    CA_CSS_PROP_DISPLAY,
    CA_CSS_PROP_FLEX_DIRECTION,
    CA_CSS_PROP_FLEX_WRAP,
    CA_CSS_PROP_ALIGN_ITEMS,
    CA_CSS_PROP_ALIGN_SELF,
    CA_CSS_PROP_ALIGN_CONTENT,
    CA_CSS_PROP_JUSTIFY_CONTENT,
    CA_CSS_PROP_JUSTIFY_SELF,
    CA_CSS_PROP_FLEX_GROW,
    CA_CSS_PROP_FLEX_SHRINK,
    CA_CSS_PROP_FLEX_BASIS,
    CA_CSS_PROP_ORDER,
    /* Visual */
    CA_CSS_PROP_BACKGROUND_COLOR,
    CA_CSS_PROP_COLOR,
    CA_CSS_PROP_BORDER_RADIUS,
    CA_CSS_PROP_BORDER_TOP_LEFT_RADIUS,
    CA_CSS_PROP_BORDER_TOP_RIGHT_RADIUS,
    CA_CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS,
    CA_CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS,
    CA_CSS_PROP_OPACITY,
    CA_CSS_PROP_VISIBILITY,
    /* Typography */
    CA_CSS_PROP_FONT_SIZE,
    CA_CSS_PROP_FONT_WEIGHT,
    CA_CSS_PROP_FONT_STYLE,
    CA_CSS_PROP_LINE_HEIGHT,
    CA_CSS_PROP_LETTER_SPACING,
    CA_CSS_PROP_WORD_SPACING,
    CA_CSS_PROP_TEXT_ALIGN,
    CA_CSS_PROP_TEXT_DECORATION,
    CA_CSS_PROP_TEXT_TRANSFORM,
    CA_CSS_PROP_WHITE_SPACE,
    /* Overflow */
    CA_CSS_PROP_OVERFLOW,
    CA_CSS_PROP_OVERFLOW_X,
    CA_CSS_PROP_OVERFLOW_Y,
    /* Transitions */
    CA_CSS_PROP_TRANSITION,
    /* Border — uniform */
    CA_CSS_PROP_BORDER_WIDTH,
    CA_CSS_PROP_BORDER_COLOR,
    CA_CSS_PROP_BORDER_STYLE,
    /* Border — per-side */
    CA_CSS_PROP_BORDER_TOP_WIDTH,
    CA_CSS_PROP_BORDER_TOP_COLOR,
    CA_CSS_PROP_BORDER_TOP_STYLE,
    CA_CSS_PROP_BORDER_RIGHT_WIDTH,
    CA_CSS_PROP_BORDER_RIGHT_COLOR,
    CA_CSS_PROP_BORDER_RIGHT_STYLE,
    CA_CSS_PROP_BORDER_BOTTOM_WIDTH,
    CA_CSS_PROP_BORDER_BOTTOM_COLOR,
    CA_CSS_PROP_BORDER_BOTTOM_STYLE,
    CA_CSS_PROP_BORDER_LEFT_WIDTH,
    CA_CSS_PROP_BORDER_LEFT_COLOR,
    CA_CSS_PROP_BORDER_LEFT_STYLE,
    /* Outline */
    CA_CSS_PROP_OUTLINE_WIDTH,
    CA_CSS_PROP_OUTLINE_COLOR,
    CA_CSS_PROP_OUTLINE_STYLE,
    CA_CSS_PROP_OUTLINE_OFFSET,
    /* Box shadow */
    CA_CSS_PROP_BOX_SHADOW,
    CA_CSS_PROP_SHADOW_OFFSET_X,
    CA_CSS_PROP_SHADOW_OFFSET_Y,
    CA_CSS_PROP_SHADOW_BLUR,
    CA_CSS_PROP_SHADOW_COLOR,
    /* Z-index */
    CA_CSS_PROP_Z_INDEX,
    /* Text wrapping */
    CA_CSS_PROP_TEXT_WRAP,
    /* Layout */
    CA_CSS_PROP_ASPECT_RATIO,
    CA_CSS_PROP_BOX_SIZING,
    /* Pointer / interaction */
    CA_CSS_PROP_CURSOR,
    CA_CSS_PROP_POINTER_EVENTS,
    CA_CSS_PROP_USER_SELECT,
    /* Scroll */
    CA_CSS_PROP_SCROLL_BEHAVIOR,
    CA_CSS_PROP_SCROLLBAR_WIDTH,
    CA_CSS_PROP_SCROLLBAR_TRACK_COLOR,
    CA_CSS_PROP_SCROLLBAR_THUMB_COLOR,
    CA_CSS_PROP_SCROLLBAR_THUMB_ACTIVE_COLOR,
    CA_CSS_PROP_SCROLLBAR_RADIUS,
    /* Background (handles gradients — `background` shorthand) */
    CA_CSS_PROP_BACKGROUND,
    /* Gradient parameters written as side-channel properties from the
       background shorthand parser; not set by the user directly. */
    CA_CSS_PROP_GRADIENT_COLOR2,   /* end color stop */
    CA_CSS_PROP_GRADIENT_ANGLE,    /* linear-gradient angle in degrees */
    CA_CSS_PROP_GRADIENT_CX,       /* radial center x (0..1) */
    CA_CSS_PROP_GRADIENT_CY,       /* radial center y (0..1) */

    /* Backdrop filter */
    CA_CSS_PROP_BACKDROP_FILTER,   /* backdrop-filter: blur(Xpx) */

    CA_CSS_PROP_COUNT
} Ca_CssPropId;

/* CSS keyword values */
typedef enum {
    /* display */
    CA_CSS_DISPLAY_FLEX = 0,
    CA_CSS_DISPLAY_BLOCK,
    CA_CSS_DISPLAY_NONE,
    CA_CSS_DISPLAY_INLINE,
    CA_CSS_DISPLAY_INLINE_FLEX,
    CA_CSS_DISPLAY_INLINE_BLOCK,
    CA_CSS_DISPLAY_GRID,
    /* flex-direction */
    CA_CSS_FLEX_ROW,
    CA_CSS_FLEX_COLUMN,
    CA_CSS_FLEX_ROW_REVERSE,
    CA_CSS_FLEX_COLUMN_REVERSE,
    /* flex-wrap */
    CA_CSS_WRAP_NOWRAP,
    CA_CSS_WRAP_WRAP,
    CA_CSS_WRAP_WRAP_REVERSE,
    /* alignment */
    CA_CSS_ALIGN_FLEX_START,
    CA_CSS_ALIGN_CENTER,
    CA_CSS_ALIGN_FLEX_END,
    CA_CSS_ALIGN_STRETCH,
    CA_CSS_ALIGN_SPACE_BETWEEN,
    CA_CSS_ALIGN_SPACE_AROUND,
    CA_CSS_ALIGN_SPACE_EVENLY,
    CA_CSS_ALIGN_BASELINE,
    CA_CSS_ALIGN_AUTO,
    CA_CSS_ALIGN_NORMAL,
    /* overflow */
    CA_CSS_OVERFLOW_VISIBLE,
    CA_CSS_OVERFLOW_HIDDEN,
    CA_CSS_OVERFLOW_SCROLL,
    CA_CSS_OVERFLOW_AUTO,
    CA_CSS_OVERFLOW_CLIP,
    /* text-align */
    CA_CSS_TEXT_ALIGN_LEFT,
    CA_CSS_TEXT_ALIGN_CENTER,
    CA_CSS_TEXT_ALIGN_RIGHT,
    CA_CSS_TEXT_ALIGN_START,
    CA_CSS_TEXT_ALIGN_END,
    CA_CSS_TEXT_ALIGN_JUSTIFY,
    /* font-weight */
    CA_CSS_FONT_WEIGHT_NORMAL,
    CA_CSS_FONT_WEIGHT_BOLD,
    CA_CSS_FONT_WEIGHT_LIGHTER,
    CA_CSS_FONT_WEIGHT_BOLDER,
    /* font-style */
    CA_CSS_FONT_STYLE_NORMAL,
    CA_CSS_FONT_STYLE_ITALIC,
    CA_CSS_FONT_STYLE_OBLIQUE,
    /* text-decoration */
    CA_CSS_TEXT_DECORATION_NONE,
    CA_CSS_TEXT_DECORATION_UNDERLINE,
    CA_CSS_TEXT_DECORATION_LINE_THROUGH,
    CA_CSS_TEXT_DECORATION_OVERLINE,
    /* text-transform */
    CA_CSS_TEXT_TRANSFORM_NONE,
    CA_CSS_TEXT_TRANSFORM_UPPERCASE,
    CA_CSS_TEXT_TRANSFORM_LOWERCASE,
    CA_CSS_TEXT_TRANSFORM_CAPITALIZE,
    /* white-space */
    CA_CSS_WHITE_SPACE_NORMAL,
    CA_CSS_WHITE_SPACE_NOWRAP,
    CA_CSS_WHITE_SPACE_PRE,
    CA_CSS_WHITE_SPACE_PRE_LINE,
    CA_CSS_WHITE_SPACE_PRE_WRAP,
    /* visibility */
    CA_CSS_VISIBILITY_VISIBLE,
    CA_CSS_VISIBILITY_HIDDEN,
    CA_CSS_VISIBILITY_COLLAPSE,
    /* border-style */
    CA_CSS_BORDER_NONE,
    CA_CSS_BORDER_SOLID,
    CA_CSS_BORDER_DASHED,
    CA_CSS_BORDER_DOTTED,
    CA_CSS_BORDER_DOUBLE,
    CA_CSS_BORDER_GROOVE,
    CA_CSS_BORDER_RIDGE,
    CA_CSS_BORDER_INSET,
    CA_CSS_BORDER_OUTSET,
    CA_CSS_BORDER_HIDDEN,
    /* box-sizing */
    CA_CSS_BOX_SIZING_CONTENT_BOX,
    CA_CSS_BOX_SIZING_BORDER_BOX,
    /* cursor */
    CA_CSS_CURSOR_AUTO,
    CA_CSS_CURSOR_DEFAULT,
    CA_CSS_CURSOR_POINTER,
    CA_CSS_CURSOR_CROSSHAIR,
    CA_CSS_CURSOR_MOVE,
    CA_CSS_CURSOR_TEXT,
    CA_CSS_CURSOR_WAIT,
    CA_CSS_CURSOR_HELP,
    CA_CSS_CURSOR_NOT_ALLOWED,
    CA_CSS_CURSOR_GRAB,
    CA_CSS_CURSOR_GRABBING,
    CA_CSS_CURSOR_EW_RESIZE,
    CA_CSS_CURSOR_NS_RESIZE,
    CA_CSS_CURSOR_NWSE_RESIZE,
    CA_CSS_CURSOR_NESW_RESIZE,
    CA_CSS_CURSOR_COL_RESIZE,
    CA_CSS_CURSOR_ROW_RESIZE,
    CA_CSS_CURSOR_NONE,
    /* pointer-events */
    CA_CSS_POINTER_EVENTS_AUTO,
    CA_CSS_POINTER_EVENTS_NONE,
    /* user-select */
    CA_CSS_USER_SELECT_AUTO,
    CA_CSS_USER_SELECT_NONE,
    CA_CSS_USER_SELECT_TEXT,
    CA_CSS_USER_SELECT_ALL,
    /* scroll-behavior */
    CA_CSS_SCROLL_AUTO,
    CA_CSS_SCROLL_SMOOTH,
    /* easing functions (stored as keyword in transition decl) */
    CA_CSS_EASE_LINEAR,
    CA_CSS_EASE_EASE,
    CA_CSS_EASE_EASE_IN,
    CA_CSS_EASE_EASE_OUT,
    CA_CSS_EASE_EASE_IN_OUT,
    CA_CSS_EASE_STEP_START,
    CA_CSS_EASE_STEP_END,
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
#define CA_CSS_MAX_DECLS_PER_RULE     96

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

/* Position-dependent selectors (structural pseudo-classes like :nth-child,
   or any multi-part selector chained via a combinator) can match
   differently for the SAME node depending on external state — sibling
   order/count, ancestor structure — that isn't reflected in the node's own
   classes/id/pseudo-state. A per-node resolved-style cache keyed only on
   local state would be unsound for those. Computed once at parse time
   (see classify_position_dependent_selectors in style.c): every class
   name that appears in the SUBJECT (rightmost) part of any such selector
   is recorded here. A node whose own classes don't intersect this set is
   safe to cache; a node that does must always be freshly resolved. */
#define CA_CSS_MAX_POS_DEP_CLASSES 32

typedef struct Ca_Stylesheet {
    Ca_CssRule rules[CA_CSS_MAX_RULES];
    int        rule_count;
    /* Root-scoped custom properties (var(--name) targets). */
    Ca_CssVar  vars[CA_CSS_MAX_VARS];
    int        var_count;
    /* String pool for var-name references inside Ca_CssValue.keyword. */
    char       str_pool[CA_CSS_STR_POOL_BYTES];
    int        str_pool_used;
    /* See CA_CSS_MAX_POS_DEP_CLASSES doc above. */
    char       pos_dep_classes[CA_CSS_MAX_POS_DEP_CLASSES][CA_CSS_CLASS_NAME_MAX];
    int        pos_dep_class_count;
    /* True if any position-dependent selector's subject has NO class at
       all (bare element/id/pseudo, e.g. "div:first-child") — in that case
       the per-class set above can't capture it, so the whole stylesheet
       falls back to "always resolve fresh" rather than risk missing it. */
    bool       pos_dep_classless_selector_exists;
} Ca_Stylesheet;

/* Helpers — intern/resolve strings in the stylesheet pool. */
int         ca_css_intern(Ca_Stylesheet *ss, const char *s); /* returns offset, -1 on overflow */
const char *ca_css_str(const Ca_Stylesheet *ss, int offset);

/* ============================================================
   API
   ============================================================ */

CA_API Ca_Stylesheet *ca_css_parse(const char *css_text);
CA_API void           ca_css_destroy(Ca_Stylesheet *ss);
