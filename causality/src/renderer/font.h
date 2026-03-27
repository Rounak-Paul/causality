/* font.h — GPU font atlas with regular and bold styles.
   Uses stb_truetype pack API for crisp text at the configured font size.
   Both regular and bold Ubuntu Nerd Font are baked into a single atlas.
   Nerd Font icon codepoint ranges are baked alongside ASCII. */
#pragma once

#include "ca_internal.h"
#include <stb_truetype.h>
#include <math.h>

#define CA_FONT_RANGE_COUNT    6
#define CA_FONT_TEXT_RANGES    1    /* ASCII + Latin-1 (range 0) */
#define CA_FONT_ICON_RANGES    5    /* icon codepoint ranges (1-5) */
#define CA_FONT_STYLE_COUNT    2    /* regular=0, bold=1 */
#define CA_FONT_STYLE_REGULAR  0
#define CA_FONT_STYLE_BOLD     1

typedef struct Ca_GlyphRange {
    int               first_codepoint;
    int               num_chars;
    stbtt_packedchar *chardata;
} Ca_GlyphRange;

typedef struct Ca_FontTier {
    float             logical_px;
    float             baked_px;
    Ca_GlyphRange     ranges[CA_FONT_RANGE_COUNT];
    stbtt_packedchar *chardata_block;
    float             ascent;
    float             descent;
    float             line_gap;
    bool              packed;
} Ca_FontTier;

typedef struct Ca_Font {
    Ca_FontTier tiers[CA_FONT_STYLE_COUNT];  /* [0]=regular, [1]=bold */

    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;

    int   atlas_w, atlas_h;
    float content_scale;
    float default_size;
} Ca_Font;

/* Return the regular font tier. */
static inline Ca_FontTier *ca_font_tier(Ca_Font *font, float desired_px)
{
    (void)desired_px;
    return &font->tiers[CA_FONT_STYLE_REGULAR];
}

/* Select regular or bold tier based on a boolean flag. Falls back to regular
   if bold was not baked (bold_data was NULL at creation time). */
static inline Ca_FontTier *ca_font_select_tier(Ca_Font *font, bool bold)
{
    if (bold && font->tiers[CA_FONT_STYLE_BOLD].packed)
        return &font->tiers[CA_FONT_STYLE_BOLD];
    return &font->tiers[CA_FONT_STYLE_REGULAR];
}

/* Look up glyph data for a Unicode codepoint within a tier. */
static inline stbtt_packedchar *ca_font_glyph(Ca_FontTier *tier, uint32_t cp)
{
    for (int i = 0; i < CA_FONT_RANGE_COUNT; i++) {
        Ca_GlyphRange *r = &tier->ranges[i];
        if (cp >= (uint32_t)r->first_codepoint &&
            cp <  (uint32_t)(r->first_codepoint + r->num_chars))
            return &r->chardata[cp - r->first_codepoint];
    }
    return NULL;
}

/* Compute glyph quad from a packedchar.
   Baseline is snapped once for the whole line; per-glyph offsets
   are applied without re-rounding so all characters stay aligned. */
static inline void ca_font_get_quad(const stbtt_packedchar *pc,
                                     int atlas_w, int atlas_h,
                                     float *xpos, float *ypos,
                                     stbtt_aligned_quad *q)
{
    float ipw = 1.0f / (float)atlas_w;
    float iph = 1.0f / (float)atlas_h;
    float x = *xpos + pc->xoff;
    float y_base = (float)(int)(*ypos + 0.5f);
    q->x0 = x;
    q->y0 = y_base + pc->yoff;
    q->x1 = x + pc->xoff2 - pc->xoff;
    q->y1 = y_base + pc->yoff2;
    q->s0 = (float)pc->x0 * ipw;
    q->t0 = (float)pc->y0 * iph;
    q->s1 = (float)pc->x1 * ipw;
    q->t1 = (float)pc->y1 * iph;
    *xpos += pc->xadvance;
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
                    const char *regular_path, const char *bold_path,
                    float font_px);

/** Create a font atlas from in-memory font data.
    regular_data/regular_size: required — Ubuntu Nerd Font Regular (text + icons).
    bold_data/bold_size: optional (pass NULL/0 to skip bold tier). */
bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *regular_data, unsigned int regular_size,
                                const unsigned char *bold_data,    unsigned int bold_size,
                                float font_px);

/** Detect the platform's default proportional UI font.
    Returns true and writes the path into out_path on success. */
bool ca_font_detect_system(char *out_path, size_t max_len);

void ca_font_destroy(Ca_Instance *inst, Ca_Font *font);
