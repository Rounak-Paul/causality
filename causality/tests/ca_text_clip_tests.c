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

/** Run glyph clipping regressions without a window or GPU. */
int main(void)
{
    return test_glyph_overhang() && test_overflow_boundaries() ? 0 : 1;
}
