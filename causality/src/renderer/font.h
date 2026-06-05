// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* font.h — GPU font atlas with regular and bold styles.

   Backend: FreeType with autohinter and the default LCD filter
   ([1,4,7,4,1]/17) for ClearType-style horizontal subpixel rendering.
   Each glyph is rasterised at 3x horizontal resolution; the filter
   averages neighbouring subpixels into per-channel R/G/B coverage so
   the shader can use the LCD stripe geometry of the monitor for an
   effective 3x horizontal sharpness boost.

   The atlas is an RGBA8 image: .r/.g/.b hold per-subpixel coverage,
   .a holds the luma-equivalent coverage used for the fallback alpha
   channel and dual-source destination weight.  Glyph quads are
   dual-source-blended in linear light against the sRGB framebuffer
   for gamma-correct ClearType.

   Both regular and bold tiers are baked into the same atlas, along
   with the Nerd Font icon ranges.                                   */
#pragma once

#include "ca_internal.h"
#include <math.h>

#define CA_FONT_RANGE_COUNT    6
#define CA_FONT_TEXT_RANGES    1    /* ASCII + Latin-1 (range 0) */
#define CA_FONT_ICON_RANGES    5    /* icon codepoint ranges (1-5) */
#define CA_FONT_STYLE_COUNT    2    /* regular=0, bold=1 */
#define CA_FONT_STYLE_REGULAR  0
#define CA_FONT_STYLE_BOLD     1
#define CA_FONT_MAX_SIZES     20    /* maximum distinct baked sizes in one atlas */
#define CA_FONT_DEFAULT_SIZE_PX 12.0f /* default logical size; also the ONE size
                                         at which all icon/Nerd-Font ranges are
                                         baked — text-only tiers fall back to this
                                         tier for icon glyph lookups. */

/* Per-glyph atlas record.  Fields mirror what stb_truetype's packedchar
   exposed, so the layout/paint call-sites translate one-to-one.  All
   coordinates are in baked-pixel space (i.e. logical_px * content_scale). */
typedef struct Ca_Glyph {
    uint16_t x0, y0, x1, y1;  /* atlas rect, in atlas pixels */
    float    xoff,  yoff;     /* top-left of glyph relative to pen origin */
    float    xoff2, yoff2;    /* bottom-right of glyph relative to pen origin */
    float    xadvance;        /* horizontal pen advance after drawing */
} Ca_Glyph;

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
    float         ascent;
    float         descent;
    float         line_gap;
    bool          packed;
    /* For text-only tiers (all sizes except CA_FONT_DEFAULT_SIZE_PX): points to
       the full-range icon tier so ca_font_glyph can fall back when a codepoint
       outside the baked text range is requested.  NULL on the icon tier itself. */
    struct Ca_FontTier *icon_fallback;
} Ca_FontTier;

typedef struct Ca_Font {
    /* size_tiers[size_slot][style]: style 0=regular, 1=bold.           */
    Ca_FontTier  size_tiers[CA_FONT_MAX_SIZES][CA_FONT_STYLE_COUNT];
    int          baked_size_count;               /* how many size slots are populated */
    float        baked_logicals[CA_FONT_MAX_SIZES]; /* logical_px for each slot         */

    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;

    int   atlas_w, atlas_h;
    float content_scale;
    float default_size;
} Ca_Font;

/* Select the tier whose baked size is nearest to desired_px, for the
   given style.  Falls back to regular when bold is not packed at that
   size.  This is the primary tier-selection call; all text rendering
   paths should call this so glyphs are rasterised at the closest
   available baked size rather than being scaled from a mismatched one. */
static inline Ca_FontTier *ca_font_select_tier_for_size(Ca_Font *font,
                                                        float desired_px,
                                                        bool bold)
{
    int best = 0;
    float best_diff = fabsf(font->baked_logicals[0] - desired_px);
    for (int i = 1; i < font->baked_size_count; i++) {
        float diff = fabsf(font->baked_logicals[i] - desired_px);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    if (bold && font->size_tiers[best][CA_FONT_STYLE_BOLD].packed)
        return &font->size_tiers[best][CA_FONT_STYLE_BOLD];
    return &font->size_tiers[best][CA_FONT_STYLE_REGULAR];
}

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

/* Look up glyph data for a Unicode codepoint within a tier.  If the codepoint
   is not found in this tier (e.g. an icon in a text-only tier), the lookup
   transparently retries on icon_fallback so callers never need to know which
   size has the full icon ranges baked. */
static inline Ca_Glyph *ca_font_glyph_from_tier(Ca_FontTier *tier, uint32_t cp,
                                                Ca_FontTier **out_tier)
{
    for (int i = 0; i < CA_FONT_RANGE_COUNT; i++) {
        Ca_GlyphRange *r = &tier->ranges[i];
        if (r->num_chars > 0 &&
            cp >= (uint32_t)r->first_codepoint &&
            cp <  (uint32_t)(r->first_codepoint + r->num_chars)) {
            if (out_tier) *out_tier = tier;
            return &r->chardata[cp - r->first_codepoint];
        }
    }
    /* Not found — try the icon fallback tier (covers icon codepoints not
       baked into this text-only tier).  Callers that scale glyphs must use
       out_tier: fallback glyph metrics come from CA_FONT_DEFAULT_SIZE_PX, not
       from the originally selected text tier. */
    if (tier->icon_fallback) {
        Ca_FontTier *fb = tier->icon_fallback;
        for (int i = 0; i < CA_FONT_RANGE_COUNT; i++) {
            Ca_GlyphRange *r = &fb->ranges[i];
            if (r->num_chars > 0 &&
                cp >= (uint32_t)r->first_codepoint &&
                cp <  (uint32_t)(r->first_codepoint + r->num_chars)) {
                if (out_tier) *out_tier = fb;
                return &r->chardata[cp - r->first_codepoint];
            }
        }
    }
    return NULL;
}

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
    regular_data/regular_size: required — Ubuntu Nerd Font Regular (text + icons).
    bold_data/bold_size: optional (pass NULL/0 to skip bold tier).
    The default logical size and icon-baking size are fixed at
    CA_FONT_DEFAULT_SIZE_PX; the full standard size set is baked automatically. */
bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *regular_data, unsigned int regular_size,
                                const unsigned char *bold_data,    unsigned int bold_size);

/** Detect the platform's default proportional UI font.
    Returns true and writes the path into out_path on success. */
bool ca_font_detect_system(char *out_path, size_t max_len);

void ca_font_destroy(Ca_Instance *inst, Ca_Font *font);
