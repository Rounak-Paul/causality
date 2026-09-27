// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

typedef struct GLFWwindow GLFWwindow;

#include "../src/ui/paint.c"
#include "embedded_font.h"
#include <ft2build.h>
#include FT_FREETYPE_H

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/** Verify real glyph ink survives a label sized to its advance at multiple scales. */
static bool test_glyph_overhang(void)
{
    FT_Library library;
    FT_Face face;
    CHECK(FT_Init_FreeType(&library) == 0);
    CHECK(FT_New_Memory_Face(library, ca_embedded_font_data,
                           ca_embedded_font_size, 0, &face) == 0);
    unsigned overhangs = 0;
    const float scales[] = {1.0f, 1.12f, 1.25f, 1.5f, 2.0f};
    for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        for (int size = 8; size <= 32; ++size) {
            CHECK(FT_Set_Char_Size(face, 0, (FT_F26Dot6)(size * scales[i] * 64), 72, 72) == 0);
            CHECK(FT_Load_Char(face, 'w', FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) == 0);
            CHECK(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0);
            float advance = (float)face->glyph->advance.x / 64.0f;
            float right = face->glyph->bitmap_left + (float)face->glyph->bitmap.width;
            if (right <= advance) continue;
            ++overhangs;
            Ca_Node label = {0};
            label.x = 20; label.y = 20;
            label.w = advance / scales[i]; label.h = (float)size;
            ClipRect clip = text_clip_for_node(&label);
            CHECK(!clip.active || clip.x + clip.w >= label.x + right / scales[i]);
        }
    }
    CHECK(overhangs > 0);
    printf("Verified %u w glyphs with ink beyond their advance.\n", overhangs);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return true;
}

/** Verify explicit overflow, empty clips, and rounded ancestor bounds are retained. */
static bool test_overflow_boundaries(void)
{
    Ca_Node parent = {0};
    parent.x = 10; parent.y = 10; parent.w = 100; parent.h = 40;
    parent.desc.overflow_x = 1; parent.desc.overflow_y = 1;
    parent.desc.corner_radius = 8;
    Ca_Node label = {0};
    label.parent = &parent;
    label.x = 20; label.y = 20; label.w = 7; label.h = 12;
    ClipRect clip = text_clip_for_node(&label);
    CHECK(clip.active && clip.x == 10 && clip.w == 100);
    CHECK(clip.y == 10 && clip.h == 40 && clip.radius == 8);
    label.desc.overflow_x = 1;
    clip = text_clip_for_node(&label);
    CHECK(clip.x == 20 && clip.w == 7 && clip.y == 10 && clip.h == 40);
    label.desc.overflow_x = 0;
    label.desc.overflow_y = 1;
    clip = text_clip_for_node(&label);
    CHECK(clip.x == 10 && clip.w == 100 && clip.y == 20 && clip.h == 12);
    label.desc.overflow_x = 1;
    label.w = 0;
    clip = text_clip_for_node(&label);
    CHECK(clip.active && clip.w == 0);
    label.w = 7; label.x = 200;
    clip = text_clip_for_node(&label);
    CHECK(clip.active && clip.w == 0);
    return true;
}

/** Return the text-decoration flags the first rule of css declares. */
static bool parse_decoration(const char *css, bool *declared, unsigned *flags)
{
    Ca_Stylesheet *ss = ca_css_parse(css);
    CHECK(ss && ss->rule_count == 1);
    *declared = false;
    for (int i = 0; i < ss->rules[0].decl_count; ++i) {
        const Ca_CssDecl *d = &ss->rules[0].decls[i];
        if (d->prop != CA_CSS_PROP_TEXT_DECORATION) continue;
        CHECK(d->value.type == CA_CSS_VAL_NUMBER);
        *declared = true;
        *flags = (unsigned)d->value.number;
    }
    ca_css_destroy(ss);
    return true;
}

/** Verify text-decoration parses to line flags, including combinations. */
static bool test_decoration_parsing(void)
{
    static const struct { const char *css; bool declared; unsigned flags; } cases[] = {
        { ".a { text-decoration: underline; }", true, CA_TEXT_DECORATION_UNDERLINE },
        { ".a { text-decoration: line-through; }", true, CA_TEXT_DECORATION_LINE_THROUGH },
        { ".a { text-decoration: underline line-through; }", true,
          CA_TEXT_DECORATION_UNDERLINE | CA_TEXT_DECORATION_LINE_THROUGH },
        { ".a { text-decoration: overline underline !important; }", true,
          CA_TEXT_DECORATION_OVERLINE | CA_TEXT_DECORATION_UNDERLINE },
        { ".a { text-decoration: none; }", true, 0u },
        { ".a { text-decoration: wavy-nonsense; }", false, 0u },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bool declared = false;
        unsigned flags = 0u;
        CHECK(parse_decoration(cases[i].css, &declared, &flags));
        CHECK(declared == cases[i].declared);
        if (declared) CHECK(flags == cases[i].flags);
    }
    return true;
}

/** Verify each decoration flag emits one crisp rect at its metric position. */
static bool test_decoration_rects(void)
{
    Ca_Window win;
    memset(&win, 0, sizeof(win));
    win.draw_cmd_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_DrawCmd);
    Ca_FontTier tier;
    memset(&tier, 0, sizeof(tier));
    tier.ascent = 12.0f;
    tier.underline_position  = 2.0f;
    tier.underline_thickness = 1.0f;
    tier.strikeout_position  = -4.0f;
    tier.strikeout_thickness = 1.0f;
    Ca_Node node;
    memset(&node, 0, sizeof(node));
    node.desc.text_decoration = (uint8_t)(CA_TEXT_DECORATION_UNDERLINE |
                                          CA_TEXT_DECORATION_LINE_THROUGH |
                                          CA_TEXT_DECORATION_OVERLINE);
    const float rgba[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    const float scales[] = { 1.0f, 2.0f };
    for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); ++si) {
        win.draw_cmd_count = 0;
        paint_text_decoration(&win, &node, &tier, 1.0f, scales[si], 10.0f, 50.0f,
                              100.0f, rgba, (ClipRect){0});
        CHECK(win.draw_cmd_count == 3);
        const float want_y[3] = { 102.0f, 96.0f, 88.5f };
        for (uint32_t i = 0; i < 3; ++i) {
            const Ca_DrawCmd *cmd = &win.draw_cmds[i];
            CHECK(cmd->type == CA_DRAW_RECT && cmd->in_use);
            CHECK(cmd->x == 10.0f && cmd->w == 40.0f);
            CHECK(cmd->h * scales[si] >= 1.0f);
            CHECK(fabsf(cmd->y + cmd->h * 0.5f - want_y[i]) <= 0.5f);
            CHECK(cmd->r == 1.0f && cmd->g == 0.5f);
        }
    }
    win.draw_cmd_count = 0;
    node.desc.text_decoration = 0u;
    paint_text_decoration(&win, &node, &tier, 1.0f, 1.0f, 10.0f, 50.0f,
                          100.0f, rgba, (ClipRect){0});
    CHECK(win.draw_cmd_count == 0);
    ca_dyn_array_destroy(&win.draw_cmd_storage);
    return true;
}

/** Return the resolved text-decoration-style keyword of css's first rule, or -1. */
static int parse_decoration_style(const char *css)
{
    Ca_Stylesheet *ss = ca_css_parse(css);
    if (!ss || ss->rule_count != 1) { ca_css_destroy(ss); return -2; }
    int keyword = -1;
    for (int i = 0; i < ss->rules[0].decl_count; ++i) {
        const Ca_CssDecl *d = &ss->rules[0].decls[i];
        if (d->prop == CA_CSS_PROP_TEXT_DECORATION_STYLE && d->value.type == CA_CSS_VAL_KEYWORD)
            keyword = d->value.keyword;
    }
    ca_css_destroy(ss);
    return keyword;
}

/** Verify text-decoration-style parses standalone and inside the shorthand. */
static bool test_decoration_style_parsing(void)
{
    CHECK(parse_decoration_style(".a { text-decoration-style: wavy; }") ==
          CA_CSS_TEXT_DECORATION_STYLE_WAVY);
    CHECK(parse_decoration_style(".a { text-decoration-style: dotted; }") ==
          CA_CSS_TEXT_DECORATION_STYLE_DOTTED);
    CHECK(parse_decoration_style(".a { text-decoration: underline dashed; }") ==
          CA_CSS_TEXT_DECORATION_STYLE_DASHED);
    CHECK(parse_decoration_style(".a { text-decoration: double line-through; }") ==
          CA_CSS_TEXT_DECORATION_STYLE_DOUBLE);
    CHECK(parse_decoration_style(".a { text-decoration: underline; }") == -1);
    bool declared = false;
    unsigned flags = 0u;
    CHECK(parse_decoration(".a { text-decoration: underline wavy; }", &declared, &flags));
    CHECK(declared && flags == CA_TEXT_DECORATION_UNDERLINE);
    return true;
}

/** Verify every decoration style emits the right draw mode and geometry. */
static bool test_decoration_style_rects(void)
{
    Ca_Window win;
    memset(&win, 0, sizeof(win));
    win.draw_cmd_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_DrawCmd);
    Ca_FontTier tier;
    memset(&tier, 0, sizeof(tier));
    tier.logical_px = 13.0f;
    tier.ascent = 12.0f;
    tier.underline_position  = 2.0f;
    tier.underline_thickness = 1.0f;
    Ca_Node node;
    memset(&node, 0, sizeof(node));
    node.desc.text_decoration = (uint8_t)CA_TEXT_DECORATION_UNDERLINE;
    const float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    node.desc.text_decoration_style = CA_TEXT_DECORATION_STYLE_DOUBLE;
    win.draw_cmd_count = 0;
    paint_text_decoration(&win, &node, &tier, 1.0f, 1.0f, 0.0f, 80.0f, 100.0f, rgba, (ClipRect){0});
    CHECK(win.draw_cmd_count == 2);
    CHECK(win.draw_cmds[0].draw_mode == CA_DRAW_MODE_NORMAL);
    CHECK(win.draw_cmds[1].y - win.draw_cmds[0].y == 2.0f);

    node.desc.text_decoration_style = CA_TEXT_DECORATION_STYLE_DOTTED;
    win.draw_cmd_count = 0;
    paint_text_decoration(&win, &node, &tier, 1.0f, 1.0f, 0.0f, 80.0f, 100.0f, rgba, (ClipRect){0});
    CHECK(win.draw_cmd_count == 1);
    CHECK(win.draw_cmds[0].draw_mode == CA_DRAW_MODE_DASH);
    CHECK(win.draw_cmds[0].blur_radius == 2.0f && win.draw_cmds[0].gradient_cx == 1.0f);

    node.desc.text_decoration_style = CA_TEXT_DECORATION_STYLE_DASHED;
    win.draw_cmd_count = 0;
    paint_text_decoration(&win, &node, &tier, 1.0f, 2.0f, 0.0f, 80.0f, 100.0f, rgba, (ClipRect){0});
    CHECK(win.draw_cmd_count == 1);
    CHECK(win.draw_cmds[0].draw_mode == CA_DRAW_MODE_DASH);
    CHECK(win.draw_cmds[0].gradient_cx < win.draw_cmds[0].blur_radius);

    node.desc.text_decoration_style = CA_TEXT_DECORATION_STYLE_WAVY;
    win.draw_cmd_count = 0;
    paint_text_decoration(&win, &node, &tier, 1.0f, 1.0f, 0.0f, 80.0f, 100.0f, rgba, (ClipRect){0});
    CHECK(win.draw_cmd_count == 1);
    const Ca_DrawCmd *wave = &win.draw_cmds[0];
    CHECK(wave->draw_mode == CA_DRAW_MODE_WAVE);
    CHECK(wave->h >= 5.0f && wave->gradient_cx == 1.5f && wave->blur_radius >= 4.0f);
    CHECK(fabsf(wave->y + wave->h * 0.5f - 102.0f) <= 0.5f);
    ca_dyn_array_destroy(&win.draw_cmd_storage);
    return true;
}

/** Return the text-decoration-color declared by css's first rule (value type out). */
static uint32_t parse_decoration_color(const char *css, int *type)
{
    Ca_Stylesheet *ss = ca_css_parse(css);
    uint32_t color = 0u;
    *type = -1;
    if (ss && ss->rule_count == 1) {
        for (int i = 0; i < ss->rules[0].decl_count; ++i) {
            const Ca_CssDecl *d = &ss->rules[0].decls[i];
            if (d->prop != CA_CSS_PROP_TEXT_DECORATION_COLOR) continue;
            *type = (int)d->value.type;
            color = d->value.color;
        }
    }
    ca_css_destroy(ss);
    return color;
}

/** Verify text-decoration-color parses standalone and in the shorthand. */
static bool test_decoration_color_parsing(void)
{
    int type = -1;
    CHECK(parse_decoration_color(".a { text-decoration-color: #ff0000; }", &type) == 0xFF0000FFu);
    CHECK(type == CA_CSS_VAL_COLOR);
    CHECK(parse_decoration_color(".a { text-decoration: underline wavy rgb(0, 128, 255); }", &type) ==
          0x0080FFFFu);
    (void)parse_decoration_color(".a { text-decoration: underline currentColor; }", &type);
    CHECK(type == CA_CSS_VAL_CURRENT_COLOR);
    (void)parse_decoration_color(".a { text-decoration: underline; }", &type);
    CHECK(type == -1);
    bool declared = false;
    unsigned flags = 0u;
    CHECK(parse_decoration(".a { text-decoration: red underline; }", &declared, &flags));
    CHECK(declared && flags == CA_TEXT_DECORATION_UNDERLINE);
    return true;
}

/** Verify decoration colour overrides the text colour and forces a repaint. */
static bool test_decoration_color_paint(void)
{
    Ca_Window win;
    memset(&win, 0, sizeof(win));
    win.draw_cmd_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_DrawCmd);
    Ca_FontTier tier;
    memset(&tier, 0, sizeof(tier));
    tier.logical_px = 13.0f;
    tier.underline_position  = 2.0f;
    tier.underline_thickness = 1.0f;
    Ca_Node node;
    memset(&node, 0, sizeof(node));
    node.desc.text_decoration = (uint8_t)CA_TEXT_DECORATION_UNDERLINE;
    const float text[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    paint_text_decoration(&win, &node, &tier, 1.0f, 1.0f, 0.0f, 10.0f, 50.0f, text, (ClipRect){0});
    CHECK(win.draw_cmd_count == 1 && win.draw_cmds[0].g == 1.0f);

    node.desc.text_decoration_color = 0xFF0000FFu;
    win.draw_cmd_count = 0;
    paint_text_decoration(&win, &node, &tier, 1.0f, 1.0f, 0.0f, 10.0f, 50.0f, text, (ClipRect){0});
    CHECK(win.draw_cmd_count == 1);
    CHECK(win.draw_cmds[0].r == 1.0f && win.draw_cmds[0].g == 0.0f && win.draw_cmds[0].b == 0.0f);
    ca_dyn_array_destroy(&win.draw_cmd_storage);

    Ca_NodeDesc a, b;
    memset(&a, 0, sizeof(a));
    b = a;
    b.text_decoration = (uint8_t)CA_TEXT_DECORATION_UNDERLINE;
    CHECK(content_desc_changed(&a, &b));
    b = a;
    b.text_decoration_style = CA_TEXT_DECORATION_STYLE_WAVY;
    CHECK(content_desc_changed(&a, &b));
    b = a;
    b.text_decoration_color = 0xFF0000FFu;
    CHECK(content_desc_changed(&a, &b));
    return true;
}

/** Run glyph clipping and text-decoration regressions without a window or GPU. */
int main(void)
{
    bool ok = test_glyph_overhang();
    ok = test_overflow_boundaries() && ok;
    ok = test_decoration_parsing() && ok;
    ok = test_decoration_rects() && ok;
    ok = test_decoration_style_parsing() && ok;
    ok = test_decoration_style_rects() && ok;
    ok = test_decoration_color_parsing() && ok;
    ok = test_decoration_color_paint() && ok;
    return ok ? 0 : 1;
}
