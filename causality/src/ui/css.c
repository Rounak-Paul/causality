/* css.c — CSS tokenizer + recursive descent parser */
#include "css.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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
    TOK_DOT,
    TOK_STAR,
    TOK_GT,
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
    if (had_ws && (c == '.' || c == '#' || c == '*')) {
        tok.type = TOK_WS;
        tok.text[0] = ' ';
        tok.text[1] = '\0';
        return tok;
    }

    /* Hash (#color) */
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
        case '.': tok.type = TOK_DOT;       break;
        case '*': tok.type = TOK_STAR;       break;
        case '>': tok.type = TOK_GT;        break;
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
} Parser;

static void parser_init(Parser *p, const char *src)
{
    lexer_init(&p->lex, src);
    p->buf_count = 0;
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
        { "overflow",         CA_CSS_PROP_OVERFLOW },
        { "overflow-x",       CA_CSS_PROP_OVERFLOW_X },
        { "overflow-y",       CA_CSS_PROP_OVERFLOW_Y },
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
    d->prop  = prop;
    d->value = val;
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

        Ca_CssPropId prop_id = lookup_property(prop_name);

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

        /* Normal property */
        if (prop_id != CA_CSS_PROP_NONE) {
            Ca_CssValue val = parse_value(p, prop_id);
            add_decl(rule, prop_id, val);
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

    /* Class selectors (.foo.bar) — no whitespace between them */
    while (parser_peek(p).type == TOK_DOT) {
        parser_next(p); /* consume dot */
        t = parser_next(p);
        if (t.type == TOK_IDENT && sel->class_count < CA_CSS_MAX_CLASSES_SEL) {
            snprintf(sel->classes[sel->class_count], CA_CSS_CLASS_NAME_MAX, "%s", t.text);
            sel->class_count++;
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
            } else if (nxt.type == TOK_IDENT || nxt.type == TOK_DOT ||
                       nxt.type == TOK_STAR) {
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

    Ca_Stylesheet *ss = (Ca_Stylesheet *)calloc(1, sizeof(Ca_Stylesheet));
    if (!ss) return NULL;

    Parser p;
    parser_init(&p, css_text);

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
    free(ss);
}
