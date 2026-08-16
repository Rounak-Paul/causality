// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* font.h — dynamic GPU font atlas with regular, bold, and icon glyphs.

   Backend: FreeType rasterisation with Causality-owned LCD/grayscale policy.
   The atlas is RGBA8: text glyphs may store per-channel LCD coverage on 1x
   displays, while icons and HiDPI glyphs use grayscale coverage for portable
   compositing.

   Font sizes are not baked up front.  Glyphs are rasterised on demand
   into a page-based LRU atlas keyed by visual pixel size and style.  Low-DPI
   displays render from higher internal FreeType samples reconstructed into
   final atlas coverage instead of relying on platform font output. */
#pragma once

#include "ca_internal.h"
#include <math.h>

#define CA_FONT_RANGE_COUNT    6
#define CA_FONT_TEXT_RANGES    1    /* ASCII + Latin-1 (range 0) */
#define CA_FONT_ICON_RANGES    5    /* icon codepoint ranges (1-5) */
#define CA_FONT_STYLE_COUNT    2    /* regular=0, bold=1 */
#define CA_FONT_STYLE_REGULAR  0
#define CA_FONT_STYLE_BOLD     1
#define CA_FONT_DEFAULT_SIZE_PX 12.0f
#define CA_FONT_TEXT_SUPERSAMPLE_LOW_DPI_SMALL 1
#define CA_FONT_TEXT_SUPERSAMPLE_LOW_DPI_LARGE 2
#define CA_FONT_TEXT_SUPERSAMPLE_HIDPI         1
#define CA_FONT_ATLAS_W        4096
#define CA_FONT_ATLAS_H        4096
#define CA_FONT_PAGE_SIZE      1024
#define CA_FONT_MAX_PAGES      ((CA_FONT_ATLAS_W / CA_FONT_PAGE_SIZE) * \
                                (CA_FONT_ATLAS_H / CA_FONT_PAGE_SIZE))

/* Per-glyph atlas record.  Fields mirror what stb_truetype's packedchar
   exposed, so the layout/paint call-sites translate one-to-one.  All
   coordinates are in baked-pixel space (i.e. logical_px * content scale). */
typedef struct Ca_Glyph {
    uint16_t x0, y0, x1, y1;  /* atlas rect, in atlas pixels */
    float    xoff,  yoff;     /* top-left of glyph relative to pen origin */
    float    xoff2, yoff2;    /* bottom-right of glyph relative to pen origin */
    float    xadvance;        /* horizontal pen advance after drawing */
    uint8_t  valid;           /* glyph was looked up/rendered, including spaces */
} Ca_Glyph;

typedef struct Ca_FontExtraGlyph {
    uint32_t codepoint;
    Ca_Glyph glyph;
} Ca_FontExtraGlyph;

/* Emitted quad in screen-space (x0..x1, y0..y1) and atlas UV (s0..t1). */
typedef struct Ca_GlyphQuad {
    float x0, y0, x1, y1;
    float s0, t0, s1, t1;
} Ca_GlyphQuad;

typedef struct Ca_GlyphRange {
    int       first_codepoint;
    int       num_chars;
    Ca_Glyph *chardata;
} Ca_GlyphRange;

typedef struct Ca_FontTier {
    float         logical_px;
    float         baked_px;
    Ca_GlyphRange ranges[CA_FONT_RANGE_COUNT];
    Ca_Glyph     *chardata_block;
    Ca_DynArray   extra_glyph_storage;
    Ca_DynArray   extra_lookup_storage;
    Ca_FontExtraGlyph *extra_glyphs;
    uint32_t     *extra_lookup;
    float         ascent;
    float         descent;
    float         line_gap;
    bool          packed;
    struct Ca_Font     *owner;
    bool          dynamic_page;
    uint8_t       style;
    uint16_t      page_index;
    uint16_t      origin_x, origin_y;
    uint16_t      shelf_x, shelf_y, shelf_h;
    uint32_t      size_key;
    uint64_t      last_used_frame;
} Ca_FontTier;

typedef struct Ca_FontDirtyRect {
    uint16_t x, y, w, h;
} Ca_FontDirtyRect;

typedef struct Ca_Font {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;

    int   atlas_w, atlas_h;
    float display_scale;
    float content_scale;
    float default_size;

    struct Ca_Instance *owner;
    Ca_FontTier pages[CA_FONT_MAX_PAGES];
    uint64_t    frame_counter;
    uint64_t    atlas_generation;
    unsigned char *atlas_rgba;
    Ca_DynArray dirty_rect_storage;
    bool        dirty_full;
    void       *ft_library;
    void       *regular_face;
    void       *bold_face;
    void       *icon_face;
    void       *fallback_face;
    unsigned char *regular_data;
    size_t      regular_size;
    unsigned char *bold_data;
    size_t      bold_size;
    unsigned char *icon_data;
    size_t      icon_size;
    unsigned char *fallback_data;
    size_t      fallback_size;
} Ca_Font;

/* Select or create the exact visual-size page for this scaled pixel size. */
Ca_FontTier *ca_font_select_tier_for_size(Ca_Font *font,
                                          float desired_px,
                                          bool bold);

/* Return the regular tier nearest to desired_px. */
static inline Ca_FontTier *ca_font_tier(Ca_Font *font, float desired_px)
{
    return ca_font_select_tier_for_size(font, desired_px, false);
}

/* Select regular or bold tier at the default size.  Kept for backward
   compatibility; call-sites that already know desired_px should prefer
   ca_font_select_tier_for_size(). */
static inline Ca_FontTier *ca_font_select_tier(Ca_Font *font, bool bold)
{
    return ca_font_select_tier_for_size(font, font->default_size, bold);
}

/* Look up or lazily rasterise a glyph within a dynamic atlas page. */
Ca_Glyph *ca_font_glyph_from_tier(Ca_FontTier *tier, uint32_t cp,
                                  Ca_FontTier **out_tier);

static inline Ca_Glyph *ca_font_glyph(Ca_FontTier *tier, uint32_t cp)
{
    return ca_font_glyph_from_tier(tier, cp, NULL);
}

static inline float ca_font_glyph_cs_eff(Ca_FontTier *glyph_tier,
                                         float desired_size,
                                         float content_scale_over_ui_scale)
{
    float font_scale = desired_size / glyph_tier->logical_px;
    return content_scale_over_ui_scale / font_scale;
}

/* Compute a glyph quad from a Ca_Glyph record.
   Baseline is snapped once for the whole line via the caller-supplied
   *ypos; per-glyph offsets are applied without re-rounding so all
   characters on the line stay aligned to the same baseline.            */
static inline void ca_font_get_quad(const Ca_Glyph *g,
                                     int atlas_w, int atlas_h,
                                     float *xpos, float *ypos,
                                     Ca_GlyphQuad *q)
{
    float ipw = 1.0f / (float)atlas_w;
    float iph = 1.0f / (float)atlas_h;
    float x = *xpos + g->xoff;
    float y_base = (float)(int)(*ypos + 0.5f);
    q->x0 = x;
    q->y0 = y_base + g->yoff;
    q->x1 = x + g->xoff2 - g->xoff;
    q->y1 = y_base + g->yoff2;
    q->s0 = (float)g->x0 * ipw;
    q->t0 = (float)g->y0 * iph;
    q->s1 = (float)g->x1 * ipw;
    q->t1 = (float)g->y1 * iph;
    *xpos += g->xadvance;
}

/* Decode one UTF-8 codepoint, advancing the pointer. */
static inline uint32_t ca_utf8_decode(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    uint32_t cp;
    if (s[0] < 0x80) {
        cp = s[0]; *pp += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *pp += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6)  | (s[2] & 0x3F);
        *pp += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) |
             ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6)  | (s[3] & 0x3F);
        *pp += 4;
    } else {
        cp = 0xFFFD; *pp += 1;
    }
    return cp;
}

bool ca_font_create(Ca_Instance *inst, GLFWwindow *glfw_win,
                    Ca_Font *out_font,
                    const char *regular_path, const char *bold_path);

/** Create a font atlas from in-memory font data.
    regular_data/regular_size: required — bundled UI font data.
    bold_data/bold_size: optional (pass NULL/0 to skip bold tier).
    Glyphs and icons are rasterised lazily at scaled runtime sizes into the
    dynamic atlas; creation only uploads an empty atlas and primes defaults. */
bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *regular_data, unsigned int regular_size,
                                const unsigned char *bold_data,    unsigned int bold_size);

void ca_font_destroy(Ca_Instance *inst, Ca_Font *font);
void ca_font_begin_frame(Ca_Font *font);
void ca_font_touch_page(Ca_Font *font, int page_index);
void ca_font_flush_uploads(Ca_Instance *inst, Ca_Font *font);
