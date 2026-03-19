/* font.h — GPU font atlas built from a TrueType/OpenType file
   using stb_truetype.  Only the struct declaration and API are here;
   the implementation (STB_TRUETYPE_IMPLEMENTATION) lives in font.c. */
#pragma once

#include "ca_internal.h"
#include <stb_truetype.h>

#define CA_FONT_GLYPH_FIRST 32
#define CA_FONT_GLYPH_COUNT 224   /* printable ASCII + Latin-1 Supplement 32–255 */

typedef struct Ca_Font {
    stbtt_bakedchar glyphs[CA_FONT_GLYPH_COUNT];

    /* GPU resources */
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;

    /* Atlas dimensions (physical pixels) */
    int   atlas_w, atlas_h;

    /* Metrics in LOGICAL pixels */
    float ascent;        /* positive: height above baseline */
    float descent;       /* negative: depth below baseline  */
    float line_gap;

    /* HiDPI helpers */
    float content_scale; /* from glfwGetWindowContentScale (e.g. 2.0 on Retina) */
    float baked_px;      /* physical pixels actually baked  */
} Ca_Font;

/* Create a font atlas from a TTF/OTF file.
   font_px is the desired size in LOGICAL pixels.
   On HiDPI the atlas is baked at font_px * content_scale for crisp text.
   Returns true on success; font is zeroed on failure. */
bool ca_font_create(Ca_Instance *inst, GLFWwindow *glfw_win,
                    Ca_Font *out_font, const char *path, float font_px);

/* Destroy all GPU resources. Safe to call on a zeroed struct. */
void ca_font_destroy(Ca_Instance *inst, Ca_Font *font);
