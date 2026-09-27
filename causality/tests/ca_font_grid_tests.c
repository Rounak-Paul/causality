// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

/* ca_font_grid_tests.c — codepoint cell widths and monospace grid snapping
 * of glyphs borrowed from the fallback font layers.
 *
 * Whitebox: includes font.c to rasterise through the real atlas pages and
 * face-resolution chain without a GPU (the Vulkan upload is never reached).
 */

#include "../src/pch.h"
#include "../src/renderer/font.c"

#include <math.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/** Verify the generated width table against known UCD classifications. */
static bool test_cell_widths(void)
{
    static const struct { uint32_t cp; int width; } cases[] = {
        { 'a', 1 }, { 0x00AD, 1 }, { 0x00E9, 1 }, { 0x2500, 1 },
        { 0x23FA, 1 }, { 0x23F5, 1 }, { 0x23BF, 1 }, { 0x25CF, 1 },
        { 0x2733, 1 }, { 0x26A0, 1 }, { 0x276F, 1 }, { 0x1F1E6, 1 },
        { 0x0301, 0 }, { 0x200B, 0 }, { 0x200D, 0 }, { 0xFE0F, 0 },
        { 0x1160, 0 }, { 0xE0100, 0 }, { 0x0007, 0 },
        { 0x231A, 2 }, { 0x23E9, 2 }, { 0x2B50, 2 }, { 0x4E2D, 2 },
        { 0x3400, 2 }, { 0x2FFFD, 2 }, { 0xFF21, 2 }, { 0x1F642, 2 },
        { 0x1F7E0, 2 }, { 0x1F004, 2 }, { 0x3030, 2 },
        { 0x110000, 1 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (ca_codepoint_cell_width(cases[i].cp) != cases[i].width) {
            fprintf(stderr, "U+%04X: width %d, expected %d\n", cases[i].cp,
                    ca_codepoint_cell_width(cases[i].cp), cases[i].width);
            return false;
        }
    }
    return true;
}

/** Build a GPU-less Ca_Font over the embedded faces at a content scale. */
static bool make_font(Ca_Font *font, float content_scale)
{
    memset(font, 0, sizeof(*font));
    font->dirty_rect_storage = (Ca_DynArray)CA_DYN_ARRAY_INIT(Ca_FontDirtyRect);
    font->display_scale = content_scale;
    font->content_scale = content_scale;
    font->default_size  = CA_FONT_DEFAULT_SIZE_PX;
    font->atlas_w       = CA_FONT_ATLAS_W;
    font->atlas_h       = CA_FONT_ATLAS_H;
    FT_Library lib = NULL;
    CHECK(FT_Init_FreeType(&lib) == 0);
    font->ft_library = lib;
    font_load_embedded_face(lib, ca_embedded_font_data, ca_embedded_font_size,
                            "regular", &font->regular_face, &font->regular_data,
                            &font->regular_size);
    font_load_embedded_face(lib, ca_embedded_font_bold_data, ca_embedded_font_bold_size,
                            "bold", &font->bold_face, &font->bold_data,
                            &font->bold_size);
    font_load_embedded_face(lib, ca_embedded_symbols_font_data,
                            ca_embedded_symbols_font_size, "icons",
                            &font->icon_face, &font->icon_data, &font->icon_size);
    font_load_embedded_face(lib, ca_embedded_mono_symbols_font_data,
                            ca_embedded_mono_symbols_font_size, "mono symbols",
                            &font->mono_symbols_face, &font->mono_symbols_data,
                            &font->mono_symbols_size);
    font_load_embedded_face(lib, ca_embedded_emoji_font_data,
                            ca_embedded_emoji_font_size, "emoji",
                            &font->emoji_face, &font->emoji_data, &font->emoji_size);
    font_load_embedded_face(lib, ca_embedded_fallback_font_data,
                            ca_embedded_fallback_font_size, "fallback",
                            &font->fallback_face, &font->fallback_data,
                            &font->fallback_size);
    CHECK(font->regular_face && font->bold_face && font->mono_symbols_face &&
          font->emoji_face && font->fallback_face);
    font->atlas_rgba = (unsigned char *)calloc(1u, (size_t)CA_FONT_ATLAS_W * CA_FONT_ATLAS_H * 4u);
    CHECK(font->atlas_rgba);
    return true;
}

/** Release everything make_font created (no Vulkan objects exist). */
static void free_font(Ca_Font *font)
{
    for (int i = 0; i < CA_FONT_MAX_PAGES; i++) {
        CA_FREE(font->pages[i].chardata_block);
        ca_dyn_array_destroy(&font->pages[i].extra_glyph_storage);
        ca_dyn_array_destroy(&font->pages[i].extra_lookup_storage);
    }
    void *faces[] = { font->regular_face, font->bold_face, font->icon_face,
                      font->mono_symbols_face, font->emoji_face, font->fallback_face };
    for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); ++i)
        if (faces[i]) FT_Done_Face((FT_Face)faces[i]);
    FT_Done_FreeType((FT_Library)font->ft_library);
    CA_FREE(font->regular_data);
    CA_FREE(font->bold_data);
    CA_FREE(font->icon_data);
    CA_FREE(font->mono_symbols_data);
    CA_FREE(font->emoji_data);
    CA_FREE(font->fallback_data);
    free(font->atlas_rgba);
    ca_dyn_array_destroy(&font->dirty_rect_storage);
}

/** Claude Code / TUI glyphs must resolve from a real face, never '?'. */
static bool test_terminal_symbols_resolve(void)
{
    Ca_Font font;
    CHECK(make_font(&font, 1.0f));
    static const uint32_t symbols[] = {
        0x23F5, 0x23F8, 0x23FA, 0x23BF, 0x25CF, 0x25D0, 0x2733, 0x26A0,
        0x2713, 0x2717, 0x276F, 0x2192, 0x21B3, 0x2B1D, 0x28FF,
    };
    FT_Face faces[] = { (FT_Face)font.regular_face, (FT_Face)font.mono_symbols_face,
                        (FT_Face)font.emoji_face, (FT_Face)font.fallback_face };
    bool ok = true;
    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
        bool found = false;
        for (size_t f = 0; f < sizeof(faces) / sizeof(faces[0]) && !found; ++f)
            found = FT_Get_Char_Index(faces[f], symbols[i]) != 0;
        if (!found) {
            fprintf(stderr, "U+%04X has no glyph in any embedded face\n", symbols[i]);
            ok = false;
        }
    }
    free_font(&font);
    return ok;
}

/** Every glyph's advance equals its cell count times the '0' advance. */
static bool test_grid_snapping(void)
{
    static const uint32_t cps[] = {
        '#', 0x2500, 0x23FA, 0x23F5, 0x23BF, 0x25CF, 0x2733, 0x26A0, 0x2713,
        0x21B3, 0x0531, 0x1F642, 0x2B50, 0x231A, 0x4E2D, 0xFE0F, 0x200D,
    };
    const float scales[] = { 1.0f, 1.5f, 2.0f };
    for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); ++si) {
        Ca_Font font;
        CHECK(make_font(&font, scales[si]));
        for (float px = 10.0f; px <= 20.0f; px += 1.0f) {
            for (int bold = 0; bold <= 1; ++bold) {
                Ca_FontTier *tier = ca_font_select_tier_for_size(&font, px, bold != 0);
                CHECK(tier);
                CHECK(tier->underline_thickness > 0.0f && tier->strikeout_thickness > 0.0f);
                CHECK(tier->underline_position > 0.0f &&
                      tier->underline_position < -tier->descent + 1.0f);
                CHECK(tier->strikeout_position < 0.0f &&
                      tier->strikeout_position > -tier->ascent);
                Ca_Glyph *zero = ca_font_glyph_from_tier(tier, '0', NULL);
                CHECK(zero && zero->xadvance > 0.0f);
                for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); ++i) {
                    Ca_Glyph *g = ca_font_glyph_from_tier(tier, cps[i], NULL);
                    CHECK(g);
                    const float want = zero->xadvance * (float)ca_codepoint_cell_width(cps[i]);
                    if (fabsf(g->xadvance - want) > 0.01f) {
                        fprintf(stderr, "U+%04X scale %.1f px %.0f bold %d: advance %.3f, want %.3f\n",
                                cps[i], (double)scales[si], (double)px, bold,
                                (double)g->xadvance, (double)want);
                        free_font(&font);
                        return false;
                    }
                    /* Connector glyphs (box drawing, U+23BF) deliberately
                       ink into the neighbouring cell; only glyphs scaled
                       down from a wider face must fit their cells. */
                    const bool scaled = cps[i] == 0x1F642 || cps[i] == 0x2B50 ||
                                        cps[i] == 0x231A || cps[i] == 0x0531;
                    const bool has_ink = g->x1 > g->x0;
                    if (scaled && has_ink && (g->xoff < -1.0f || g->xoff2 > want + 1.0f)) {
                        fprintf(stderr, "U+%04X scale %.1f px %.0f: ink [%.2f, %.2f] outside cell %.2f\n",
                                cps[i], (double)scales[si], (double)px,
                                (double)g->xoff, (double)g->xoff2, (double)want);
                        free_font(&font);
                        return false;
                    }
                }
            }
        }
        free_font(&font);
    }
    return true;
}

int main(void)
{
    bool ok = true;
    ok = test_cell_widths() && ok;
    ok = test_terminal_symbols_resolve() && ok;
    ok = test_grid_snapping() && ok;
    printf("%s\n", ok ? "ca_font_grid_tests passed" : "ca_font_grid_tests FAILED");
    return ok ? 0 : 1;
}
