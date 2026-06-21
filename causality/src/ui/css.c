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

/* OKLCH -> linear sRGB -> gamma sRGB conversion. */
static uint32_t parse_oklch_func(Parser *p)
{
    float L = 0.0f, C = 0.0f, H = 0.0f, alpha = 1.0f;
    float vals[4] = {0, 0, 0, 1.0f};
    int idx = 0;
    bool after_slash = false;

    while (idx < 4) {
        skip_ws(p);
        Token t = parser_peek(p);
        if (t.type == TOK_RPAREN || t.type == TOK_EOF) { parser_next(p); break; }
        if (t.type == TOK_IDENT && strcmp(t.text, "/") == 0) {
            parser_next(p); after_slash = true; continue;
        }
        /* handle slash as operator */
        parser_next(p);
        if (t.type == TOK_NUMBER || t.type == TOK_DIMENSION) {
            if (after_slash) { alpha = t.number; if (t.type == TOK_DIMENSION && strcmp(t.unit, "%") == 0) alpha /= 100.0f; }
            else { vals[idx++] = t.number; if (t.type == TOK_DIMENSION && strcmp(t.unit, "%") == 0 && idx == 1) vals[idx-1] /= 100.0f; }
        } else if (t.type == TOK_SEMICOLON || t.type == TOK_COMMA) { continue; }
    }
    L = vals[0]; C = vals[1]; H = vals[2];
    if (alpha < 0) alpha = 0; if (alpha > 1) alpha = 1;

    float hr = H * 3.14159265358979323846f / 180.0f;
    float a_ok = C * cosf(hr);
    float b_ok = C * sinf(hr);

    /* OKLab -> linear sRGB via D65 matrix */
    float ll = L + 0.3963377774f * a_ok + 0.2158037573f * b_ok;
    float mm = L - 0.1055613458f * a_ok - 0.0638541728f * b_ok;
    float ss = L - 0.0894841775f * a_ok - 1.2914855480f * b_ok;
    ll = ll*ll*ll; mm = mm*mm*mm; ss = ss*ss*ss;

    float lr =  4.0767416621f*ll - 3.3077115913f*mm + 0.2309699292f*ss;
    float lg = -1.2684380046f*ll + 2.6097574011f*mm - 0.3413193965f*ss;
    float lb = -0.0041960863f*ll - 0.7034186147f*mm + 1.7076147010f*ss;

    /* Linear -> gamma sRGB */
    #define OKLCH_GAMMA(x) ((x) <= 0.0031308f ? (x)*12.92f : 1.055f*powf((x),1.0f/2.4f)-0.055f)
    float rf = OKLCH_GAMMA(lr); float gf = OKLCH_GAMMA(lg); float bf = OKLCH_GAMMA(lb);
    #undef OKLCH_GAMMA
    if (rf < 0) rf = 0; if (rf > 1) rf = 1;
    if (gf < 0) gf = 0; if (gf > 1) gf = 1;
    if (bf < 0) bf = 0; if (bf > 1) bf = 1;

    uint32_t r = (uint32_t)(rf*255.0f+0.5f);
    uint32_t g = (uint32_t)(gf*255.0f+0.5f);
    uint32_t b = (uint32_t)(bf*255.0f+0.5f);
    uint32_t ai= (uint32_t)(alpha*255.0f+0.5f);
    if (r>255) r=255; if (g>255) g=255; if (b>255) b=255; if (ai>255) ai=255;
    return (r<<24)|(g<<16)|(b<<8)|ai;
}

/* Blends two CSS colors with given percentage weight.
   color-mix(in sRGB, color1 p1%, color2 p2%) */
static uint32_t parse_color_mix_func(Parser *p)
{
    /* Skip "in <colorspace>," */
    int depth = 0;
    bool past_comma1 = false;
    uint32_t c1 = 0xFF000000, c2 = 0xFFFFFFFF;
    float p1 = 50.0f, p2 = 50.0f;
    int color_idx = 0;

    (void)depth;
    /* consume "in sRGB ," */
    skip_ws(p);
    Token colorspace = parser_next(p); /* 'in' */
    if (colorspace.type == TOK_IDENT && strcasecmp(colorspace.text, "in") == 0) {
        skip_ws(p);
        parser_next(p); /* e.g. 'sRGB' */
        skip_ws(p);
        Token comma = parser_next(p); /* comma */
        (void)comma;
    }
    past_comma1 = true;
    (void)past_comma1;

    /* Parse up to two color entries */
    while (color_idx < 2) {
        skip_ws(p);
        Token t = parser_peek(p);
        if (t.type == TOK_RPAREN || t.type == TOK_EOF) { parser_next(p); break; }

        uint32_t color = 0;
        bool got_color = false;

        if (t.type == TOK_HASH) {
            parser_next(p);
            color = parse_hex_color(t.text); got_color = true;
        } else if (t.type == TOK_FUNCTION) {
            parser_next(p);
            if      (strcasecmp(t.text, "rgb")  == 0) { color = parse_rgb_func(p, false); got_color = true; }
            else if (strcasecmp(t.text, "rgba") == 0) { color = parse_rgb_func(p, true);  got_color = true; }
            else if (strcasecmp(t.text, "hsl")  == 0) { color = parse_hsl_func(p, false); got_color = true; }
            else if (strcasecmp(t.text, "hsla") == 0) { color = parse_hsl_func(p, true);  got_color = true; }
            else if (strcasecmp(t.text, "oklch")== 0) { color = parse_oklch_func(p);      got_color = true; }
            else {
                int d = 1;
                while (d > 0) { Token tt = parser_next(p); if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) d++; else if (tt.type == TOK_RPAREN) d--; else if (tt.type == TOK_EOF) break; }
            }
        } else if (t.type == TOK_IDENT) {
            parser_next(p);
            if (!lookup_named_color(t.text, &color)) color = 0xFF000000;
            got_color = true;
        } else {
            parser_next(p);
            continue;
        }

        /* optional percentage following the color */
        skip_ws(p);
        Token pk = parser_peek(p);
        float pct = (color_idx == 0) ? 50.0f : 50.0f;
        if (pk.type == TOK_DIMENSION && strcmp(pk.unit, "%") == 0) {
            parser_next(p);
            pct = pk.number;
        }

        if (got_color) {
            if (color_idx == 0) { c1 = color; p1 = pct; }
            else                { c2 = color; p2 = pct; }
            color_idx++;
        }

        skip_ws(p);
        pk = parser_peek(p);
        if (pk.type == TOK_COMMA) { parser_next(p); continue; }
        if (pk.type == TOK_RPAREN) { parser_next(p); break; }
    }

    /* consume ) if still open */
    Token t = parser_peek(p);
    if (t.type == TOK_RPAREN) parser_next(p);

    float total = p1 + p2;
    if (total <= 0) total = 100.0f;
    float w1 = p1 / total, w2 = p2 / total;

    float r = ((c1 >> 24) & 0xFF) * w1 + ((c2 >> 24) & 0xFF) * w2;
    float g = ((c1 >> 16) & 0xFF) * w1 + ((c2 >> 16) & 0xFF) * w2;
    float b = ((c1 >>  8) & 0xFF) * w1 + ((c2 >>  8) & 0xFF) * w2;
    float a = ( c1        & 0xFF) * w1 + ( c2        & 0xFF) * w2;

    #define CM_CLAMP(x) ((x) > 255 ? 255 : ((x) < 0 ? 0 : (uint32_t)((x)+0.5f)))
    return (CM_CLAMP(r) << 24) | (CM_CLAMP(g) << 16) | (CM_CLAMP(b) << 8) | CM_CLAMP(a);
    #undef CM_CLAMP
}

/* calc() — left-associative flat expression, returns pixel value. */
static float parse_calc_expr(Parser *p)
{
    float result = 0.0f;
    char op = '+';
    int depth = 1; /* we're already inside the first '(' */

    while (1) {
        skip_ws(p);
        Token t = parser_next(p);

        if (t.type == TOK_RPAREN || t.type == TOK_EOF) {
            depth--;
            if (depth <= 0) break;
        }
        if (t.type == TOK_LPAREN || t.type == TOK_FUNCTION) {
            depth++;
            float inner = parse_calc_expr(p);
            float term = inner;
            if      (op == '+') result += term;
            else if (op == '-') result -= term;
            else if (op == '*') result *= term;
            else if (op == '/' && inner != 0.0f) result /= inner;
            continue;
        }
        if (t.type == TOK_NUMBER || t.type == TOK_DIMENSION) {
            float v = t.number;
            if (t.type == TOK_DIMENSION) {
                if      (strcmp(t.unit, "em")  == 0) v *= 16.0f;
                else if (strcmp(t.unit, "rem") == 0) v *= 16.0f;
                else if (strcmp(t.unit, "vw")  == 0) v *= 19.2f;
                else if (strcmp(t.unit, "vh")  == 0) v *= 10.8f;
                else if (strcmp(t.unit, "pt")  == 0) v *= (96.0f/72.0f);
                else if (strcmp(t.unit, "cm")  == 0) v *= 37.795f;
                else if (strcmp(t.unit, "mm")  == 0) v *= 3.7795f;
                else if (strcmp(t.unit, "in")  == 0) v *= 96.0f;
            }
            if      (op == '+') result += v;
            else if (op == '-') result -= v;
            else if (op == '*') result *= v;
            else if (op == '/' && v != 0.0f) result /= v;
            else if (op == 0) result = v;
            op = 0;
            continue;
        }
        if (t.type == TOK_PLUS)  { op = '+'; continue; }
        if (t.type == TOK_MINUS) { op = '-'; continue; }
        if (t.type == TOK_STAR)  { op = '*'; continue; }
        /* '/' would need a special token — skip unknown */
    }
    return result;
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
        { "width",                     CA_CSS_PROP_WIDTH },
        { "height",                    CA_CSS_PROP_HEIGHT },
        { "min-width",                 CA_CSS_PROP_MIN_WIDTH },
        { "max-width",                 CA_CSS_PROP_MAX_WIDTH },
        { "min-height",                CA_CSS_PROP_MIN_HEIGHT },
        { "max-height",                CA_CSS_PROP_MAX_HEIGHT },
        { "padding-top",               CA_CSS_PROP_PADDING_TOP },
        { "padding-right",             CA_CSS_PROP_PADDING_RIGHT },
        { "padding-bottom",            CA_CSS_PROP_PADDING_BOTTOM },
        { "padding-left",              CA_CSS_PROP_PADDING_LEFT },
        { "margin-top",                CA_CSS_PROP_MARGIN_TOP },
        { "margin-right",              CA_CSS_PROP_MARGIN_RIGHT },
        { "margin-bottom",             CA_CSS_PROP_MARGIN_BOTTOM },
        { "margin-left",               CA_CSS_PROP_MARGIN_LEFT },
        { "gap",                       CA_CSS_PROP_GAP },
        { "row-gap",                   CA_CSS_PROP_ROW_GAP },
        { "column-gap",                CA_CSS_PROP_COLUMN_GAP },
        { "display",                   CA_CSS_PROP_DISPLAY },
        { "flex-direction",            CA_CSS_PROP_FLEX_DIRECTION },
        { "flex-wrap",                 CA_CSS_PROP_FLEX_WRAP },
        { "align-items",               CA_CSS_PROP_ALIGN_ITEMS },
        { "align-self",                CA_CSS_PROP_ALIGN_SELF },
        { "align-content",             CA_CSS_PROP_ALIGN_CONTENT },
        { "justify-content",           CA_CSS_PROP_JUSTIFY_CONTENT },
        { "justify-self",              CA_CSS_PROP_JUSTIFY_SELF },
        { "flex-grow",                 CA_CSS_PROP_FLEX_GROW },
        { "flex-shrink",               CA_CSS_PROP_FLEX_SHRINK },
        { "flex-basis",                CA_CSS_PROP_FLEX_BASIS },
        { "order",                     CA_CSS_PROP_ORDER },
        { "background-color",          CA_CSS_PROP_BACKGROUND_COLOR },
        { "background",                CA_CSS_PROP_BACKGROUND },
        { "color",                     CA_CSS_PROP_COLOR },
        { "border-radius",             CA_CSS_PROP_BORDER_RADIUS },
        { "border-top-left-radius",    CA_CSS_PROP_BORDER_TOP_LEFT_RADIUS },
        { "border-top-right-radius",   CA_CSS_PROP_BORDER_TOP_RIGHT_RADIUS },
        { "border-bottom-right-radius",CA_CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS },
        { "border-bottom-left-radius", CA_CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS },
        { "opacity",                   CA_CSS_PROP_OPACITY },
        { "visibility",                CA_CSS_PROP_VISIBILITY },
        { "font-size",                 CA_CSS_PROP_FONT_SIZE },
        { "font-weight",               CA_CSS_PROP_FONT_WEIGHT },
        { "font-style",                CA_CSS_PROP_FONT_STYLE },
        { "line-height",               CA_CSS_PROP_LINE_HEIGHT },
        { "letter-spacing",            CA_CSS_PROP_LETTER_SPACING },
        { "word-spacing",              CA_CSS_PROP_WORD_SPACING },
        { "text-align",                CA_CSS_PROP_TEXT_ALIGN },
        { "text-decoration",           CA_CSS_PROP_TEXT_DECORATION },
        { "text-transform",            CA_CSS_PROP_TEXT_TRANSFORM },
        { "white-space",               CA_CSS_PROP_WHITE_SPACE },
        { "overflow",                  CA_CSS_PROP_OVERFLOW },
        { "overflow-x",                CA_CSS_PROP_OVERFLOW_X },
        { "overflow-y",                CA_CSS_PROP_OVERFLOW_Y },
        { "border-width",              CA_CSS_PROP_BORDER_WIDTH },
        { "border-color",              CA_CSS_PROP_BORDER_COLOR },
        { "border-style",              CA_CSS_PROP_BORDER_STYLE },
        { "border-top-width",          CA_CSS_PROP_BORDER_TOP_WIDTH },
        { "border-top-color",          CA_CSS_PROP_BORDER_TOP_COLOR },
        { "border-top-style",          CA_CSS_PROP_BORDER_TOP_STYLE },
        { "border-right-width",        CA_CSS_PROP_BORDER_RIGHT_WIDTH },
        { "border-right-color",        CA_CSS_PROP_BORDER_RIGHT_COLOR },
        { "border-right-style",        CA_CSS_PROP_BORDER_RIGHT_STYLE },
        { "border-bottom-width",       CA_CSS_PROP_BORDER_BOTTOM_WIDTH },
        { "border-bottom-color",       CA_CSS_PROP_BORDER_BOTTOM_COLOR },
        { "border-bottom-style",       CA_CSS_PROP_BORDER_BOTTOM_STYLE },
        { "border-left-width",         CA_CSS_PROP_BORDER_LEFT_WIDTH },
        { "border-left-color",         CA_CSS_PROP_BORDER_LEFT_COLOR },
        { "border-left-style",         CA_CSS_PROP_BORDER_LEFT_STYLE },
        { "outline-width",             CA_CSS_PROP_OUTLINE_WIDTH },
        { "outline-color",             CA_CSS_PROP_OUTLINE_COLOR },
        { "outline-style",             CA_CSS_PROP_OUTLINE_STYLE },
        { "outline-offset",            CA_CSS_PROP_OUTLINE_OFFSET },
        { "box-shadow",                CA_CSS_PROP_BOX_SHADOW },
        { "shadow-offset-x",           CA_CSS_PROP_SHADOW_OFFSET_X },
        { "shadow-offset-y",           CA_CSS_PROP_SHADOW_OFFSET_Y },
        { "shadow-blur",               CA_CSS_PROP_SHADOW_BLUR },
        { "shadow-color",              CA_CSS_PROP_SHADOW_COLOR },
        { "z-index",                   CA_CSS_PROP_Z_INDEX },
        { "text-wrap",                 CA_CSS_PROP_TEXT_WRAP },
        { "aspect-ratio",              CA_CSS_PROP_ASPECT_RATIO },
        { "box-sizing",                CA_CSS_PROP_BOX_SIZING },
        { "cursor",                    CA_CSS_PROP_CURSOR },
        { "pointer-events",            CA_CSS_PROP_POINTER_EVENTS },
        { "user-select",               CA_CSS_PROP_USER_SELECT },
        { "scroll-behavior",           CA_CSS_PROP_SCROLL_BEHAVIOR },
        { "scrollbar-width",            CA_CSS_PROP_SCROLLBAR_WIDTH },
        { "scrollbar-track-color",      CA_CSS_PROP_SCROLLBAR_TRACK_COLOR },
        { "scrollbar-thumb-color",      CA_CSS_PROP_SCROLLBAR_THUMB_COLOR },
        { "scrollbar-thumb-active-color", CA_CSS_PROP_SCROLLBAR_THUMB_ACTIVE_COLOR },
        { "scrollbar-radius",           CA_CSS_PROP_SCROLLBAR_RADIUS },
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
        { "normal",  CA_CSS_FONT_WEIGHT_NORMAL },
        { "bold",    CA_CSS_FONT_WEIGHT_BOLD },
        { "lighter", CA_CSS_FONT_WEIGHT_LIGHTER },
        { "bolder",  CA_CSS_FONT_WEIGHT_BOLDER },
    };
    /* font-style */
    static Ca_KwEntry fontstyle_kw[] = {
        { "normal",  CA_CSS_FONT_STYLE_NORMAL },
        { "italic",  CA_CSS_FONT_STYLE_ITALIC },
        { "oblique", CA_CSS_FONT_STYLE_OBLIQUE },
    };
    /* text-align */
    static Ca_KwEntry textalign_kw[] = {
        { "left",    CA_CSS_TEXT_ALIGN_LEFT },
        { "center",  CA_CSS_TEXT_ALIGN_CENTER },
        { "right",   CA_CSS_TEXT_ALIGN_RIGHT },
        { "start",   CA_CSS_TEXT_ALIGN_START },
        { "end",     CA_CSS_TEXT_ALIGN_END },
        { "justify", CA_CSS_TEXT_ALIGN_JUSTIFY },
    };
    /* text-decoration */
    static Ca_KwEntry textdeco_kw[] = {
        { "none",         CA_CSS_TEXT_DECORATION_NONE },
        { "underline",    CA_CSS_TEXT_DECORATION_UNDERLINE },
        { "line-through", CA_CSS_TEXT_DECORATION_LINE_THROUGH },
        { "overline",     CA_CSS_TEXT_DECORATION_OVERLINE },
    };
    /* text-transform */
    static Ca_KwEntry texttrans_kw[] = {
        { "none",       CA_CSS_TEXT_TRANSFORM_NONE },
        { "uppercase",  CA_CSS_TEXT_TRANSFORM_UPPERCASE },
        { "lowercase",  CA_CSS_TEXT_TRANSFORM_LOWERCASE },
        { "capitalize", CA_CSS_TEXT_TRANSFORM_CAPITALIZE },
    };
    /* white-space */
    static Ca_KwEntry whitespace_kw[] = {
        { "normal",   CA_CSS_WHITE_SPACE_NORMAL },
        { "nowrap",   CA_CSS_WHITE_SPACE_NOWRAP },
        { "pre",      CA_CSS_WHITE_SPACE_PRE },
        { "pre-line", CA_CSS_WHITE_SPACE_PRE_LINE },
        { "pre-wrap", CA_CSS_WHITE_SPACE_PRE_WRAP },
    };
    /* visibility */
    static Ca_KwEntry visibility_kw[] = {
        { "visible",  CA_CSS_VISIBILITY_VISIBLE },
        { "hidden",   CA_CSS_VISIBILITY_HIDDEN },
        { "collapse", CA_CSS_VISIBILITY_COLLAPSE },
    };
    /* border-style */
    static Ca_KwEntry borderstyle_kw[] = {
        { "none",   CA_CSS_BORDER_NONE },
        { "solid",  CA_CSS_BORDER_SOLID },
        { "dashed", CA_CSS_BORDER_DASHED },
        { "dotted", CA_CSS_BORDER_DOTTED },
        { "double", CA_CSS_BORDER_DOUBLE },
        { "groove", CA_CSS_BORDER_GROOVE },
        { "ridge",  CA_CSS_BORDER_RIDGE },
        { "inset",  CA_CSS_BORDER_INSET },
        { "outset", CA_CSS_BORDER_OUTSET },
        { "hidden", CA_CSS_BORDER_HIDDEN },
    };
    /* box-sizing */
    static Ca_KwEntry boxsizing_kw[] = {
        { "content-box", CA_CSS_BOX_SIZING_CONTENT_BOX },
        { "border-box",  CA_CSS_BOX_SIZING_BORDER_BOX },
    };
    /* cursor */
    static Ca_KwEntry cursor_kw[] = {
        { "auto",       CA_CSS_CURSOR_AUTO },
        { "default",    CA_CSS_CURSOR_DEFAULT },
        { "pointer",    CA_CSS_CURSOR_POINTER },
        { "crosshair",  CA_CSS_CURSOR_CROSSHAIR },
        { "move",       CA_CSS_CURSOR_MOVE },
        { "text",       CA_CSS_CURSOR_TEXT },
        { "wait",       CA_CSS_CURSOR_WAIT },
        { "help",       CA_CSS_CURSOR_HELP },
        { "not-allowed",CA_CSS_CURSOR_NOT_ALLOWED },
        { "grab",       CA_CSS_CURSOR_GRAB },
        { "grabbing",   CA_CSS_CURSOR_GRABBING },
        { "ew-resize",  CA_CSS_CURSOR_EW_RESIZE },
        { "ns-resize",  CA_CSS_CURSOR_NS_RESIZE },
        { "nwse-resize",CA_CSS_CURSOR_NWSE_RESIZE },
        { "nesw-resize",CA_CSS_CURSOR_NESW_RESIZE },
        { "col-resize", CA_CSS_CURSOR_COL_RESIZE },
        { "row-resize", CA_CSS_CURSOR_ROW_RESIZE },
        { "none",       CA_CSS_CURSOR_NONE },
    };
    /* pointer-events */
    static Ca_KwEntry ptrevents_kw[] = {
        { "auto", CA_CSS_POINTER_EVENTS_AUTO },
        { "none", CA_CSS_POINTER_EVENTS_NONE },
    };
    /* user-select */
    static Ca_KwEntry usersel_kw[] = {
        { "auto", CA_CSS_USER_SELECT_AUTO },
        { "none", CA_CSS_USER_SELECT_NONE },
        { "text", CA_CSS_USER_SELECT_TEXT },
        { "all",  CA_CSS_USER_SELECT_ALL },
    };
    /* scroll-behavior */
    static Ca_KwEntry scrollbeh_kw[] = {
        { "auto",   CA_CSS_SCROLL_AUTO },
        { "smooth", CA_CSS_SCROLL_SMOOTH },
    };

    switch (prop) {
        case CA_CSS_PROP_DISPLAY:
            tbl = display_kw; count = 3; break;
        case CA_CSS_PROP_FLEX_DIRECTION:
            tbl = flexdir_kw; count = 4; break;
        case CA_CSS_PROP_FLEX_WRAP:
            tbl = wrap_kw; count = 2; break;
        case CA_CSS_PROP_ALIGN_ITEMS:
        case CA_CSS_PROP_ALIGN_SELF:
        case CA_CSS_PROP_ALIGN_CONTENT:
        case CA_CSS_PROP_JUSTIFY_SELF:
            tbl = align_kw; count = (int)(sizeof(align_kw)/sizeof(align_kw[0])); break;
        case CA_CSS_PROP_JUSTIFY_CONTENT:
            tbl = justify_kw; count = (int)(sizeof(justify_kw)/sizeof(justify_kw[0])); break;
        case CA_CSS_PROP_OVERFLOW:
        case CA_CSS_PROP_OVERFLOW_X:
        case CA_CSS_PROP_OVERFLOW_Y:
            tbl = overflow_kw; count = 4; break;
        case CA_CSS_PROP_TEXT_ALIGN:
            tbl = textalign_kw; count = (int)(sizeof(textalign_kw)/sizeof(textalign_kw[0])); break;
        case CA_CSS_PROP_FONT_WEIGHT:
            tbl = fontweight_kw; count = (int)(sizeof(fontweight_kw)/sizeof(fontweight_kw[0])); break;
        case CA_CSS_PROP_FONT_STYLE:
            tbl = fontstyle_kw; count = 3; break;
        case CA_CSS_PROP_TEXT_DECORATION:
            tbl = textdeco_kw; count = 4; break;
        case CA_CSS_PROP_TEXT_TRANSFORM:
            tbl = texttrans_kw; count = 4; break;
        case CA_CSS_PROP_WHITE_SPACE:
            tbl = whitespace_kw; count = 5; break;
        case CA_CSS_PROP_VISIBILITY:
            tbl = visibility_kw; count = 3; break;
        case CA_CSS_PROP_BORDER_STYLE:
        case CA_CSS_PROP_BORDER_TOP_STYLE:
        case CA_CSS_PROP_BORDER_RIGHT_STYLE:
        case CA_CSS_PROP_BORDER_BOTTOM_STYLE:
        case CA_CSS_PROP_BORDER_LEFT_STYLE:
        case CA_CSS_PROP_OUTLINE_STYLE:
            tbl = borderstyle_kw; count = (int)(sizeof(borderstyle_kw)/sizeof(borderstyle_kw[0])); break;
        case CA_CSS_PROP_BOX_SIZING:
            tbl = boxsizing_kw; count = 2; break;
        case CA_CSS_PROP_CURSOR:
            tbl = cursor_kw; count = (int)(sizeof(cursor_kw)/sizeof(cursor_kw[0])); break;
        case CA_CSS_PROP_POINTER_EVENTS:
            tbl = ptrevents_kw; count = 2; break;
        case CA_CSS_PROP_USER_SELECT:
            tbl = usersel_kw; count = 4; break;
        case CA_CSS_PROP_SCROLL_BEHAVIOR:
            tbl = scrollbeh_kw; count = 2; break;
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
        if (strcasecmp(t.text, "oklch") == 0) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = parse_oklch_func(p);
            return val;
        }
        if (strcasecmp(t.text, "color-mix") == 0) {
            val.type  = CA_CSS_VAL_COLOR;
            val.color = parse_color_mix_func(p);
            return val;
        }
        if (strcasecmp(t.text, "calc") == 0 ||
            strcasecmp(t.text, "min")  == 0 ||
            strcasecmp(t.text, "max")  == 0 ||
            strcasecmp(t.text, "clamp") == 0) {
            val.type   = CA_CSS_VAL_PX;
            val.number = parse_calc_expr(p);
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
        } else if (strcmp(t.unit, "em") == 0) {
            val.type   = CA_CSS_VAL_EM;
            val.number = t.number;
        } else if (strcmp(t.unit, "rem") == 0) {
            val.type   = CA_CSS_VAL_REM;
            val.number = t.number;
        } else if (strcmp(t.unit, "vw") == 0) {
            val.type   = CA_CSS_VAL_VW;
            val.number = t.number;
        } else if (strcmp(t.unit, "vh") == 0) {
            val.type   = CA_CSS_VAL_VH;
            val.number = t.number;
        } else if (strcmp(t.unit, "pt") == 0) {
            val.type   = CA_CSS_VAL_PX;
            val.number = t.number * (96.0f / 72.0f);
        } else if (strcmp(t.unit, "pc") == 0) {
            val.type   = CA_CSS_VAL_PX;
            val.number = t.number * 16.0f;
        } else if (strcmp(t.unit, "cm") == 0) {
            val.type   = CA_CSS_VAL_PX;
            val.number = t.number * 37.795f;
        } else if (strcmp(t.unit, "mm") == 0) {
            val.type   = CA_CSS_VAL_PX;
            val.number = t.number * 3.7795f;
        } else if (strcmp(t.unit, "in") == 0) {
            val.type   = CA_CSS_VAL_PX;
            val.number = t.number * 96.0f;
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
        if (strcasecmp(t.text, "inherit") == 0) {
            val.type = CA_CSS_VAL_INHERIT;
            return val;
        }
        if (strcasecmp(t.text, "initial") == 0 ||
            strcasecmp(t.text, "unset")   == 0) {
            val.type = CA_CSS_VAL_INITIAL;
            return val;
        }
        if (strcasecmp(t.text, "currentColor") == 0 ||
            strcasecmp(t.text, "currentcolor") == 0) {
            val.type = CA_CSS_VAL_CURRENT_COLOR;
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

/* Parse a color stop token (hash, function, or named color keyword) and
   return its packed RRGGBBAA value.  Returns 0x000000FF (opaque black) on
   failure.  The parser position is advanced past the color token(s). */
static uint32_t parse_gradient_color_stop(Parser *p)
{
    skip_ws(p);
    Token t = parser_peek(p);
    if (t.type == TOK_HASH) {
        parser_next(p);
        return parse_hex_color(t.text);
    }
    if (t.type == TOK_FUNCTION) {
        parser_next(p);
        if (strcasecmp(t.text, "rgb")  == 0) return parse_rgb_func(p, false);
        if (strcasecmp(t.text, "rgba") == 0) return parse_rgb_func(p, true);
        if (strcasecmp(t.text, "hsl")  == 0) return parse_hsl_func(p, false);
        if (strcasecmp(t.text, "hsla") == 0) return parse_hsl_func(p, true);
        if (strcasecmp(t.text, "oklch") == 0) return parse_oklch_func(p);
        if (strcasecmp(t.text, "color-mix") == 0) return parse_color_mix_func(p);
        /* Unknown function — consume to closing paren */
        int depth = 1;
        while (depth > 0) {
            Token tt = parser_next(p);
            if (tt.type == TOK_FUNCTION || tt.type == TOK_LPAREN) depth++;
            else if (tt.type == TOK_RPAREN) depth--;
            else if (tt.type == TOK_EOF) break;
        }
        return 0x000000FFu;
    }
    if (t.type == TOK_IDENT) {
        /* Named color */
        parser_next(p);
        struct { const char *name; uint32_t rgba; } ctbl[] = {
            {"transparent", 0x00000000u}, {"black",   0x000000FFu},
            {"white",       0xFFFFFFFFu}, {"red",     0xFF0000FFu},
            {"green",       0x008000FFu}, {"blue",    0x0000FFFFu},
            {"yellow",      0xFFFF00FFu}, {"cyan",    0x00FFFFFFu},
            {"magenta",     0xFF00FFFFu}, {"orange",  0xFFA500FFu},
            {"purple",      0x800080FFu}, {"pink",    0xFFC0CBFFu},
            {"gray",        0x808080FFu}, {"grey",    0x808080FFu},
            {"silver",      0xC0C0C0FFu}, {"lime",    0x00FF00FFu},
            {"navy",        0x000080FFu}, {"teal",    0x008080FFu},
            {"maroon",      0x800000FFu}, {"olive",   0x808000FFu},
            {"aqua",        0x00FFFFFFu}, {"fuchsia", 0xFF00FFFFu},
            {"coral",       0xFF7F50FFu}, {"salmon",  0xFA8072FFu},
            {"khaki",       0xF0E68CFFu}, {"violet",  0xEE82EEFFu},
            {"indigo",      0x4B0082FFu}, {"gold",    0xFFD700FFu},
            {"crimson",     0xDC143CFFu}, {"tan",     0xD2B48CFFu},
        };
        for (size_t i = 0; i < sizeof(ctbl)/sizeof(ctbl[0]); i++) {
            if (strcasecmp(t.text, ctbl[i].name) == 0)
                return ctbl[i].rgba;
        }
        return 0x000000FFu;
    }
    return 0x000000FFu;
}

/* Skip past any position hints inside a gradient after a color stop
   (e.g., "red 20%" — we accept the stop color but ignore percentage hints). */
static void skip_gradient_stop_hints(Parser *p)
{
    skip_ws(p);
    Token t = parser_peek(p);
    if (t.type == TOK_NUMBER || (t.type == TOK_DIMENSION)) {
        parser_next(p); /* skip percentage / length hint */
    }
}

/* Parse linear-gradient() or radial-gradient() and emit 5 declarations:
     CA_CSS_PROP_BACKGROUND       — start color (RRGGBBAA)
     CA_CSS_PROP_GRADIENT_COLOR2  — end color
     CA_CSS_PROP_GRADIENT_ANGLE   — degrees (linear) or 0 (radial)
     CA_CSS_PROP_GRADIENT_CX      — radial center x 0..1
     CA_CSS_PROP_GRADIENT_CY      — radial center y 0..1
   The draw_mode is encoded in the `keyword` field of BACKGROUND.
   Caller must have already consumed the function token (name without paren). */
static void parse_gradient_func(Parser *p, Ca_CssRule *rule, int is_radial)
{
    float angle = 180.0f; /* default: top→bottom */
    float cx = 0.5f, cy = 0.5f;

    skip_ws(p);
    Token t = parser_peek(p);

    if (!is_radial) {
        /* Check for optional angle: e.g., "to bottom", "45deg", "180deg" */
        if (t.type == TOK_IDENT && strcasecmp(t.text, "to") == 0) {
            parser_next(p); /* consume "to" */
            skip_ws(p);
            Token dir = parser_next(p);
            if (dir.type == TOK_IDENT) {
                if      (strcasecmp(dir.text, "bottom") == 0) angle = 180.0f;
                else if (strcasecmp(dir.text, "top")    == 0) angle = 0.0f;
                else if (strcasecmp(dir.text, "right")  == 0) angle = 90.0f;
                else if (strcasecmp(dir.text, "left")   == 0) angle = 270.0f;
                /* "to top right" etc. — peek for second direction word */
                skip_ws(p);
                Token dir2 = parser_peek(p);
                if (dir2.type == TOK_IDENT &&
                    (strcasecmp(dir2.text, "top")    == 0 || strcasecmp(dir2.text, "bottom") == 0 ||
                     strcasecmp(dir2.text, "left")   == 0 || strcasecmp(dir2.text, "right")  == 0)) {
                    parser_next(p); /* consume second direction, use approximate diagonal */
                    if (strcasecmp(dir.text, "top") == 0 && strcasecmp(dir2.text, "right") == 0)  angle = 45.0f;
                    else if (strcasecmp(dir.text, "bottom") == 0 && strcasecmp(dir2.text, "right") == 0) angle = 135.0f;
                    else if (strcasecmp(dir.text, "bottom") == 0 && strcasecmp(dir2.text, "left") == 0)  angle = 225.0f;
                    else if (strcasecmp(dir.text, "top") == 0 && strcasecmp(dir2.text, "left") == 0)     angle = 315.0f;
                }
            }
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_COMMA) { parser_next(p); }
        } else if (t.type == TOK_DIMENSION || t.type == TOK_NUMBER) {
            /* Numeric angle like "45deg" or "0.5turn" */
            parser_next(p);
            angle = t.number;
            if (t.unit[0] && (strcasecmp(t.unit, "turn") == 0 || strcasecmp(t.unit, "turns") == 0))
                angle *= 360.0f;
            else if (t.unit[0] && strcasecmp(t.unit, "rad") == 0)
                angle *= (180.0f / 3.14159265f);
            else if (t.unit[0] && strcasecmp(t.unit, "grad") == 0)
                angle *= 0.9f;
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_COMMA) parser_next(p);
        }
    } else {
        /* Radial: optional "circle at <cx> <cy>" prefix */
        if (t.type == TOK_IDENT && strcasecmp(t.text, "circle") == 0) {
            parser_next(p);
            skip_ws(p);
            Token at = parser_peek(p);
            if (at.type == TOK_IDENT && strcasecmp(at.text, "at") == 0) {
                parser_next(p);
                skip_ws(p);
                Token px = parser_next(p);
                if (px.type == TOK_NUMBER || px.type == TOK_DIMENSION)
                    cx = px.number / 100.0f; /* assume % */
                skip_ws(p);
                Token py = parser_peek(p);
                if (py.type == TOK_NUMBER || py.type == TOK_DIMENSION) {
                    parser_next(p);
                    cy = py.number / 100.0f;
                }
            }
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_COMMA) parser_next(p);
        }
    }

    /* Parse first color stop */
    uint32_t color1 = parse_gradient_color_stop(p);
    skip_gradient_stop_hints(p);
    skip_ws(p);
    t = parser_peek(p);
    if (t.type == TOK_COMMA) parser_next(p);

    /* Parse second color stop (we support exactly two stops for now) */
    uint32_t color2 = parse_gradient_color_stop(p);
    skip_gradient_stop_hints(p);

    /* Skip any additional stops (consume until closing paren) */
    int depth = 1;
    while (depth > 0) {
        Token tt = parser_next(p);
        if (tt.type == TOK_FUNCTION || tt.type == TOK_LPAREN) depth++;
        else if (tt.type == TOK_RPAREN) depth--;
        else if (tt.type == TOK_EOF) break;
    }

    /* Emit declarations */
    Ca_CssValue bg   = {0}; bg.type = CA_CSS_VAL_COLOR; bg.color = color1;
    /* Encode draw mode in keyword field: 2=linear-gradient, 3=radial-gradient.
       These match Ca_DrawMode values defined in ca_internal.h without requiring
       a cross-layer include. */
    bg.keyword = is_radial ? 3 : 2;
    add_decl(rule, CA_CSS_PROP_BACKGROUND, bg);

    Ca_CssValue c2   = {0}; c2.type = CA_CSS_VAL_COLOR; c2.color = color2;
    add_decl(rule, CA_CSS_PROP_GRADIENT_COLOR2, c2);

    Ca_CssValue ang  = {0}; ang.type = CA_CSS_VAL_NUMBER; ang.number = angle;
    add_decl(rule, CA_CSS_PROP_GRADIENT_ANGLE, ang);

    Ca_CssValue gcx  = {0}; gcx.type = CA_CSS_VAL_NUMBER; gcx.number = cx;
    add_decl(rule, CA_CSS_PROP_GRADIENT_CX, gcx);

    Ca_CssValue gcy  = {0}; gcy.type = CA_CSS_VAL_NUMBER; gcy.number = cy;
    add_decl(rule, CA_CSS_PROP_GRADIENT_CY, gcy);
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

        /* `background` shorthand: either a solid color or a gradient function.
           When a gradient function is detected, emit 5 declarations via
           parse_gradient_func. Otherwise promote to background-color. */
        if (prop_id == CA_CSS_PROP_BACKGROUND) {
            skip_ws(p);
            Token pk = parser_peek(p);
            if (pk.type == TOK_FUNCTION &&
                (strcasecmp(pk.text, "linear-gradient") == 0 ||
                 strcasecmp(pk.text, "radial-gradient") == 0)) {
                parser_next(p); /* consume function token (name without paren) */
                int is_radial = (strcasecmp(pk.text, "radial-gradient") == 0);
                int from = rule->decl_count;
                parse_gradient_func(p, rule, is_radial);
                consume_important(p, rule, from);
            } else {
                /* Solid color — treat as background-color */
                int from = rule->decl_count;
                Ca_CssValue val = parse_value(p, CA_CSS_PROP_BACKGROUND_COLOR);
                add_decl(rule, CA_CSS_PROP_BACKGROUND_COLOR, val);
                consume_important(p, rule, from);
            }
            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

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

        /* box-shadow shorthand: [inset] offset-x offset-y [blur] [spread] [color]
           We ignore inset/spread. Emits two decls:
             Decl A (keyword=0): type=COLOR, color=shadow color, keyword packs
                                 offset_x (upper 16 bits) and offset_y (lower 16 bits) as int16.
             Decl B (keyword=1): type=NUMBER, number=blur radius. */
        if (prop_id == CA_CSS_PROP_BOX_SHADOW) {
            float offset_x = 0.0f, offset_y = 0.0f, blur = 0.0f;
            uint32_t color = 0x00000080;
            int nums_seen = 0;
            float nums[3] = {0};

            while (1) {
                skip_ws(p);
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF) break;

                if (pk.type == TOK_HASH) {
                    parser_next(p);
                    color = parse_hex_color(pk.text);
                } else if (pk.type == TOK_FUNCTION) {
                    parser_next(p);
                    if (strcasecmp(pk.text, "rgb") == 0)
                        color = parse_rgb_func(p, false);
                    else if (strcasecmp(pk.text, "rgba") == 0)
                        color = parse_rgb_func(p, true);
                    else {
                        int depth = 1;
                        while (depth > 0) {
                            Token tt = parser_next(p);
                            if (tt.type == TOK_LPAREN || tt.type == TOK_FUNCTION) depth++;
                            else if (tt.type == TOK_RPAREN) depth--;
                            else if (tt.type == TOK_EOF) break;
                        }
                    }
                } else if (pk.type == TOK_IDENT) {
                    parser_next(p);
                    uint32_t named;
                    if (lookup_named_color(pk.text, &named))
                        color = named;
                    /* unknown keywords (e.g. "inset") are skipped */
                } else if ((pk.type == TOK_DIMENSION || pk.type == TOK_NUMBER) && nums_seen < 3) {
                    parser_next(p);
                    nums[nums_seen++] = pk.number;
                } else {
                    parser_next(p);
                }
            }

            if (nums_seen >= 2) {
                offset_x = nums[0];
                offset_y = nums[1];
                blur     = (nums_seen >= 3) ? nums[2] : 0.0f;
            }

            int ox = (int)offset_x;
            int oy = (int)offset_y;
            if (ox >  32767) ox =  32767; if (ox < -32768) ox = -32768;
            if (oy >  32767) oy =  32767; if (oy < -32768) oy = -32768;

            int from = rule->decl_count;
            Ca_CssValue va = {0};
            va.type    = CA_CSS_VAL_COLOR;
            va.color   = color;
            va.keyword = (int)(((uint32_t)(uint16_t)(int16_t)ox << 16) |
                                ((uint32_t)(uint16_t)(int16_t)oy & 0xFFFF));
            add_decl(rule, CA_CSS_PROP_BOX_SHADOW, va);

            Ca_CssValue vb = {0};
            vb.type    = CA_CSS_VAL_NUMBER;
            vb.number  = blur;
            vb.keyword = 1;
            add_decl(rule, CA_CSS_PROP_BOX_SHADOW, vb);
            consume_important(p, rule, from);

            skip_ws(p);
            t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* gap shorthand: gap: <row-gap> [<column-gap>] */
        if (strcasecmp(prop_name, "gap") == 0) {
            int from = rule->decl_count;
            Ca_CssValue v1 = parse_value(p, CA_CSS_PROP_ROW_GAP);
            skip_ws(p);
            Token pk = parser_peek(p);
            if (pk.type != TOK_SEMICOLON && pk.type != TOK_RBRACE && pk.type != TOK_EOF && pk.type != TOK_BANG) {
                Ca_CssValue v2 = parse_value(p, CA_CSS_PROP_COLUMN_GAP);
                add_decl(rule, CA_CSS_PROP_ROW_GAP, v1);
                add_decl(rule, CA_CSS_PROP_COLUMN_GAP, v2);
            } else {
                add_decl(rule, CA_CSS_PROP_GAP, v1);
                add_decl(rule, CA_CSS_PROP_ROW_GAP, v1);
                add_decl(rule, CA_CSS_PROP_COLUMN_GAP, v1);
            }
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* border-radius shorthand: border-radius: <tl> [<tr> [<br> [<bl>]]] [/ <vert>] */
        if (strcasecmp(prop_name, "border-radius") == 0) {
            int from = rule->decl_count;
            Ca_CssValue h[4] = {0}; int hcount = 0;

            while (hcount < 4) {
                skip_ws(p);
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF || pk.type == TOK_BANG) break;
                /* slash separates horizontal from vertical — treat as end for simplicity (average later) */
                if (pk.type == TOK_IDENT && strcmp(pk.text, "/") == 0) { parser_next(p); break; }
                h[hcount++] = parse_value(p, CA_CSS_PROP_BORDER_RADIUS);
            }
            /* Skip vertical part if present */
            while (1) {
                skip_ws(p);
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF || pk.type == TOK_BANG) break;
                parser_next(p);
            }

            Ca_CssValue tl, tr, br, bl;
            if (hcount == 0) { Ca_CssValue z = {0}; tl=tr=br=bl=z; }
            else if (hcount == 1) { tl=tr=br=bl=h[0]; }
            else if (hcount == 2) { tl=br=h[0]; tr=bl=h[1]; }
            else if (hcount == 3) { tl=h[0]; tr=bl=h[1]; br=h[2]; }
            else                  { tl=h[0]; tr=h[1]; br=h[2]; bl=h[3]; }

            add_decl(rule, CA_CSS_PROP_BORDER_RADIUS, tl); /* uniform fallback */
            add_decl(rule, CA_CSS_PROP_BORDER_TOP_LEFT_RADIUS,     tl);
            add_decl(rule, CA_CSS_PROP_BORDER_TOP_RIGHT_RADIUS,    tr);
            add_decl(rule, CA_CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS, br);
            add_decl(rule, CA_CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS,  bl);
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* outline shorthand: outline: <width> [<style>] [<color>] */
        if (strcasecmp(prop_name, "outline") == 0) {
            int from = rule->decl_count;
            bool got_w = false, got_c = false;
            while (1) {
                skip_ws(p);
                Token pk = parser_peek(p);
                if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF || pk.type == TOK_BANG) break;
                Ca_CssValue ov = parse_value(p, CA_CSS_PROP_NONE);
                if (ov.type == CA_CSS_VAL_COLOR && !got_c) {
                    add_decl(rule, CA_CSS_PROP_OUTLINE_COLOR, ov); got_c = true;
                } else if ((ov.type == CA_CSS_VAL_PX || ov.type == CA_CSS_VAL_NUMBER) && !got_w) {
                    add_decl(rule, CA_CSS_PROP_OUTLINE_WIDTH, ov); got_w = true;
                }
            }
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* flex shorthand: flex: none | auto | <grow> [<shrink> [<basis>]] */
        if (strcasecmp(prop_name, "flex") == 0 && prop_id == CA_CSS_PROP_NONE) {
            int from = rule->decl_count;
            skip_ws(p);
            Token pk = parser_peek(p);
            if (pk.type == TOK_IDENT &&
                (strcasecmp(pk.text, "none") == 0 || strcasecmp(pk.text, "auto") == 0)) {
                parser_next(p);
                bool is_auto = (strcasecmp(pk.text, "auto") == 0);
                Ca_CssValue gv = {0}, sv = {0};
                gv.type = CA_CSS_VAL_NUMBER; gv.number = is_auto ? 1.0f : 0.0f;
                sv.type = CA_CSS_VAL_NUMBER; sv.number = is_auto ? 1.0f : 0.0f;
                add_decl(rule, CA_CSS_PROP_FLEX_GROW, gv);
                add_decl(rule, CA_CSS_PROP_FLEX_SHRINK, sv);
            } else {
                Ca_CssValue vals[3] = {0}; int vc = 0;
                while (vc < 3) {
                    skip_ws(p);
                    pk = parser_peek(p);
                    if (pk.type == TOK_SEMICOLON || pk.type == TOK_RBRACE || pk.type == TOK_EOF || pk.type == TOK_BANG) break;
                    vals[vc++] = parse_value(p, CA_CSS_PROP_NONE);
                }
                if (vc >= 1) { Ca_CssValue gv = vals[0]; gv.type = CA_CSS_VAL_NUMBER; add_decl(rule, CA_CSS_PROP_FLEX_GROW,   gv); }
                if (vc >= 2) { Ca_CssValue sv = vals[1]; sv.type = CA_CSS_VAL_NUMBER; add_decl(rule, CA_CSS_PROP_FLEX_SHRINK, sv); }
                if (vc >= 3) add_decl(rule, CA_CSS_PROP_FLEX_BASIS, vals[2]);
            }
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* flex-flow shorthand: flex-flow: <direction> [<wrap>] */
        if (strcasecmp(prop_name, "flex-flow") == 0) {
            int from = rule->decl_count;
            Ca_CssValue dv = parse_value(p, CA_CSS_PROP_FLEX_DIRECTION);
            add_decl(rule, CA_CSS_PROP_FLEX_DIRECTION, dv);
            skip_ws(p);
            Token pk = parser_peek(p);
            if (pk.type != TOK_SEMICOLON && pk.type != TOK_RBRACE && pk.type != TOK_EOF && pk.type != TOK_BANG) {
                Ca_CssValue wv = parse_value(p, CA_CSS_PROP_FLEX_WRAP);
                add_decl(rule, CA_CSS_PROP_FLEX_WRAP, wv);
            }
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* place-items shorthand: place-items: <align-items> [<justify-items>] */
        if (strcasecmp(prop_name, "place-items") == 0) {
            int from = rule->decl_count;
            Ca_CssValue av = parse_value(p, CA_CSS_PROP_ALIGN_ITEMS);
            add_decl(rule, CA_CSS_PROP_ALIGN_ITEMS, av);
            skip_ws(p);
            Token pk = parser_peek(p);
            if (pk.type != TOK_SEMICOLON && pk.type != TOK_RBRACE && pk.type != TOK_EOF && pk.type != TOK_BANG)
                parser_next(p); /* consume justify-items (not stored, not in our model) */
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
            if (t.type == TOK_SEMICOLON) parser_next(p);
            continue;
        }

        /* place-content shorthand: place-content: <align-content> [<justify-content>] */
        if (strcasecmp(prop_name, "place-content") == 0) {
            int from = rule->decl_count;
            Ca_CssValue av = parse_value(p, CA_CSS_PROP_ALIGN_CONTENT);
            add_decl(rule, CA_CSS_PROP_ALIGN_CONTENT, av);
            skip_ws(p);
            Token pk = parser_peek(p);
            if (pk.type != TOK_SEMICOLON && pk.type != TOK_RBRACE && pk.type != TOK_EOF && pk.type != TOK_BANG) {
                Ca_CssValue jv = parse_value(p, CA_CSS_PROP_JUSTIFY_CONTENT);
                add_decl(rule, CA_CSS_PROP_JUSTIFY_CONTENT, jv);
            }
            consume_important(p, rule, from);
            skip_ws(p); t = parser_peek(p);
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
