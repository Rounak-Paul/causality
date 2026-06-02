// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* css.c — CSS tokenizer + recursive descent parser */
#include "css.h"
#include "causality_config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ============================================================
   TOKENIZER
   ============================================================ */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_HASH,       /* #rrggbb / #rgb */
    TOK_NUMBER,     /* e.g. 10, 10.5 */
    TOK_DIMENSION,  /* number + unit, e.g. 10px, 50% */
    TOK_STRING,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_COMMA,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_DOT,
    TOK_STAR,
    TOK_GT,
    TOK_PLUS,       /* '+' — sibling combinator or sign */
    TOK_TILDE,      /* '~' — sibling combinator */
    TOK_BANG,       /* '!' — introduces `important` */
    TOK_MINUS,      /* '-' — only emitted in selector-context: --vars start with -- */
    TOK_DBLDASH,    /* '--' — start of a custom-property name */
    TOK_WS,         /* significant whitespace (descendant combinator) */
    TOK_FUNCTION,   /* ident( — e.g. rgb( */
} TokType;

typedef struct {
    TokType type;
    char    text[256];
    float   number;
    char    unit[16];
} Token;

typedef struct {
    const char *src;
    int         pos;
    int         len;
} Lexer;

static void lexer_init(Lexer *lex, const char *src)
{
    lex->src = src;
    lex->pos = 0;
    lex->len = (int)strlen(src);
}

static char peek(const Lexer *lex)
{
    return (lex->pos < lex->len) ? lex->src[lex->pos] : '\0';
}

static char advance(Lexer *lex)
{
    if (lex->pos < lex->len) return lex->src[lex->pos++];
    return '\0';
}

static void skip_comment(Lexer *lex)
{
    /* Already consumed '/' — check for '*' */
    if (peek(lex) == '*') {
        advance(lex);
        while (lex->pos < lex->len) {
            if (lex->src[lex->pos] == '*' && lex->pos + 1 < lex->len &&
                lex->src[lex->pos + 1] == '/') {
                lex->pos += 2;
                return;
            }
            lex->pos++;
        }
    }
}

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || c == '-';
}

static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static Token next_token(Lexer *lex)
{
    Token tok = {0};

    /* Skip whitespace, tracking if any was found */
    bool had_ws = false;
    while (lex->pos < lex->len) {
        char c = peek(lex);
        if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '*') {
            advance(lex);
            skip_comment(lex);
            had_ws = true;
        } else if (isspace((unsigned char)c)) {
            advance(lex);
            had_ws = true;
        } else {
            break;
        }
    }

    if (lex->pos >= lex->len) { tok.type = TOK_EOF; return tok; }

    char c = peek(lex);

    /* Significant whitespace (for descendant combinator) — only between
       selectors, not around punctuation. We return it, and the parser
       decides whether to use it. */
    if (had_ws && is_ident_start(c)) {
        /* Don't consume the ident yet; return WS token */
        tok.type = TOK_WS;
        tok.text[0] = ' ';
        tok.text[1] = '\0';
        return tok;
    }
    if (had_ws && (c == '.' || c == '#' || c == '*' || c == ':' ||
                   c == '[' || c == '+' || c == '~')) {
        tok.type = TOK_WS;
        tok.text[0] = ' ';
        tok.text[1] = '\0';
        return tok;
    }

    /* Custom-property name starts with `--` (only treated as such in
       declaration context — the property-name parser checks for this). */
    if (c == '-' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '-') {
        /* Lex as identifier so the parser can use lookup_property() and
           detect the `--` prefix uniformly. */
        int i = 0;
        while (i < 255 && (is_ident_char(peek(lex)) || peek(lex) == '-'))
            tok.text[i++] = advance(lex);
        tok.text[i] = '\0';
        tok.type = TOK_IDENT;
        return tok;
    }

    /* Hash (#color or #id) */
    if (c == '#') {
        advance(lex);
        int i = 0;
        while (i < 255 && isxdigit((unsigned char)peek(lex)))
            tok.text[i++] = advance(lex);
        tok.text[i] = '\0';
        tok.type = TOK_HASH;
        return tok;
    }

    /* Number / dimension */
    if (isdigit((unsigned char)c) || (c == '.' && lex->pos + 1 < lex->len &&
        isdigit((unsigned char)lex->src[lex->pos + 1]))) {
        int i = 0;
        bool dot = false;
        while (i < 255) {
            char d = peek(lex);
            if (isdigit((unsigned char)d)) { tok.text[i++] = advance(lex); }
            else if (d == '.' && !dot) { dot = true; tok.text[i++] = advance(lex); }
            else break;
        }
        tok.text[i] = '\0';
        tok.number = (float)atof(tok.text);

        /* Check for unit */
        int u = 0;
        while (u < 15 && isalpha((unsigned char)peek(lex)))
            tok.unit[u++] = advance(lex);
        tok.unit[u] = '\0';

        /* Check for % */
        if (u == 0 && peek(lex) == '%') {
            tok.unit[0] = '%';
            tok.unit[1] = '\0';
            advance(lex);
        }

        tok.type = (tok.unit[0] != '\0') ? TOK_DIMENSION : TOK_NUMBER;
        return tok;
    }

    /* Identifier or function */
    if (is_ident_start(c)) {
        int i = 0;
        while (i < 255 && is_ident_char(peek(lex)))
            tok.text[i++] = advance(lex);
        tok.text[i] = '\0';

        /* Check for function: ident( */
        if (peek(lex) == '(') {
            advance(lex);
            tok.type = TOK_FUNCTION;
        } else {
            tok.type = TOK_IDENT;
        }
        return tok;
    }

    /* String */
    if (c == '"' || c == '\'') {
        char quote = advance(lex);
        int i = 0;
        while (i < 255 && peek(lex) != quote && peek(lex) != '\0')
            tok.text[i++] = advance(lex);
        tok.text[i] = '\0';
        if (peek(lex) == quote) advance(lex);
        tok.type = TOK_STRING;
        return tok;
    }

    /* Single character tokens */
    advance(lex);
    tok.text[0] = c;
    tok.text[1] = '\0';
    switch (c) {
        case ':': tok.type = TOK_COLON;     break;
        case ';': tok.type = TOK_SEMICOLON; break;
        case ',': tok.type = TOK_COMMA;     break;
        case '{': tok.type = TOK_LBRACE;    break;
        case '}': tok.type = TOK_RBRACE;    break;
        case '(': tok.type = TOK_LPAREN;    break;
        case ')': tok.type = TOK_RPAREN;    break;
        case '[': tok.type = TOK_LBRACKET;  break;
        case ']': tok.type = TOK_RBRACKET;  break;
        case '.': tok.type = TOK_DOT;       break;
        case '*': tok.type = TOK_STAR;      break;
        case '>': tok.type = TOK_GT;        break;
        case '+': tok.type = TOK_PLUS;      break;
        case '~': tok.type = TOK_TILDE;     break;
        case '!': tok.type = TOK_BANG;      break;
        default:  tok.type = TOK_EOF;       break; /* unexpected char, skip */
    }
    return tok;
}

/* ============================================================
   TOKEN BUFFER — allows lookahead + ungetting
   ============================================================ */

#define TOK_BUF_SIZE 4

typedef struct {
    Lexer lex;
    Token buffer[TOK_BUF_SIZE];
    int   buf_count;
    /* Current stylesheet — needed by parse_value to intern var() names
       and by parse_declarations to hoist :root custom properties. */
    Ca_Stylesheet *ss;
    /* True while parsing declarations of a rule whose selector list is
       exactly `:root`. Custom properties are only hoisted then. */
    bool           in_root_rule;
} Parser;

static void parser_init(Parser *p, const char *src)
{
    lexer_init(&p->lex, src);
    p->buf_count    = 0;
    p->ss           = NULL;
    p->in_root_rule = false;
}

static Token parser_next(Parser *p)
{
    if (p->buf_count > 0)
        return p->buffer[--p->buf_count];
    return next_token(&p->lex);
}

static Token parser_peek(Parser *p)
{
    Token t = parser_next(p);
    p->buffer[p->buf_count++] = t;
    return t;
}

static void parser_unget(Parser *p, Token t)
{
    if (p->buf_count < TOK_BUF_SIZE)
        p->buffer[p->buf_count++] = t;
}

static bool parser_expect(Parser *p, TokType type)
{
    Token t = parser_next(p);
    return t.type == type;
}

/* Skip WS tokens */
static void skip_ws(Parser *p)
{
    while (parser_peek(p).type == TOK_WS)
        parser_next(p);
}

/* ============================================================
   COLOR PARSING
   ============================================================ */

static uint32_t parse_hex_color(const char *hex)
{
    int len = (int)strlen(hex);
    uint32_t r = 0, g = 0, b = 0, a = 255;

    if (len == 3) {
        /* #RGB → #RRGGBB */
        sscanf(hex, "%1x%1x%1x", &r, &g, &b);
        r = r * 17; g = g * 17; b = b * 17;
    } else if (len == 4) {
        /* #RGBA */
        sscanf(hex, "%1x%1x%1x%1x", &r, &g, &b, &a);
        r = r * 17; g = g * 17; b = b * 17; a = a * 17;
    } else if (len == 6) {
        sscanf(hex, "%2x%2x%2x", &r, &g, &b);
    } else if (len == 8) {
        sscanf(hex, "%2x%2x%2x%2x", &r, &g, &b, &a);
    }

    return (r << 24) | (g << 16) | (b << 8) | a;
}

/* Named CSS colors — just the common ones */
static bool lookup_named_color(const char *name, uint32_t *out)
{
    struct { const char *n; uint32_t c; } colors[] = {
        { "transparent", 0x00000000 },
        { "black",       0x000000FF },
        { "white",       0xFFFFFFFF },
        { "red",         0xFF0000FF },
        { "green",       0x008000FF },
        { "blue",        0x0000FFFF },
        { "yellow",      0xFFFF00FF },
        { "cyan",        0x00FFFFFF },
        { "magenta",     0xFF00FFFF },
        { "orange",      0xFFA500FF },
        { "purple",      0x800080FF },
        { "pink",        0xFFC0CBFF },
        { "grey",        0x808080FF },
        { "gray",        0x808080FF },
        { "darkgray",    0xA9A9A9FF },
        { "darkgrey",    0xA9A9A9FF },
        { "lightgray",   0xD3D3D3FF },
        { "lightgrey",   0xD3D3D3FF },
        { "silver",      0xC0C0C0FF },
        { "navy",        0x000080FF },
        { "teal",        0x008080FF },
        { "maroon",      0x800000FF },
        { "olive",       0x808000FF },
        { "lime",        0x00FF00FF },
        { "aqua",        0x00FFFFFF },
        { "fuchsia",     0xFF00FFFF },
        { "coral",       0xFF7F50FF },
        { "tomato",      0xFF6347FF },
        { "gold",        0xFFD700FF },
        { "indigo",      0x4B0082FF },
        { "violet",      0xEE82EEFF },
        { "brown",       0xA52A2AFF },
        { "wheat",       0xF5DEB3FF },
        { "ivory",       0xFFFFF0FF },
        { "beige",       0xF5F5DCFF },
        { "linen",       0xFAF0E6FF },
        { "salmon",      0xFA8072FF },
        { "khaki",       0xF0E68CFF },
        { "plum",        0xDDA0DDFF },
        { "orchid",      0xDA70D6FF },
        { "tan",         0xD2B48CFF },
        { "crimson",     0xDC143CFF },
        { "turquoise",   0x40E0D0FF },
        { "steelblue",   0x4682B4FF },
        { "slategray",   0x708090FF },
        { "slategrey",   0x708090FF },
        { "skyblue",     0x87CEEBFF },
        { "royalblue",   0x4169E1FF },
        { "dodgerblue",  0x1E90FFFF },
        { "firebrick",   0xB22222FF },
        { "forestgreen", 0x228B22FF },
        { "seagreen",    0x2E8B57FF },
        { "darkblue",    0x00008BFF },
        { "darkgreen",   0x006400FF },
        { "darkred",     0x8B0000FF },
    };
    int count = (int)(sizeof(colors) / sizeof(colors[0]));

    /* Case-insensitive comparison */
    for (int i = 0; i < count; ++i) {
        if (strcasecmp(name, colors[i].n) == 0) {
            *out = colors[i].c;
            return true;
        }
    }
    return false;
}

/* Parse rgb(r,g,b) or rgba(r,g,b,a) — parser already consumed the function token */
static uint32_t parse_rgb_func(Parser *p, bool has_alpha)
{
    float vals[4] = { 0, 0, 0, 1.0f };
    int max_args = has_alpha ? 4 : 3;

    for (int i = 0; i < max_args; ++i) {
        skip_ws(p);
        Token t = parser_next(p);
        if (t.type == TOK_NUMBER || t.type == TOK_DIMENSION)
            vals[i] = t.number;
        skip_ws(p);
        Token comma = parser_next(p);
        if (comma.type == TOK_RPAREN) break;
        if (comma.type != TOK_COMMA && comma.type != TOK_RPAREN)
            ; /* skip bad tokens */
    }

    /* Consume trailing ) if not consumed */
    Token t = parser_peek(p);
    if (t.type == TOK_RPAREN) parser_next(p);

    uint32_t r = (uint32_t)(vals[0] > 255 ? 255 : (vals[0] < 0 ? 0 : vals[0]));
    uint32_t g = (uint32_t)(vals[1] > 255 ? 255 : (vals[1] < 0 ? 0 : vals[1]));
    uint32_t b = (uint32_t)(vals[2] > 255 ? 255 : (vals[2] < 0 ? 0 : vals[2]));
    uint32_t a = (uint32_t)(vals[3] * 255.0f);
    if (a > 255) a = 255;

    return (r << 24) | (g << 16) | (b << 8) | a;
}

/* HSL -> RGB conversion. h: 0..360 degrees, s/l: 0..1 fractions. */
static void hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b)
{
    /* Normalise hue into [0,360) */
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;

    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x  = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r1=0,g1=0,b1=0;
    if      (hp < 1) { r1 = c; g1 = x; }
    else if (hp < 2) { r1 = x; g1 = c; }
    else if (hp < 3) { g1 = c; b1 = x; }
    else if (hp < 4) { g1 = x; b1 = c; }
    else if (hp < 5) { r1 = x; b1 = c; }
    else             { r1 = c; b1 = x; }
    float m = l - c * 0.5f;
    *r = r1 + m; *g = g1 + m; *b = b1 + m;
}

/* Parse hsl(h, s%, l%) or hsla(...). */
static uint32_t parse_hsl_func(Parser *p, bool has_alpha)
{
    float h = 0, s = 0, l = 0, a = 1.0f;
    int max_args = has_alpha ? 4 : 3;
    float vals[4] = { 0, 0, 0, 1.0f };

    for (int i = 0; i < max_args; ++i) {
        skip_ws(p);
        Token t = parser_next(p);
        if (t.type == TOK_NUMBER || t.type == TOK_DIMENSION) {
            vals[i] = t.number;
            /* Saturation / lightness are typically percentages; if the token
               carries a % unit it's already a percentage — we'll divide by
               100 below. Hue may carry `deg` which we treat as degrees. */
            if (i == 1 || i == 2) {
                if (t.type == TOK_DIMENSION && strcmp(t.unit, "%") == 0)
                    vals[i] /= 100.0f;
            }
        }
        skip_ws(p);
        Token comma = parser_next(p);
        if (comma.type == TOK_RPAREN) break;
    }
    /* Consume trailing ) if still present */
    Token tt = parser_peek(p);
    if (tt.type == TOK_RPAREN) parser_next(p);

    h = vals[0]; s = vals[1]; l = vals[2]; a = has_alpha ? vals[3] : 1.0f;
    if (s < 0) s = 0; if (s > 1) s = 1;
    if (l < 0) l = 0; if (l > 1) l = 1;
    if (a < 0) a = 0; if (a > 1) a = 1;

    float rf, gf, bf;
    hsl_to_rgb(h, s, l, &rf, &gf, &bf);
    uint32_t r = (uint32_t)(rf * 255.0f + 0.5f); if (r > 255) r = 255;
    uint32_t g = (uint32_t)(gf * 255.0f + 0.5f); if (g > 255) g = 255;
    uint32_t b = (uint32_t)(bf * 255.0f + 0.5f); if (b > 255) b = 255;
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f); if (ai > 255) ai = 255;
    return (r << 24) | (g << 16) | (b << 8) | ai;
}

/* ============================================================
   STRING POOL (intern var-name strings in stylesheet)
   ============================================================ */

int ca_css_intern(Ca_Stylesheet *ss, const char *s)
{
    if (!ss || !s) return -1;
    int len = (int)strlen(s);
    /* Search for an existing copy. */
    int i = 0;
    while (i < ss->str_pool_used) {
        int rem = ss->str_pool_used - i;
        int sl  = (int)strnlen(ss->str_pool + i, rem);
        if (sl == len && strcmp(ss->str_pool + i, s) == 0)
            return i;
        i += sl + 1;
        if (sl == rem) break; /* shouldn't happen — pool always NUL-terminated */
    }
    if (ss->str_pool_used + len + 1 > CA_CSS_STR_POOL_BYTES) return -1;
    int offset = ss->str_pool_used;
    memcpy(ss->str_pool + offset, s, len + 1);
    ss->str_pool_used += len + 1;
    return offset;
}

const char *ca_css_str(const Ca_Stylesheet *ss, int offset)
{
    if (!ss || offset < 0 || offset >= ss->str_pool_used) return NULL;
    return ss->str_pool + offset;
}

/* ============================================================
   PROPERTY NAME LOOKUP
   ============================================================ */

static Ca_CssPropId lookup_property(const char *name)
{
    struct { const char *n; Ca_CssPropId id; } props[] = {
        { "width",            CA_CSS_PROP_WIDTH },
        { "height",           CA_CSS_PROP_HEIGHT },
        { "min-width",        CA_CSS_PROP_MIN_WIDTH },
        { "max-width",        CA_CSS_PROP_MAX_WIDTH },
        { "min-height",       CA_CSS_PROP_MIN_HEIGHT },
        { "max-height",       CA_CSS_PROP_MAX_HEIGHT },
        { "padding-top",      CA_CSS_PROP_PADDING_TOP },
        { "padding-right",    CA_CSS_PROP_PADDING_RIGHT },
        { "padding-bottom",   CA_CSS_PROP_PADDING_BOTTOM },
        { "padding-left",     CA_CSS_PROP_PADDING_LEFT },
        { "margin-top",       CA_CSS_PROP_MARGIN_TOP },
        { "margin-right",     CA_CSS_PROP_MARGIN_RIGHT },
        { "margin-bottom",    CA_CSS_PROP_MARGIN_BOTTOM },
        { "margin-left",      CA_CSS_PROP_MARGIN_LEFT },
        { "gap",              CA_CSS_PROP_GAP },
        { "display",          CA_CSS_PROP_DISPLAY },
        { "flex-direction",   CA_CSS_PROP_FLEX_DIRECTION },
        { "flex-wrap",        CA_CSS_PROP_FLEX_WRAP },
        { "align-items",      CA_CSS_PROP_ALIGN_ITEMS },
        { "justify-content",  CA_CSS_PROP_JUSTIFY_CONTENT },
        { "flex-grow",        CA_CSS_PROP_FLEX_GROW },
        { "flex-shrink",      CA_CSS_PROP_FLEX_SHRINK },
        { "background-color", CA_CSS_PROP_BACKGROUND_COLOR },
        { "background",       CA_CSS_PROP_BACKGROUND_COLOR },
        { "color",            CA_CSS_PROP_COLOR },
        { "border-radius",    CA_CSS_PROP_BORDER_RADIUS },
        { "opacity",          CA_CSS_PROP_OPACITY },
        { "font-size",        CA_CSS_PROP_FONT_SIZE },
        { "font-weight",      CA_CSS_PROP_FONT_WEIGHT },
        { "text-align",       CA_CSS_PROP_TEXT_ALIGN },
        { "overflow",         CA_CSS_PROP_OVERFLOW },
        { "overflow-x",       CA_CSS_PROP_OVERFLOW_X },
        { "overflow-y",       CA_CSS_PROP_OVERFLOW_Y },
        { "border-width",        CA_CSS_PROP_BORDER_WIDTH },
        { "border-color",        CA_CSS_PROP_BORDER_COLOR },
        { "border-top-width",    CA_CSS_PROP_BORDER_TOP_WIDTH },
        { "border-top-color",    CA_CSS_PROP_BORDER_TOP_COLOR },
        { "border-right-width",  CA_CSS_PROP_BORDER_RIGHT_WIDTH },
        { "border-right-color",  CA_CSS_PROP_BORDER_RIGHT_COLOR },
        { "border-bottom-width", CA_CSS_PROP_BORDER_BOTTOM_WIDTH },
        { "border-bottom-color", CA_CSS_PROP_BORDER_BOTTOM_COLOR },
        { "border-left-width",   CA_CSS_PROP_BORDER_LEFT_WIDTH },
        { "border-left-color",   CA_CSS_PROP_BORDER_LEFT_COLOR },
        { "box-shadow",          CA_CSS_PROP_BOX_SHADOW },
        { "z-index",          CA_CSS_PROP_Z_INDEX },
        { "text-wrap",        CA_CSS_PROP_TEXT_WRAP },
    };
    int count = (int)(sizeof(props) / sizeof(props[0]));
    for (int i = 0; i < count; ++i) {
        if (strcasecmp(name, props[i].n) == 0)
            return props[i].id;
    }
    return CA_CSS_PROP_NONE;
}

/* ============================================================
   KEYWORD LOOKUP
   ============================================================ */

typedef struct { const char *n; int val; } Ca_KwEntry;

static bool lookup_keyword(const char *name, Ca_CssPropId prop, int *out)
{
    Ca_KwEntry *tbl = NULL;
    int count = 0;

    /* display */
    static Ca_KwEntry display_kw[] = {
        { "flex",  CA_CSS_DISPLAY_FLEX },
        { "block", CA_CSS_DISPLAY_BLOCK },
        { "none",  CA_CSS_DISPLAY_NONE },
    };
    /* flex-direction */
    static Ca_KwEntry flexdir_kw[] = {
        { "row",            CA_CSS_FLEX_ROW },
        { "column",         CA_CSS_FLEX_COLUMN },
        { "row-reverse",    CA_CSS_FLEX_ROW_REVERSE },
        { "column-reverse", CA_CSS_FLEX_COLUMN_REVERSE },
    };
    /* flex-wrap */
    static Ca_KwEntry wrap_kw[] = {
        { "nowrap", CA_CSS_WRAP_NOWRAP },
        { "wrap",   CA_CSS_WRAP_WRAP },
    };
    /* align-items */
    static Ca_KwEntry align_kw[] = {
        { "flex-start", CA_CSS_ALIGN_FLEX_START },
        { "start",      CA_CSS_ALIGN_FLEX_START },
        { "center",     CA_CSS_ALIGN_CENTER },
        { "flex-end",   CA_CSS_ALIGN_FLEX_END },
        { "end",        CA_CSS_ALIGN_FLEX_END },
        { "stretch",    CA_CSS_ALIGN_STRETCH },
    };
    /* justify-content */
    static Ca_KwEntry justify_kw[] = {
        { "flex-start",     CA_CSS_ALIGN_FLEX_START },
        { "start",          CA_CSS_ALIGN_FLEX_START },
        { "center",         CA_CSS_ALIGN_CENTER },
        { "flex-end",       CA_CSS_ALIGN_FLEX_END },
        { "end",            CA_CSS_ALIGN_FLEX_END },
        { "space-between",  CA_CSS_ALIGN_SPACE_BETWEEN },
        { "space-around",   CA_CSS_ALIGN_SPACE_AROUND },
        { "space-evenly",   CA_CSS_ALIGN_SPACE_EVENLY },
    };
    /* overflow */
    static Ca_KwEntry overflow_kw[] = {
        { "visible", CA_CSS_OVERFLOW_VISIBLE },
        { "hidden",  CA_CSS_OVERFLOW_HIDDEN },
        { "scroll",  CA_CSS_OVERFLOW_SCROLL },
        { "auto",    CA_CSS_OVERFLOW_AUTO },
    };
    /* font-weight */
    static Ca_KwEntry fontweight_kw[] = {
        { "normal", 0 },
        { "bold",   1 },
        { "100",    0 }, { "200", 0 }, { "300", 0 }, { "400", 0 },
        { "500",    0 }, { "600", 1 }, { "700", 1 }, { "800", 1 }, { "900", 1 },
    };
    /* text-align */
    static Ca_KwEntry textalign_kw[] = {
        { "left",   CA_CSS_TEXT_ALIGN_LEFT },
        { "center", CA_CSS_TEXT_ALIGN_CENTER },
        { "right",  CA_CSS_TEXT_ALIGN_RIGHT },
    };

    switch (prop) {
        case CA_CSS_PROP_DISPLAY:
            tbl = display_kw; count = 3; break;
        case CA_CSS_PROP_FLEX_DIRECTION:
            tbl = flexdir_kw; count = 4; break;
        case CA_CSS_PROP_FLEX_WRAP:
            tbl = wrap_kw; count = 2; break;
        case CA_CSS_PROP_ALIGN_ITEMS:
            tbl = align_kw; count = 6; break;
        case CA_CSS_PROP_JUSTIFY_CONTENT:
            tbl = justify_kw; count = 8; break;
        case CA_CSS_PROP_OVERFLOW:
        case CA_CSS_PROP_OVERFLOW_X:
        case CA_CSS_PROP_OVERFLOW_Y:
            tbl = overflow_kw; count = 4; break;
        case CA_CSS_PROP_TEXT_ALIGN:
            tbl = textalign_kw; count = 3; break;
        case CA_CSS_PROP_FONT_WEIGHT:
            tbl = fontweight_kw; count = 10; break;
        case CA_CSS_PROP_TEXT_WRAP:
            tbl = wrap_kw; count = 2; break;
        default: return false;
    }

    for (int i = 0; i < count; ++i) {
        if (strcasecmp(name, tbl[i].n) == 0) {
            *out = tbl[i].val;
            return true;
        }
    }
    return false;
}

/* ============================================================
   PARSE A SINGLE VALUE
   ============================================================ */

static Ca_CssValue parse_value(Parser *p, Ca_CssPropId prop)
{
    Ca_CssValue val = {0};
    skip_ws(p);
    Token t = parser_next(p);

    if (t.type == TOK_HASH) {
        val.type  = CA_CSS_VAL_COLOR;
        val.color = parse_hex_color(t.text);
        return val;
    }

    if (t.type == TOK_FUNCTION) {
        if (strcasecmp(t.text, "rgb") == 0) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = parse_rgb_func(p, false);
            return val;
        }
        if (strcasecmp(t.text, "rgba") == 0) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = parse_rgb_func(p, true);
            return val;
        }
        if (strcasecmp(t.text, "hsl") == 0) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = parse_hsl_func(p, false);
            return val;
        }
        if (strcasecmp(t.text, "hsla") == 0) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = parse_hsl_func(p, true);
            return val;
        }
        if (strcasecmp(t.text, "var") == 0) {
            /* var(--name [, fallback]) */
            skip_ws(p);
            Token nm = parser_next(p);
            char varname[CA_CSS_VAR_NAME_MAX];
            varname[0] = '\0';
            if (nm.type == TOK_IDENT && nm.text[0] == '-' && nm.text[1] == '-') {
                snprintf(varname, sizeof(varname), "%s", nm.text);
            }
            /* Skip optional fallback (not retained \u2014 v1 returns 0 on miss). */
            int depth = 1;
            while (depth > 0) {
                Token tt = parser_next(p);
                if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
                else if (tt.type == TOK_RPAREN) depth--;
                else if (tt.type == TOK_EOF) break;
            }
            int offset = -1;
            if (p->ss && varname[0])
                offset = ca_css_intern(p->ss, varname);
            val.type    = CA_CSS_VAL_VAR;
            val.keyword = offset;
            return val;
        }
        /* Unknown function — skip to closing paren */
        int depth = 1;
        while (depth > 0) {
            Token tt = parser_next(p);
            if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
            else if (tt.type == TOK_RPAREN) depth--;
            else if (tt.type == TOK_EOF) break;
        }
        return val;
    }

    if (t.type == TOK_NUMBER) {
        val.type   = CA_CSS_VAL_PX;  /* unitless number = px */
        val.number = t.number;
        return val;
    }

    if (t.type == TOK_DIMENSION) {
        if (strcmp(t.unit, "%") == 0) {
            val.type   = CA_CSS_VAL_PERCENT;
            val.number = t.number;
        } else {
            val.type   = CA_CSS_VAL_PX;
            val.number = t.number;
        }
        return val;
    }

    if (t.type == TOK_IDENT) {
        if (strcasecmp(t.text, "auto") == 0) {
            val.type = CA_CSS_VAL_AUTO;
            return val;
        }
        /* Try named color */
        uint32_t color;
        if (lookup_named_color(t.text, &color)) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = color;
            return val;
        }
        /* Try keyword for this property */
        int kw;
        if (lookup_keyword(t.text, prop, &kw)) {
            val.type    = CA_CSS_VAL_KEYWORD;
            val.keyword = kw;
            return val;
        }
    }

    return val;
}

/* ============================================================
   PARSE DECLARATIONS
   ============================================================ */

static void add_decl(Ca_CssRule *rule, Ca_CssPropId prop, Ca_CssValue val)
{
    if (rule->decl_count >= CA_CSS_MAX_DECLS_PER_RULE) return;
    Ca_CssDecl *d = &rule->decls[rule->decl_count++];
    d->prop      = prop;
    d->value     = val;
    d->important = false;
    d->var_name[0] = '\0';
}

static Ca_CssDecl *last_decl(Ca_CssRule *rule)
{
    if (rule->decl_count == 0) return NULL;
    return &rule->decls[rule->decl_count - 1];
}

/* If the trailing tokens are `! important`, mark recent decls (those added
   since `from_decl`) as important and return true. */
static bool consume_important(Parser *p, Ca_CssRule *rule, int from_decl)
{
    skip_ws(p);
    Token t = parser_peek(p);
    if (t.type != TOK_BANG) return false;
    parser_next(p);
    skip_ws(p);
    Token ident = parser_next(p);
    if (ident.type != TOK_IDENT || strcasecmp(ident.text, "important") != 0)
        return false;
    for (int i = from_decl; i < rule->decl_count; ++i)
        rule->decls[i].important = true;
    return true;
}

static void parse_declarations(Parser *p, Ca_CssRule *rule)
{
    /* Already consumed '{'. Parse until '}'. */
    while (1) {
        skip_ws(p);
        Token t = parser_peek(p);
        if (t.type == TOK_RBRACE || t.type == TOK_EOF) {
            parser_next(p);
            break;
        }

        /* Property name */
        t = parser_next(p);
        if (t.type != TOK_IDENT) {
            /* Skip to next ';' or '}' */
            while (t.type != TOK_SEMICOLON && t.type != TOK_RBRACE && t.type != TOK_EOF)
                t = parser_next(p);
            if (t.type == TOK_RBRACE) break;
            continue;
        }

        char prop_name[64];
        snprintf(prop_name, sizeof(prop_name), "%s", t.text);

        /* Expect ':' */
        skip_ws(p);
        if (!parser_expect(p, TOK_COLON)) {
            /* Skip to ';' or '}' */
            while (1) {
                t = parser_next(p);
                if (t.type == TOK_SEMICOLON || t.type == TOK_RBRACE || t.type == TOK_EOF) break;
            }
            if (t.type == TOK_RBRACE) break;
            continue;
        }

        /* Custom property: --name : value;
           Only hoisted to the stylesheet's vars table when inside :root. */
        if (prop_name[0] == '-' && prop_name[1] == '-' && prop_name[2] != '\0') {
            Ca_CssValue val = parse_value(p, CA_CSS_PROP_NONE);
            if (p->in_root_rule && p->ss &&
                p->ss->var_count < CA_CSS_MAX_VARS) {
                Ca_CssVar *v = &p->ss->vars[p->ss->var_count++];
                snprintf(v->name, sizeof(v->name), "%s", prop_name);
                v->value = val;
            }
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_BANG) { /* allow `!important` */ parser_next(p); skip_ws(p); Token ii = parser_next(p); (void)ii; skip_ws(p); t = parser_peek(p); }
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        Ca_CssPropId prop_id = lookup_property(prop_name);

        /* `border` shorthand: border: <width> [<style>] <color>;
           We retain border-width + border-color (border-style is ignored). */
        if (strcasecmp(prop_name, "border") == 0) {
            int from = rule->decl_count;
            bool got_width = false, got_color = false;
            while (1) {
                skip_ws(p);
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF || pk.type == TOK_BANG)
                    break;
                Ca_CssValue bv = parse_value(p, CA_CSS_PROP_NONE);
                if (bv.type == CA_CSS_VAL_COLOR ||
                    (bv.type == CA_CSS_VAL_VAR && !got_color)) {
                    add_decl(rule, CA_CSS_PROP_BORDER_COLOR, bv);
                    got_color = true;
                } else if ((bv.type == CA_CSS_VAL_PX || bv.type == CA_CSS_VAL_NUMBER) && !got_width) {
                    add_decl(rule, CA_CSS_PROP_BORDER_WIDTH, bv);
                    got_width = true;
                }
                /* Other tokens (style keywords like 'solid') are ignored. */
            }
            consume_important(p, rule, from);
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* Per-side border shorthands: border-{top|right|bottom|left}: <width> <color> */
        {
            Ca_CssPropId side_w = CA_CSS_PROP_NONE, side_c = CA_CSS_PROP_NONE;
            if      (strcasecmp(prop_name, "border-top")    == 0) { side_w = CA_CSS_PROP_BORDER_TOP_WIDTH;    side_c = CA_CSS_PROP_BORDER_TOP_COLOR;    }
            else if (strcasecmp(prop_name, "border-right")  == 0) { side_w = CA_CSS_PROP_BORDER_RIGHT_WIDTH;  side_c = CA_CSS_PROP_BORDER_RIGHT_COLOR;  }
            else if (strcasecmp(prop_name, "border-bottom") == 0) { side_w = CA_CSS_PROP_BORDER_BOTTOM_WIDTH; side_c = CA_CSS_PROP_BORDER_BOTTOM_COLOR; }
            else if (strcasecmp(prop_name, "border-left")   == 0) { side_w = CA_CSS_PROP_BORDER_LEFT_WIDTH;   side_c = CA_CSS_PROP_BORDER_LEFT_COLOR;   }
            if (side_w != CA_CSS_PROP_NONE) {
                int from = rule->decl_count;
                bool got_width = false, got_color = false;
                while (1) {
                    skip_ws(p);
                    Token pk = parser_peek(p);
                    if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF || pk.type == TOK_BANG)
                        break;
                    Ca_CssValue bv = parse_value(p, CA_CSS_PROP_NONE);
                    if ((bv.type == CA_CSS_VAL_COLOR) && !got_color) {
                        add_decl(rule, side_c, bv); got_color = true;
                    } else if ((bv.type == CA_CSS_VAL_PX || bv.type == CA_CSS_VAL_NUMBER) && !got_width) {
                        add_decl(rule, side_w, bv); got_width = true;
                    }
                }
                consume_important(p, rule, from);
                skip_ws(p);
                t = parser_peek(p);
                if (t.type == TOK_SEMICOLON) parser_next(p);
                continue;
            }
        }

        /* Handle shorthand 'padding' and 'margin' */
        if (strcasecmp(prop_name, "padding") == 0 || strcasecmp(prop_name, "margin") == 0) {
            bool is_padding = (strcasecmp(prop_name, "padding") == 0);
            Ca_CssPropId top    = is_padding ? CA_CSS_PROP_PADDING_TOP    : CA_CSS_PROP_MARGIN_TOP;
            Ca_CssPropId right  = is_padding ? CA_CSS_PROP_PADDING_RIGHT  : CA_CSS_PROP_MARGIN_RIGHT;
            Ca_CssPropId bottom = is_padding ? CA_CSS_PROP_PADDING_BOTTOM : CA_CSS_PROP_MARGIN_BOTTOM;
            Ca_CssPropId left   = is_padding ? CA_CSS_PROP_PADDING_LEFT   : CA_CSS_PROP_MARGIN_LEFT;

            Ca_CssValue vals[4] = {0};
            int val_count = 0;

            while (val_count < 4) {
                skip_ws(p);
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF)
                    break;
                vals[val_count++] = parse_value(p, CA_CSS_PROP_NONE);
            }

            /* CSS shorthand: 1→all, 2→v h, 3→t h b, 4→t r b l */
            Ca_CssValue vt, vr, vb, vl;
            if (val_count == 1) {
                vt = vr = vb = vl = vals[0];
            } else if (val_count == 2) {
                vt = vb = vals[0]; vr = vl = vals[1];
            } else if (val_count == 3) {
                vt = vals[0]; vr = vl = vals[1]; vb = vals[2];
            } else {
                vt = vals[0]; vr = vals[1]; vb = vals[2]; vl = vals[3];
            }

            add_decl(rule, top, vt);
            add_decl(rule, right, vr);
            add_decl(rule, bottom, vb);
            add_decl(rule, left, vl);

            /* Consume trailing ';' */
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* Handle shorthand 'overflow' */
        if (prop_id == CA_CSS_PROP_OVERFLOW) {
            Ca_CssValue val = parse_value(p, CA_CSS_PROP_OVERFLOW);
            add_decl(rule, CA_CSS_PROP_OVERFLOW_X, val);
            add_decl(rule, CA_CSS_PROP_OVERFLOW_Y, val);

            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* Handle 'transition' shorthand:
           transition: <property> <duration> [<easing>]
           We store the duration as a number value and encode the target
           property id in the keyword field.  Multiple transitions
           (comma-separated) are not yet supported — only the first is used. */
        if (strcasecmp(prop_name, "transition") == 0) {
            skip_ws(p);
            Token prop_tok = parser_next(p);
            Ca_CssPropId trans_prop = CA_CSS_PROP_NONE;
            if (prop_tok.type == TOK_IDENT) {
                if (strcasecmp(prop_tok.text, "all") == 0) {
                    trans_prop = CA_CSS_PROP_COUNT; /* sentinel: all */
                } else {
                    trans_prop = lookup_property(prop_tok.text);
                }
            }

            /* Duration (in seconds or ms) */
            float duration = 0.0f;
            skip_ws(p);
            Token dur_tok = parser_peek(p);
            if (dur_tok.type == TOK_DIMENSION || dur_tok.type == TOK_NUMBER) {
                parser_next(p);
                duration = dur_tok.number;
                if (dur_tok.type == TOK_DIMENSION &&
                    strcasecmp(dur_tok.unit, "ms") == 0)
                    duration /= 1000.0f;
            }

            /* Skip optional easing / rest of value */
            while (1) {
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF) break;
                parser_next(p);
            }

            if (trans_prop != CA_CSS_PROP_NONE && duration > 0.0f) {
                Ca_CssValue tv = {0};
                tv.type    = CA_CSS_VAL_NUMBER;
                tv.number  = duration;
                tv.keyword = (int)trans_prop;
                add_decl(rule, CA_CSS_PROP_TRANSITION, tv);
            }

            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* Normal property */
        if (prop_id != CA_CSS_PROP_NONE) {
            int from = rule->decl_count;
            Ca_CssValue val = parse_value(p, prop_id);
            add_decl(rule, prop_id, val);
            consume_important(p, rule, from);
        } else {
            /* Unknown property — skip value */
            while (1) {
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF) break;
                parser_next(p);
            }
        }

        /* Consume trailing ';' */
        skip_ws(p);
        t = parser_peek(p);
        if (t.type == TOK_SEMICOLON) parser_next(p);
    }
}

/* ============================================================
   PARSE SELECTORS
   ============================================================ */

static void parse_simple_selector(Parser *p, Ca_CssSimpleSel *sel)
{
    memset(sel, 0, sizeof(*sel));

    Token t = parser_peek(p);

    /* Universal selector */
    if (t.type == TOK_STAR) {
        parser_next(p);
        sel->element[0] = '*';
        sel->element[1] = '\0';
    }
    /* Element name */
    else if (t.type == TOK_IDENT) {
        parser_next(p);
        snprintf(sel->element, sizeof(sel->element), "%s", t.text);
    }

    /* ID selector (#id) — TOK_HASH contains the text after '#' */
    while (parser_peek(p).type == TOK_HASH) {
        t = parser_next(p);
        /* Only take the first ID (CSS spec: multiple IDs are unusual) */
        if (sel->id[0] == '\0')
            snprintf(sel->id, CA_CSS_CLASS_NAME_MAX, "%s", t.text);
    }

    /* Class selectors (.foo.bar) — no whitespace between them */
    while (parser_peek(p).type == TOK_DOT) {
        parser_next(p); /* consume dot */
        t = parser_next(p);
        if (t.type == TOK_IDENT && sel->class_count < CA_CSS_MAX_CLASSES_SEL) {
            snprintf(sel->classes[sel->class_count], CA_CSS_CLASS_NAME_MAX, "%s", t.text);
            sel->class_count++;
        }
    }

    /* Also allow ID after classes: div.foo#bar */
    while (parser_peek(p).type == TOK_HASH) {
        t = parser_next(p);
        if (sel->id[0] == '\0')
            snprintf(sel->id, CA_CSS_CLASS_NAME_MAX, "%s", t.text);
    }

    /* Pseudo-classes (:hover, :focus, :nth-child(...), :not(...) ...) */
    while (parser_peek(p).type == TOK_COLON) {
        parser_next(p);
        Token ptok = parser_next(p);
        if (ptok.type != TOK_IDENT && ptok.type != TOK_FUNCTION) break;

        if (sel->pseudo_count >= CA_CSS_MAX_PSEUDOS_PER_PART) {
            /* Drop excess */
            if (ptok.type == TOK_FUNCTION) {
                int depth = 1;
                while (depth > 0) {
                    Token tt = parser_next(p);
                    if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
                    else if (tt.type == TOK_RPAREN) depth--;
                    else if (tt.type == TOK_EOF) break;
                }
            }
            continue;
        }

        Ca_CssPseudo *ps = &sel->pseudos[sel->pseudo_count];
        memset(ps, 0, sizeof(*ps));

        if (ptok.type == TOK_FUNCTION) {
            if (strcasecmp(ptok.text, "nth-child") == 0 ||
                strcasecmp(ptok.text, "nth-last-child") == 0) {
                ps->kind = (strcasecmp(ptok.text, "nth-child") == 0)
                            ? CA_CSS_PSEUDO_NTH_CHILD
                            : CA_CSS_PSEUDO_NTH_LAST_CHILD;
                /* Parse An+B, odd, even */
                skip_ws(p);
                Token arg = parser_next(p);
                ps->a = 0; ps->b = 0;
                if (arg.type == TOK_IDENT) {
                    if (strcasecmp(arg.text, "odd") == 0)       { ps->a = 2; ps->b = 1; }
                    else if (strcasecmp(arg.text, "even") == 0) { ps->a = 2; ps->b = 0; }
                    else if (strcasecmp(arg.text, "n") == 0)    { ps->a = 1; ps->b = 0; }
                    else {
                        /* Forms like "2n", "2n+1", "-n+3" embedded in ident */
                        const char *s = arg.text;
                        int sign = 1;
                        if (*s == '-') { sign = -1; s++; }
                        else if (*s == '+') { s++; }
                        int num = 0; bool has_num = false;
                        while (*s >= '0' && *s <= '9') { num = num*10 + (*s - '0'); has_num = true; s++; }
                        if (*s == 'n' || *s == 'N') {
                            ps->a = sign * (has_num ? num : 1);
                            s++;
                            /* optional +/-B */
                            while (*s == ' ') s++;
                            int bsign = 1;
                            if (*s == '+') s++;
                            else if (*s == '-') { bsign = -1; s++; }
                            int bnum = 0;
                            while (*s == ' ') s++;
                            while (*s >= '0' && *s <= '9') { bnum = bnum*10 + (*s - '0'); s++; }
                            ps->b = bsign * bnum;
                        } else if (has_num) {
                            ps->a = 0;
                            ps->b = sign * num;
                        }
                    }
                } else if (arg.type == TOK_NUMBER || arg.type == TOK_DIMENSION) {
                    /* e.g. "3" or "2n" lexed as dimension with unit "n" */
                    if (arg.type == TOK_DIMENSION &&
                        (strcasecmp(arg.unit, "n") == 0)) {
                        ps->a = (int)arg.number;
                        /* Peek for trailing +B/-B */
                        skip_ws(p);
                        Token sgn = parser_peek(p);
                        if (sgn.type == TOK_PLUS || sgn.type == TOK_MINUS) {
                            parser_next(p);
                            int bsign = (sgn.type == TOK_MINUS) ? -1 : 1;
                            Token bn = parser_next(p);
                            if (bn.type == TOK_NUMBER || bn.type == TOK_DIMENSION)
                                ps->b = bsign * (int)bn.number;
                        }
                    } else {
                        ps->a = 0;
                        ps->b = (int)arg.number;
                    }
                }
                /* Consume rest until ) */
                int depth = 1;
                while (depth > 0) {
                    Token tt = parser_next(p);
                    if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
                    else if (tt.type == TOK_RPAREN) depth--;
                    else if (tt.type == TOK_EOF) break;
                }
            } else if (strcasecmp(ptok.text, "not") == 0) {
                ps->kind = CA_CSS_PSEUDO_NOT;
                /* Parse a single simple selector inside :not(...) */
                skip_ws(p);
                Token a = parser_peek(p);
                /* element */
                if (a.type == TOK_IDENT) {
                    parser_next(p);
                    snprintf(ps->not_element, sizeof(ps->not_element), "%s", a.text);
                } else if (a.type == TOK_STAR) {
                    parser_next(p);
                    ps->not_element[0] = '*'; ps->not_element[1] = '\0';
                }
                /* id / classes / inner pseudo (single) */
                while (1) {
                    Token b = parser_peek(p);
                    if (b.type == TOK_HASH) {
                        parser_next(p);
                        if (ps->not_id[0] == '\0')
                            snprintf(ps->not_id, sizeof(ps->not_id), "%s", b.text);
                    } else if (b.type == TOK_DOT) {
                        parser_next(p);
                        Token cls = parser_next(p);
                        if (cls.type == TOK_IDENT && ps->not_class[0] == '\0')
                            snprintf(ps->not_class, sizeof(ps->not_class), "%s", cls.text);
                    } else if (b.type == TOK_COLON) {
                        parser_next(p);
                        Token inner = parser_next(p);
                        if (inner.type == TOK_IDENT) {
                            if      (strcasecmp(inner.text, "hover")    == 0) ps->not_pseudo = CA_CSS_PSEUDO_HOVER;
                            else if (strcasecmp(inner.text, "active")   == 0) ps->not_pseudo = CA_CSS_PSEUDO_ACTIVE;
                            else if (strcasecmp(inner.text, "focus")    == 0) ps->not_pseudo = CA_CSS_PSEUDO_FOCUS;
                            else if (strcasecmp(inner.text, "disabled") == 0) ps->not_pseudo = CA_CSS_PSEUDO_DISABLED;
                            else if (strcasecmp(inner.text, "enabled")  == 0) ps->not_pseudo = CA_CSS_PSEUDO_ENABLED;
                            else if (strcasecmp(inner.text, "checked")  == 0) ps->not_pseudo = CA_CSS_PSEUDO_CHECKED;
                        }
                    } else break;
                }
                /* Consume rest until ) */
                int depth = 1;
                while (depth > 0) {
                    Token tt = parser_next(p);
                    if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
                    else if (tt.type == TOK_RPAREN) depth--;
                    else if (tt.type == TOK_EOF) break;
                }
            } else {
                /* Unknown functional pseudo \u2014 skip */
                int depth = 1;
                while (depth > 0) {
                    Token tt = parser_next(p);
                    if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
                    else if (tt.type == TOK_RPAREN) depth--;
                    else if (tt.type == TOK_EOF) break;
                }
                continue; /* don't store */
            }
            sel->pseudo_count++;
        } else {
            /* Simple identifier pseudo */
            if      (strcasecmp(ptok.text, "hover")        == 0) ps->kind = CA_CSS_PSEUDO_HOVER;
            else if (strcasecmp(ptok.text, "active")       == 0) ps->kind = CA_CSS_PSEUDO_ACTIVE;
            else if (strcasecmp(ptok.text, "focus")        == 0) ps->kind = CA_CSS_PSEUDO_FOCUS;
            else if (strcasecmp(ptok.text, "focus-within") == 0) ps->kind = CA_CSS_PSEUDO_FOCUS_WITHIN;
            else if (strcasecmp(ptok.text, "disabled")     == 0) ps->kind = CA_CSS_PSEUDO_DISABLED;
            else if (strcasecmp(ptok.text, "enabled")      == 0) ps->kind = CA_CSS_PSEUDO_ENABLED;
            else if (strcasecmp(ptok.text, "checked")      == 0) ps->kind = CA_CSS_PSEUDO_CHECKED;
            else if (strcasecmp(ptok.text, "first-child")  == 0) ps->kind = CA_CSS_PSEUDO_FIRST_CHILD;
            else if (strcasecmp(ptok.text, "last-child")   == 0) ps->kind = CA_CSS_PSEUDO_LAST_CHILD;
            else if (strcasecmp(ptok.text, "only-child")   == 0) ps->kind = CA_CSS_PSEUDO_ONLY_CHILD;
            else if (strcasecmp(ptok.text, "first-of-type")== 0) ps->kind = CA_CSS_PSEUDO_FIRST_OF_TYPE;
            else if (strcasecmp(ptok.text, "last-of-type") == 0) ps->kind = CA_CSS_PSEUDO_LAST_OF_TYPE;
            else if (strcasecmp(ptok.text, "root")         == 0) ps->kind = CA_CSS_PSEUDO_ROOT;
            else if (strcasecmp(ptok.text, "empty")        == 0) ps->kind = CA_CSS_PSEUDO_EMPTY;
            else continue; /* unknown \u2014 don't store */
            sel->pseudo_count++;
        }
    }
}

static void parse_selector(Parser *p, Ca_CssSelector *sel)
{
    memset(sel, 0, sizeof(*sel));

    parse_simple_selector(p, &sel->parts[0]);
    sel->part_count = 1;

    while (sel->part_count < CA_CSS_MAX_CHAIN) {
        Token t = parser_peek(p);

        /* Check for combinator */
        if (t.type == TOK_GT) {
            parser_next(p);
            skip_ws(p);
            Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
            parse_simple_selector(p, part);
            part->combinator = CA_CSS_COMB_CHILD;
            sel->part_count++;
        } else if (t.type == TOK_PLUS) {
            parser_next(p);
            skip_ws(p);
            Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
            parse_simple_selector(p, part);
            part->combinator = CA_CSS_COMB_NEXT_SIBLING;
            sel->part_count++;
        } else if (t.type == TOK_TILDE) {
            parser_next(p);
            skip_ws(p);
            Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
            parse_simple_selector(p, part);
            part->combinator = CA_CSS_COMB_SUBSEQ_SIBLING;
            sel->part_count++;
        } else if (t.type == TOK_WS) {
            parser_next(p);
            /* Check if next is a combinator or selector start */
            Token nxt = parser_peek(p);
            if (nxt.type == TOK_GT) {
                /* > with spaces around it */
                parser_next(p);
                skip_ws(p);
                Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
                parse_simple_selector(p, part);
                part->combinator = CA_CSS_COMB_CHILD;
                sel->part_count++;
            } else if (nxt.type == TOK_PLUS) {
                parser_next(p); skip_ws(p);
                Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
                parse_simple_selector(p, part);
                part->combinator = CA_CSS_COMB_NEXT_SIBLING;
                sel->part_count++;
            } else if (nxt.type == TOK_TILDE) {
                parser_next(p); skip_ws(p);
                Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
                parse_simple_selector(p, part);
                part->combinator = CA_CSS_COMB_SUBSEQ_SIBLING;
                sel->part_count++;
            } else if (nxt.type == TOK_IDENT || nxt.type == TOK_DOT ||
                       nxt.type == TOK_STAR || nxt.type == TOK_HASH ||
                       nxt.type == TOK_COLON) {
                /* Descendant combinator */
                Ca_CssSimpleSel *part = &sel->parts[sel->part_count];
                parse_simple_selector(p, part);
                part->combinator = CA_CSS_COMB_DESCENDANT;
                sel->part_count++;
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

static void parse_selector_list(Parser *p, Ca_CssRule *rule)
{
    rule->selector_count = 0;

    if (rule->selector_count < CA_CSS_MAX_SELECTORS_PER_RULE) {
        parse_selector(p, &rule->selectors[rule->selector_count]);
        rule->selector_count++;
    }

    while (1) {
        skip_ws(p);
        Token t = parser_peek(p);
        if (t.type == TOK_COMMA) {
            parser_next(p);
            skip_ws(p);
            if (rule->selector_count < CA_CSS_MAX_SELECTORS_PER_RULE) {
                parse_selector(p, &rule->selectors[rule->selector_count]);
                rule->selector_count++;
            }
        } else {
            break;
        }
    }
}

/* ============================================================
   PARSE STYLESHEET
   ============================================================ */

Ca_Stylesheet *ca_css_parse(const char *css_text)
{
    if (!css_text) return NULL;

    Ca_Stylesheet *ss = (Ca_Stylesheet *)CA_CALLOC(1, sizeof(Ca_Stylesheet));
    if (!ss) return NULL;

    Parser p;
    parser_init(&p, css_text);
    p.ss = ss;

    int order = 0;

    while (1) {
        skip_ws(&p);
        Token t = parser_peek(&p);
        if (t.type == TOK_EOF) break;

        if (ss->rule_count >= CA_CSS_MAX_RULES) {
            fprintf(stderr, "[css] max rules exceeded (%d)\n", CA_CSS_MAX_RULES);
            break;
        }

        Ca_CssRule *rule = &ss->rules[ss->rule_count];
        memset(rule, 0, sizeof(*rule));

        /* Parse selector list */
        parse_selector_list(&p, rule);

        /* Detect :root rule (any selector whose only part is :root). */
        p.in_root_rule = false;
        for (int si = 0; si < rule->selector_count; ++si) {
            Ca_CssSelector *s = &rule->selectors[si];
            if (s->part_count == 1 &&
                s->parts[0].element[0] == '\0' &&
                s->parts[0].id[0] == '\0' &&
                s->parts[0].class_count == 0 &&
                s->parts[0].pseudo_count == 1 &&
                s->parts[0].pseudos[0].kind == CA_CSS_PSEUDO_ROOT) {
                p.in_root_rule = true;
                break;
            }
        }

        /* Expect '{' */
        skip_ws(&p);
        t = parser_peek(&p);
        if (t.type != TOK_LBRACE) {
            /* Error — skip to next '{' or EOF */
            while (t.type != TOK_LBRACE && t.type != TOK_EOF) {
                parser_next(&p);
                t = parser_peek(&p);
            }
            if (t.type == TOK_EOF) break;
        }
        parser_next(&p); /* consume '{' */

        /* Parse declarations */
        parse_declarations(&p, rule);

        rule->source_order = order++;
        ss->rule_count++;
    }

    return ss;
}

void ca_css_destroy(Ca_Stylesheet *ss)
{
    CA_FREE(ss);
}
