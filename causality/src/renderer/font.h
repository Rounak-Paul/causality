/* font.h — GPU font atlas with multi-size tiers and Unicode support.
   Uses stb_truetype pack API for crisp text at every CSS font-size.
   Nerd Font icon codepoint ranges are baked alongside ASCII. */
#pragma once

#include "ca_internal.h"
#include <stb_truetype.h>
#include <math.h>

#define CA_FONT_RANGE_COUNT    6
#define CA_FONT_TIER_COUNT     9

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
    Ca_FontTier tiers[CA_FONT_TIER_COUNT];

    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;

    int   atlas_w, atlas_h;
    float content_scale;
    float default_size;
} Ca_Font;

/* Find the best packed tier for a desired logical pixel size. */
static inline Ca_FontTier *ca_font_tier(Ca_Font *font, float desired_px)
{
    Ca_FontTier *best = NULL;
    float best_diff = 1e20f;
    for (int i = 0; i < CA_FONT_TIER_COUNT; i++) {
        if (!font->tiers[i].packed) continue;
        float diff = fabsf(font->tiers[i].logical_px - desired_px);
        if (diff < best_diff) { best = &font->tiers[i]; best_diff = diff; }
    }
    return best ? best : &font->tiers[0];
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

/* Compute glyph quad from a packedchar (align_to_integer behaviour). */
static inline void ca_font_get_quad(const stbtt_packedchar *pc,
                                     int atlas_w, int atlas_h,
                                     float *xpos, float *ypos,
                                     stbtt_aligned_quad *q)
{
    float ipw = 1.0f / (float)atlas_w;
    float iph = 1.0f / (float)atlas_h;
    float x = (float)(int)(*xpos + pc->xoff + 0.5f);
    float y = (float)(int)(*ypos + pc->yoff + 0.5f);
    q->x0 = x;
    q->y0 = y;
    q->x1 = x + pc->xoff2 - pc->xoff;
    q->y1 = y + pc->yoff2 - pc->yoff;
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
                    Ca_Font *out_font, const char *path, float font_px);

bool ca_font_create_from_memory(Ca_Instance *inst, GLFWwindow *glfw_win,
                                Ca_Font *out_font,
                                const unsigned char *data, unsigned int data_size,
                                float font_px);

void ca_font_destroy(Ca_Instance *inst, Ca_Font *font);
