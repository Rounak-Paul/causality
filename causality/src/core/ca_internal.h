// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* ca_internal.h — internal struct definitions, never exposed publicly */
#pragma once

#include "causality.h"
#include "ca_gpu.h"
#include "causality_config.h"
#include "ca_pool.h"
#include "../ui/ca_resolved_style.h"
#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
#endif
#include <stdint.h>

/* All pool/limit constants are defined (with override hooks) in
   causality_config.h. They are intentionally not redefined here. */

typedef struct {
    VkCommandBuffer cmd;
    VkSemaphore     image_available;
    VkFence         in_flight;
    /* Per-frame storage buffer for instanced rendering */
    VkBuffer        instance_buf;
    VkDeviceMemory  instance_mem;
    void           *instance_mapped;  /* persistently mapped */
    VkDescriptorSet ssbo_set;
    VkDescriptorPool ssbo_pool;
    uint32_t        instance_capacity;
} Ca_Frame;

typedef struct {
    VkSwapchainKHR  swapchain;
    VkFormat        format;
    VkImageUsageFlags image_usage;
    VkExtent2D      extent;
    uint32_t        image_count;
    Ca_DynArray     image_storage;
    Ca_DynArray     image_view_storage;
    Ca_DynArray     image_render_finished_storage;
    VkImage        *images;
    VkImageView    *image_views;
    VkSemaphore    *image_render_finished;
    Ca_DynArray     viewport_wait_storage;
    Ca_DynArray     submit_wait_storage;
    Ca_DynArray     submit_stage_storage;
    Ca_Frame        frames[CA_FRAMES_IN_FLIGHT];
    uint32_t        current_frame;
} Ca_Swapchain;

/* ======================================================
   RENDERER — rect pipeline types
   ====================================================== */

typedef struct {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
} Ca_RectPipeline;

/* ======================================================
   RENDERER — text pipeline types
   ====================================================== */

typedef struct {
    VkPipeline            pipeline;
    VkPipelineLayout      layout;
    VkDescriptorSetLayout desc_layout;
    VkDescriptorPool      desc_pool;
    VkDescriptorSet       desc_set;
} Ca_TextPipeline;

/* ======================================================
   RENDERER — image types
   ====================================================== */

struct Ca_Image {
    VkImage          vk_image;
    VkDeviceMemory   memory;
    VkImageView      view;
    VkSampler        sampler;
    VkDescriptorSet  desc_set;     /* per-image descriptor set */
    VkDescriptorPool desc_pool;
    int              width, height;
    bool             in_use;
};

/* Must exactly match the push_constant block in the text vertex shader:
     vec2 pos      (offset  0)
     vec2 size     (offset  8)
     vec4 uv       (offset 16)  -- (s0, t0, s1, t1)
     vec4 color    (offset 32)
     vec2 viewport (offset 48)
   Total: 56 bytes.                                               */
typedef struct {
    float pos[2];
    float size[2];
    float uv[4];
    float color[4];
    float viewport[2];
} Ca_TextPushConst;

/* std430-padded text instance for SSBO. Keep stride equal to Ca_RectPushConst.
   Glyphs carry the same 2x2 paint transform as rects (translation folded
   into `pos`) so text inside a rotated panel rotates with it. */
typedef struct {
    float pos[2];
    float size[2];
    float uv[4];
    float color[4];
    float viewport[2];
    float xf_ab[2];             /* transform a, b — identity is (1, 0) */
    float xf_cd[2];             /* transform c, d — identity is (0, 1) */
    float _pad1[14];            /* image pipeline uses [0..3] for tl,tr,br,bl */
} Ca_TextInstance;

/* Forward-declare Ca_Font (full definition lives in renderer/font.h) */
typedef struct Ca_Font Ca_Font;

/* Draw modes for Ca_RectPushConst.draw_mode */
typedef enum {
    CA_DRAW_MODE_NORMAL      = 0,  /* solid fill + uniform border              */
    CA_DRAW_MODE_SHADOW      = 1,  /* SDF Gaussian shadow — blur via GPU       */
    CA_DRAW_MODE_LINEAR_GRAD = 2,  /* linear-gradient(angle, color, color2)    */
    CA_DRAW_MODE_RADIAL_GRAD = 3,  /* radial-gradient(circle, color, color2)   */
} Ca_DrawMode;

/* GPU-side instance data — must exactly match RectData in VERT_GLSL (std430).
   Offsets (bytes):
     pos[2]          offset   0  (8)
     size[2]         offset   8  (8)
     color[4]        offset  16  (16)
     viewport[2]     offset  32  (8)
     xf_ab[2]        offset  40  (8)  transform column 0 (a, b)
     corner_radii[4] offset  48  (16) tl, tr, br, bl
     border_color[4] offset  64  (16)
     color2[4]       offset  80  (16) gradient end / shadow tint
     border_width    offset  96  (4)
     blur_radius     offset 100  (4)
     draw_mode       offset 104  (4)  Ca_DrawMode
     gradient_angle  offset 108  (4)  degrees (linear) or unused (radial)
     gradient_cx     offset 112  (4)  radial center x (0..1)
     gradient_cy     offset 116  (4)  radial center y (0..1)
     xf_cd[2]        offset 120  (8)  transform column 1 (c, d)
   Total: 128 bytes

   The 2x2 transform occupies what used to be pure padding, so adding
   rotation cost no extra bytes and left the shared 128-byte instance
   stride (asserted below) untouched. Its translation is folded into `pos`
   on the CPU instead of needing two more floats, since the vertex shader
   already adds `pos` after expanding the quad corner.                     */
typedef struct {
    float    pos[2];
    float    size[2];
    float    color[4];
    float    viewport[2];
    float    xf_ab[2];          /* transform a, b — identity is (1, 0) */
    float    corner_radii[4];   /* tl, tr, br, bl */
    float    border_color[4];
    float    color2[4];
    float    border_width;
    float    blur_radius;
    uint32_t draw_mode;
    float    gradient_angle;
    float    gradient_cx;
    float    gradient_cy;
    float    xf_cd[2];          /* transform c, d — identity is (0, 1) */
} Ca_RectPushConst;

/* Fragment-stage push constant carrying the rounded clip rect shared by
   every instance in the current batch (see paint.c's ClipRect / clip_radius
   and the rect pipeline's ClipPC block in pipeline.c). Field order/size
   matches the shader's std430-equivalent push-constant layout exactly.
   edge_aa_scale is physical-px-per-logical-px (content_scale) for the
   window this batch belongs to: shape SDFs are evaluated in logical-pixel
   node space (see v_local in VERT_GLSL), so the shader needs this factor
   to size the anti-aliasing ramp as exactly one physical pixel regardless
   of DPI — see the comment above roundedBoxSDF's callers in pipeline.c. */
typedef struct {
    float pos[2];
    float size[2];
    float radius;
    float edge_aa_scale;
} Ca_ClipPushConst;

/* Instance buffer holds one fixed-stride slot per draw command for one frame.
   Rect and text/image records intentionally share a 128-byte slot size so
   all pipelines can bind the same storage buffer without dynamic offsets. */
#define CA_INSTANCE_SLOT_SIZE ((uint32_t)sizeof(Ca_RectPushConst))

/* ca_instance_pack_transform is defined just below Ca_DrawCmd, which it
   reads from. */

_Static_assert(sizeof(Ca_TextInstance) == sizeof(Ca_RectPushConst),
               "Causality instance SSBO records must use one fixed stride");

/* The GLSL struct declarations in pipeline.c hardcode these std430 offsets;
   a silent mismatch would render garbage rather than fail to build.

   These assertions only constrain the C side. The matching GLSL structs
   must be checked by hand, because std430 alignment does NOT follow C
   struct rules: a `vec4` (or an array of them) is 16-byte aligned, so
   declaring padding as `vec4 pad[3]` after a member ending at a non-16
   boundary silently inflates the GLSL struct and desynchronises the SSBO
   stride from this one. That exact mistake shipped once and dropped almost
   every glyph on screen while rects rendered fine. Always express trailing
   padding as `vec2`, and verify any change with:

     glslc -fshader-stage=compute layout.comp -o out.spv
     spirv-dis out.spv | grep ArrayStride     # must equal 128            */
_Static_assert(sizeof(Ca_RectPushConst) == 128,
               "RectData must stay 128 bytes to match VERT_GLSL");
_Static_assert(offsetof(Ca_RectPushConst, xf_ab) == 40,
               "RectData.xf_ab must sit at offset 40 to match VERT_GLSL");
_Static_assert(offsetof(Ca_RectPushConst, xf_cd) == 120,
               "RectData.xf_cd must sit at offset 120 to match VERT_GLSL");
_Static_assert(offsetof(Ca_TextInstance, viewport) == 48,
               "TextData.viewport must sit at offset 48 to match TEXT_VERT_GLSL");
_Static_assert(offsetof(Ca_TextInstance, xf_ab) == 56,
               "TextData.xf_ab must sit at offset 56 to match TEXT_VERT_GLSL");
_Static_assert(offsetof(Ca_TextInstance, xf_cd) == 64,
               "TextData.xf_cd must sit at offset 64 to match TEXT_VERT_GLSL");
_Static_assert(offsetof(Ca_TextInstance, _pad1) == 72,
               "ImageData.corner_01 must sit at offset 72 to match IMAGE_VERT_GLSL");

/* ======================================================
   EVENTS
   ====================================================== */

typedef struct {
    Ca_EventFn  fn;
    void       *user_data;
} Ca_EventHandler;

/* ======================================================
   UI — forward declarations
   ====================================================== */

typedef struct Ca_Node Ca_Node;

/* ======================================================
   UI — dirty flags, layout enums, node descriptor.
   (These are internal; users work with widgets instead.)
   ====================================================== */

typedef enum {
    CA_DIRTY_NONE     = 0,
    CA_DIRTY_CONTENT  = 1 << 0,
    CA_DIRTY_LAYOUT   = 1 << 1,
    CA_DIRTY_CHILDREN = 1 << 2,
} Ca_DirtyFlags;

typedef enum {
    CA_DIR_ROW    = 0,
    CA_DIR_COLUMN = 1,
} Ca_Direction;

typedef enum {
    CA_ALIGN_START   = 0,
    CA_ALIGN_CENTER  = 1,
    CA_ALIGN_END     = 2,
    CA_ALIGN_STRETCH = 3,
} Ca_Align;

/* Position mode values defined in causality.h:
   CA_POSITION_RELATIVE (0), CA_POSITION_ABSOLUTE (1), CA_POSITION_FIXED (2) */

typedef struct {
    float        width,  height;
    float        min_w,  min_h;
    float        max_w,  max_h;
    float        padding_top,    padding_right,
                 padding_bottom, padding_left;
    float        margin_top,     margin_right,
                 margin_bottom,  margin_left;
    /* Gap — row_gap / column_gap (gap is kept for backward-compat) */
    float        gap;
    float        row_gap, column_gap;
    Ca_Direction direction;
    Ca_Align     align_items;
    Ca_Align     align_self;    /* per-child cross-axis alignment; 0=auto */
    Ca_Align     align_content; /* multi-line cross-axis packing */
    Ca_Align     justify_content;
    uint32_t     background;
    /* Per-corner border radius (all four; uniform corner_radius for GPU) */
    float        border_radius_tl;
    float        border_radius_tr;
    float        border_radius_br;
    float        border_radius_bl;
    float        corner_radius;
    float        opacity;        /* 0 = not set (default 1.0) */
    uint8_t      corner_radii_set; /* CSS per-corner declarations: tl,tr,br,bl */
    /* Flex properties */
    float        flex_grow;
    float        flex_shrink;
    float        flex_basis;     /* 0 = auto */
    uint8_t      flex_wrap;      /* 0=nowrap, 1=wrap */
    int          flex_order;     /* CSS order property */
    /* Overflow clipping / scrolling */
    float        font_size;      /* 0 = use default baked size */
    float        line_height;    /* 0 = not set */
    float        letter_spacing; /* extra spacing between glyphs in px */
    float        word_spacing;   /* extra spacing between words in px */
    bool         font_bold;      /* true = use bold font tier */
    uint8_t      font_weight;    /* 0=normal, 1=bold, 2=lighter, 3=bolder */
    uint8_t      font_style;     /* 0=normal, 1=italic, 2=oblique */
    uint8_t      text_align;     /* 0=left (default), 1=center, 2=right */
    uint8_t      text_decoration; /* text-decoration keyword */
    uint8_t      text_transform;  /* text-transform keyword */
    uint8_t      white_space;     /* white-space keyword */
    uint8_t      overflow_x;     /* 0=visible, 1=hidden, 2=scroll, 3=auto */
    uint8_t      overflow_y;
    float        scrollbar_width;
    uint32_t     scrollbar_track_color;
    uint32_t     scrollbar_thumb_color;
    uint32_t     scrollbar_thumb_active_color;
    float        scrollbar_radius;
    bool         scrollbar_width_set;
    bool         scrollbar_track_color_set;
    bool         scrollbar_thumb_color_set;
    bool         scrollbar_thumb_active_color_set;
    bool         scrollbar_radius_set;
    bool         hidden;          /* display: none */
    bool         visibility_hidden; /* visibility: hidden */
    bool         disabled;        /* non-interactive, visually dimmed */
    bool         no_hover;        /* transparent to hover detection */
    /* Positioning */
    uint8_t      position;       /* Ca_Position: 0=relative, 1=absolute, 2=fixed */
    float        pos_x, pos_y;   /* used when position != relative */
    float        pos_right, pos_bottom;
    uint8_t      position_offsets; /* left=1, right=2, top=4, bottom=8 */
    uint8_t      position_percent; /* percentage bits matching position_offsets */
    /* Aspect ratio — width/height; 0 = not set */
    float        aspect_ratio;
    /* Box-sizing */
    uint8_t      box_sizing;     /* 0=content-box, 1=border-box */
    /* Border — uniform */
    float        border_width;
    uint32_t     border_color;
    /* Border — per-side */
    float        border_top_w,   border_right_w,   border_bottom_w,   border_left_w;
    uint32_t     border_top_c,   border_right_c,   border_bottom_c,   border_left_c;
    /* Outline — drawn outside the border, does not affect layout */
    float        outline_width;
    uint32_t     outline_color;
    float        outline_offset;
    /* Box shadow */
    float        shadow_offset_x, shadow_offset_y;
    float        shadow_blur;
    uint32_t     shadow_color;
    /* Backdrop filter — blur applied to pixels behind this node */
    float        backdrop_blur;
    /* Z-index */
    int16_t      z_index;
    /* Text wrapping */
    uint8_t      text_wrap;      /* 0=nowrap (default), 1=wrap */
    /* Percentage sizing (resolved during layout) */
    bool         width_pct;
    bool         height_pct;
    /* Interaction */
    uint8_t      cursor;         /* Ca_CssKeyword CURSOR_* */
    uint8_t      pointer_events; /* 0=auto, 1=none */
    uint8_t      user_select;    /* 0=auto, 1=none, 2=text, 3=all */
    /* Gradient background — 0 means no gradient (solid fill only) */
    uint8_t      gradient_type;  /* CA_DRAW_MODE_LINEAR_GRAD or CA_DRAW_MODE_RADIAL_GRAD */
    uint32_t     gradient_color2; /* end color stop (RRGGBBAA) */
    float        gradient_angle;  /* degrees (linear); 0 = top→bottom */
    float        gradient_cx;     /* radial center x (0..1) */
    float        gradient_cy;     /* radial center y (0..1) */
    /* 2D transform — purely visual, applied at paint time about the node's
       pivot. Layout (x/y/w/h) is unaffected, so a rotated node still
       occupies its axis-aligned box for sizing purposes; transforms
       compose down the subtree and are inverted for hit-testing.

       Scale is stored biased by -1 (0 = identity) and pivot is stored as an
       offset from center (0 = center) so that the `Ca_NodeDesc nd = {0}`
       initializer used at every construction site means "no transform"
       without every builder having to restate defaults. Use the
       ca_desc_scale_x/y and ca_desc_pivot_x/y accessors to read them. */
    float        rotation;        /* degrees clockwise; 0 = none */
    float        scale_bias_x, scale_bias_y;  /* actual scale minus 1 */
    float        pivot_off_x, pivot_off_y;    /* normalized pivot minus 0.5 */
} Ca_NodeDesc;

enum {
    CA_CORNER_RADIUS_TL_SET = 1u << 0,
    CA_CORNER_RADIUS_TR_SET = 1u << 1,
    CA_CORNER_RADIUS_BR_SET = 1u << 2,
    CA_CORNER_RADIUS_BL_SET = 1u << 3,
};

static inline float ca_desc_corner_tl(const Ca_NodeDesc *desc)
{
    return ((desc->corner_radii_set & CA_CORNER_RADIUS_TL_SET) != 0u ||
            desc->border_radius_tl > 0.0f)
        ? desc->border_radius_tl : desc->corner_radius;
}

static inline float ca_desc_corner_tr(const Ca_NodeDesc *desc)
{
    return ((desc->corner_radii_set & CA_CORNER_RADIUS_TR_SET) != 0u ||
            desc->border_radius_tr > 0.0f)
        ? desc->border_radius_tr : desc->corner_radius;
}

static inline float ca_desc_corner_br(const Ca_NodeDesc *desc)
{
    return ((desc->corner_radii_set & CA_CORNER_RADIUS_BR_SET) != 0u ||
            desc->border_radius_br > 0.0f)
        ? desc->border_radius_br : desc->corner_radius;
}

static inline float ca_desc_corner_bl(const Ca_NodeDesc *desc)
{
    return ((desc->corner_radii_set & CA_CORNER_RADIUS_BL_SET) != 0u ||
            desc->border_radius_bl > 0.0f)
        ? desc->border_radius_bl : desc->corner_radius;
}

static inline float ca_desc_scale_x(const Ca_NodeDesc *d) { return 1.0f + d->scale_bias_x; }
static inline float ca_desc_scale_y(const Ca_NodeDesc *d) { return 1.0f + d->scale_bias_y; }
static inline float ca_desc_pivot_x(const Ca_NodeDesc *d) { return 0.5f + d->pivot_off_x; }
static inline float ca_desc_pivot_y(const Ca_NodeDesc *d) { return 0.5f + d->pivot_off_y; }

static inline bool ca_desc_has_transform(const Ca_NodeDesc *d)
{
    return d->rotation != 0.0f ||
           d->scale_bias_x != 0.0f || d->scale_bias_y != 0.0f;
}

/* Accumulated 2D affine transform for a painted subtree, expressed as the
   matrix [ a c ; b d ] plus translation (tx, ty) in window pixels:

       screen_x = a * px + c * py + tx
       screen_y = b * px + d * py + ty

   Composed down the paint walk (ca_transform_mul) so a rotated panel
   rotates its descendants and their glyphs, and inverted at the single
   hit-test chokepoint so input follows the pixels. `active` is false for
   the identity, letting the untransformed common path skip all of it. */
typedef struct {
    float a, b, c, d;
    float tx, ty;
    bool  active;
} Ca_Transform2D;

static inline Ca_Transform2D ca_transform_identity(void)
{
    return (Ca_Transform2D){ .a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f,
                             .tx = 0.0f, .ty = 0.0f, .active = false };
}

/* Returns `outer` applied on top of `inner` (outer ∘ inner): a point is
   first mapped by `inner`, then by `outer`. */
static inline Ca_Transform2D ca_transform_mul(Ca_Transform2D outer,
                                              Ca_Transform2D inner)
{
    if (!outer.active) return inner;
    if (!inner.active) return outer;
    Ca_Transform2D r;
    r.a  = outer.a * inner.a + outer.c * inner.b;
    r.b  = outer.b * inner.a + outer.d * inner.b;
    r.c  = outer.a * inner.c + outer.c * inner.d;
    r.d  = outer.b * inner.c + outer.d * inner.d;
    r.tx = outer.a * inner.tx + outer.c * inner.ty + outer.tx;
    r.ty = outer.b * inner.tx + outer.d * inner.ty + outer.ty;
    r.active = true;
    return r;
}

/* Builds the transform for one node's rotation/scale about its pivot,
   where (px, py) is the node's top-left corner and (w, h) its size. */
Ca_Transform2D ca_transform_from_desc(const Ca_NodeDesc *desc,
                                      float px, float py, float w, float h);

/* Maps a point through a transform. */
static inline void ca_transform_apply(Ca_Transform2D t, float x, float y,
                                      float *out_x, float *out_y)
{
    if (!t.active) { *out_x = x; *out_y = y; return; }
    *out_x = t.a * x + t.c * y + t.tx;
    *out_y = t.b * x + t.d * y + t.ty;
}

/* Maps a point through a transform's inverse. Returns false when the
   transform is singular (a zero scale), in which case nothing is hit. */
bool ca_transform_apply_inverse(Ca_Transform2D t, float x, float y,
                                float *out_x, float *out_y);

/* ======================================================
   UI — constants (limits live in causality_config.h)
   ====================================================== */

/* Constants below are *not* user-tunable: they are tied to specific data
   structures inside the library and changing them requires code changes. */
#define CA_MENU_LABEL_MAX            64
#define CA_SELECT_MAX_VISIBLE         8   /* max options visible without scrolling */

typedef char Ca_OptionText[CA_OPTION_TEXT_MAX];

/* ======================================================
   UI — draw command (CPU-side, one per visible node or glyph)
   ====================================================== */

typedef enum {
    CA_DRAW_RECT           = 0,  /* solid colour rectangle               */
    CA_DRAW_GLYPH          = 1,  /* font glyph textured quad             */
    CA_DRAW_IMAGE          = 2,  /* user-loaded image textured quad      */
    CA_DRAW_VIEWPORT       = 3,  /* offscreen viewport textured quad     */
    CA_DRAW_BACKDROP_BLUR  = 4,  /* frosted-glass: blur what's behind    */
} Ca_DrawType;

typedef struct {
    Ca_DrawType type;
    float       x, y, w, h;
    float       r, g, b, a;
    /* Per-corner border radius (tl, tr, br, bl).
       corner_radius is the uniform fallback used when all four are equal. */
    float       corner_radius;
    float       corner_tl, corner_tr, corner_br, corner_bl;
    /* CA_DRAW_GLYPH: normalised UV coords in the font atlas */
    float       u0, v0, u1, v1;
    bool        in_use;
    bool        overlay;
    bool        has_clip;
    float       clip_x, clip_y, clip_w, clip_h;
    /* Corner radius of the clip rect itself (uniform). Zero means the
       clip behaves as a plain rectangle, as before. Set when the nearest
       overflow:hidden ancestor has a nonzero corner-radius, so content
       painted near a rounded panel's edge is masked to the same curve
       instead of poking past it into the panel's square bounding box. */
    float       clip_radius;
    /* Border — uniform */
    float       border_width;
    float       border_r, border_g, border_b, border_a;
    /* Box-shadow: blur uses SDF Gaussian approximation on the GPU */
    float       blur_radius;
    /* Gradient: second color stop and parameters */
    float       color2_r, color2_g, color2_b, color2_a;
    float       gradient_angle; /* degrees for linear; unused for radial */
    float       gradient_cx, gradient_cy; /* radial center 0..1 */
    /* Draw mode (normal / shadow / linear-gradient / radial-gradient) */
    Ca_DrawMode draw_mode;
    /* Backdrop blur radius in pixels (CA_DRAW_BACKDROP_BLUR only) */
    float       backdrop_blur_radius;
    /* Z-index for draw order sorting */
    int16_t     z_index;
    uint32_t    image_index;
    uint32_t    viewport_index;
    int16_t     font_page_index;
    /* Paint-time 2D transform inherited from this command's node and every
       transformed ancestor. Applied in the vertex shader about (origin_x,
       origin_y) in window pixels. Identity when `xf_active` is false, which
       is the common case and costs nothing on the GPU either way. */
    bool        xf_active;
    float       xf_a, xf_b, xf_c, xf_d;
    float       xf_tx, xf_ty;
} Ca_DrawCmd;

/* Writes one draw command's paint transform into an instance record, shared
   by every pipeline so the CPU-side convention stays in one place.

   The quad's own origin (qx, qy) is mapped through the transform and stored
   in `pos`, while only the 2x2 part reaches the shader — folding translation
   here is what let the transform fit in the pre-existing padding without
   growing the 128-byte instance stride. */
static inline void ca_instance_pack_transform(const Ca_DrawCmd *cmd,
                                              float qx, float qy,
                                              float pos[2], float xf_ab[2],
                                              float xf_cd[2])
{
    if (!cmd->xf_active) {
        pos[0] = qx;      pos[1] = qy;
        xf_ab[0] = 1.0f;  xf_ab[1] = 0.0f;
        xf_cd[0] = 0.0f;  xf_cd[1] = 1.0f;
        return;
    }
    pos[0] = cmd->xf_a * qx + cmd->xf_c * qy + cmd->xf_tx;
    pos[1] = cmd->xf_b * qx + cmd->xf_d * qy + cmd->xf_ty;
    xf_ab[0] = cmd->xf_a;  xf_ab[1] = cmd->xf_b;
    xf_cd[0] = cmd->xf_c;  xf_cd[1] = cmd->xf_d;
}

/* ======================================================
   UI — node type
   ====================================================== */

typedef enum {
    CA_NODE_BOX = 0,
} Ca_NodeType;

typedef enum {
    CA_WIDGET_NONE       = 0,
    CA_WIDGET_LABEL      = 1,
    CA_WIDGET_BUTTON     = 2,
    CA_WIDGET_TEXT_INPUT = 3,
    CA_WIDGET_CHECKBOX   = 4,
    CA_WIDGET_RADIO      = 5,
    CA_WIDGET_SLIDER     = 6,
    CA_WIDGET_TOGGLE     = 7,
    CA_WIDGET_PROGRESS   = 8,
    CA_WIDGET_SELECT     = 9,
    CA_WIDGET_TABBAR     = 10,
    CA_WIDGET_TREENODE   = 11,
    CA_WIDGET_TABLE      = 12,
    CA_WIDGET_SPLITTER   = 13,
    CA_WIDGET_IMAGE      = 14,
    CA_WIDGET_VIEWPORT   = 15,
    CA_WIDGET_MODAL      = 16,
    CA_WIDGET_MENUBAR    = 17,
} Ca_WidgetType;

/* ======================================================
   UI — node transition (for CSS transition property)
   ====================================================== */

/* Interpolation curve applied across a transition's duration. Values match
   the CSS transition-timing-function keywords this maps from. */
typedef enum {
    CA_EASING_LINEAR = 0,
    CA_EASING_EASE_IN,
    CA_EASING_EASE_OUT,
    CA_EASING_EASE_IN_OUT,
} Ca_Easing;

typedef struct {
    uint8_t  prop;           /* Ca_CssPropId being animated */
    bool     active;
    float    from_f;         /* start value (float) or unpacked from color */
    float    to_f;           /* target value (float) */
    uint32_t from_color;     /* start RGBA (for color props) */
    uint32_t to_color;       /* target RGBA */
    double   start_time;     /* glfwGetTime() when transition began */
    float    duration;       /* seconds */
    Ca_Easing easing;
} Ca_Transition;

/* ======================================================
   UI — Node (full definition)
   ====================================================== */

#define CA_NODE_CLASS_MAX 128
#define CA_NODE_ID_MAX     64

struct Ca_Node {
    Ca_NodeType   type;
    uint8_t       dirty;           /* Ca_DirtyFlags bits              */
    bool          in_use;
    Ca_NodeDesc   desc;
    /* Pre-CSS sparse desc — snapshotted at the START of apply_css, after
       widget-specific post-claim_child mutations (hidden, disabled, position,
       etc.) have been written.  ca_widget_reapply_css resets desc = base_desc
       then re-resolves CSS so :hover / :focus / :active rules take effect on
       builder-pattern panels without destroying the subtree.
       Valid only when has_base_desc is true (apply_css ran). */
    Ca_NodeDesc   base_desc;
    bool          has_base_desc;
    float         x, y, w, h;     /* computed by layout pass         */
    Ca_Window    *window;
    Ca_Node      *parent;
    Ca_Node     **children;        /* heap-allocated, grown on demand */
    uint32_t      child_capacity;  /* allocated slots in children[]   */
    uint32_t      child_count;
    int32_t       draw_cmd_idx;    /* -1 = no slot assigned           */
    uint8_t       widget_type;     /* Ca_WidgetType — for unified per-node paint */
    void         *widget;          /* back-pointer to widget struct (Ca_Label* etc.) */
    /* Paint cache — per-node incremental rendering */
    uint32_t      cache_start;     /* index into win->paint_cache (pre-children cmds) */
    uint32_t      cache_count;
    uint32_t      cache_post_start; /* post-children cmds (scrollbars) */
    uint32_t      cache_post_count;
    /* CSS integration */
    uint8_t       elem_type;       /* Ca_ElementType from style.h     */
    char          classes[CA_NODE_CLASS_MAX]; /* space-separated CSS classes */
    char          id[CA_NODE_ID_MAX];         /* CSS id (without #)          */
    Ca_Stylesheet *scoped_stylesheet;
    /* Per-node resolved-style cache (apply_css in style.c). Skips the full
       O(rules) selector-match scan when this node's classes/pseudo-state
       fingerprint is unchanged from last call AND the stylesheet has no
       position-dependent selector targeting these classes (see
       ca_style_node_is_cacheable and the classification notes in css.h).
       Was the dominant cost in panels with many rows (e.g. a Hierarchy
       tree with hundreds of entities) rebuilt every frame — each row paid
       for a full stylesheet scan twice (container + header) even when
       nothing about its style inputs had changed. */
    Ca_ResolvedStyle style_cache;
    char          style_cache_classes[CA_NODE_CLASS_MAX];
    bool          style_cache_valid;
    Ca_Stylesheet *style_cache_scoped_stylesheet;
    bool          style_cache_hover, style_cache_active, style_cache_focus,
                  style_cache_focus_within, style_cache_disabled;
    /* Scroll state (for overflow: scroll) */
    float         scroll_x, scroll_y;
    float         content_w, content_h; /* natural content size        */
    bool          scrollbar_x_visible;
    bool          scrollbar_y_visible;
    /* Lazily created by ca_get_scroll_y_signal() the first time a caller
       asks to observe this node's scroll position reactively. Mirrored
       (via ca_signal_set_float, a no-op when unchanged) at every scroll_y
       mutation site — mouse wheel, scrollbar drag, ca_set_scroll_y,
       ca_scroll_to_top/bottom — so a ca_div_set_builder can depend on
       scroll position the same way it depends on any other signal,
       instead of polling ca_get_scroll_y every frame. */
    Ca_Signal    *scroll_y_signal;
    /* Transition animations */
    Ca_DynArray   transition_storage;
    Ca_Transition *transitions;
    float         transition_duration;   /* default duration from CSS (sec) */
    uint64_t      transition_props;      /* bitmask of props that should animate */
    Ca_Easing     transition_easing;     /* default easing from CSS, applies
                                             to every property this node
                                             transitions (one shorthand per
                                             node, matching transition_duration) */
    /* Drag callbacks (user-level drag interaction) */
    void         *drag_fn_start;    /* Ca_DragFn */
    void         *drag_fn_move;     /* Ca_DragFn */
    void         *drag_fn_end;      /* Ca_DragFn */
    void         *drag_data;        /* user_data for drag callbacks */
    uint8_t       tree_drop_indicator;
    uint32_t      tree_drop_color;
    /* Scroll callback */
    void         *scroll_fn;        /* Ca_ScrollFn */
    void         *scroll_data;      /* user_data for scroll callback */
    /* Reactive builder — ca_div_set_builder. The effect re-runs the
       builder whenever any signal it read via ca_signal_get changes. */
    void          (*builder_fn)(Ca_Div *div, void *user_data);
    void           *builder_data;
    struct Ca_Effect *builder_effect;
    /* Debug overlay — set true during paint when node was dirty (paint-flash) */
    bool          dbg_repainted;
};

/*
 * Return true if the node or any ancestor in the tree has hidden=true.
 *
 * Used to suppress dropdown overlays whose host widget is inside a panel
 * hidden via ca_set_hidden() — the select node is not individually hidden,
 * only its parent panel is, so checking node->desc.hidden alone misses it.
 *
 * n        Node at which to begin the ancestor walk.
 * Returns  true if any node on the path to root is hidden.
 */
static inline bool node_is_ancestor_hidden(const Ca_Node *n)
{
    while (n) {
        if (n->desc.hidden) return true;
        n = n->parent;
    }
    return false;
}

/* ======================================================
   UI — Widget structs (full definitions; opaque in public header)
   ====================================================== */

struct Ca_Label {
    Ca_Node  *node;
    char      text[CA_LABEL_TEXT_MAX];
    char     *dyn_text;  /* heap-allocated when text > CA_LABEL_TEXT_MAX */
    uint32_t  color;     /* packed RGBA foreground colour */
    bool      in_use;
};

/*
 * Return the active text pointer for a label, preferring heap-allocated
 * dynamic text when present.
 *
 * lbl      Label to query.
 * Returns  Pointer to the label's current text string.
 */
static inline const char *ca_label_get_text(const Ca_Label *lbl)
{ return lbl->dyn_text ? lbl->dyn_text : lbl->text; }

struct Ca_Button {
    Ca_Node    *node;
    char        text[CA_BUTTON_TEXT_MAX];
    uint32_t    text_color;  /* packed RGBA; 0 → white default */
    Ca_ClickFn  on_click;
    void       *click_data;
    /* Pixel offset of the most recent click relative to the button's
       top-left. Populated immediately before `on_click` is fired so
       handlers can do hit-testing (e.g. column under the cursor). */
    float       last_click_x;
    float       last_click_y;
    bool        last_click_valid;
    bool        keyboard_focusable;
    bool        in_use;
};

struct Ca_TextInput {
    Ca_Node    *node;
    char        text[CA_INPUT_TEXT_MAX];
    uint32_t    text_color;
    uint32_t    placeholder_color;
    char        placeholder[CA_INPUT_TEXT_MAX];
    int         cursor;       /* byte offset into text */
    int         sel_start;    /* selection anchor (-1 = no selection) */
    Ca_ChangeFn on_change;
    void       *change_data;
    Ca_InputMode input_mode;
    float       drag_speed;
    bool        in_use;
};

/* ---- New widgets ---- */

struct Ca_Checkbox {
    Ca_Node      *node;
    char          text[CA_LABEL_TEXT_MAX];
    uint32_t      text_color;
    bool          checked;
    Ca_CheckFn    on_change;
    void         *change_data;
    bool          in_use;
};

struct Ca_Radio {
    Ca_Node      *node;
    char          text[CA_LABEL_TEXT_MAX];
    uint32_t      text_color;
    int           group;
    int           value;
    Ca_CheckFn    on_change;
    void         *change_data;
    bool          in_use;
};

struct Ca_Slider {
    Ca_Node      *node;
    float         min_val, max_val, value;
    Ca_SliderFn   on_change;
    void         *change_data;
    bool          in_use;
};

struct Ca_Toggle {
    Ca_Node      *node;
    bool          on;
    Ca_ToggleFn   on_change;
    void         *change_data;
    bool          in_use;
};

struct Ca_Progress {
    Ca_Node      *node;
    float         value;        /* 0.0 – 1.0 */
    uint32_t      bar_color;
    bool          in_use;
};

struct Ca_Select {
    Ca_Node      *node;
    Ca_DynArray   option_storage;
    Ca_OptionText *options;
    int           option_count;
    int           selected;
    bool          open;
    int           scroll_offset;  /* index of first visible option */
    float         scroll_accum;   /* sub-integer scroll accumulator  */
    int           hover_item;     /* index of dropdown item under cursor, -1 = none */
    Ca_SelectFn   on_change;
    void         *change_data;
    Ca_SelectFn   on_hover;       /* fired each time hover_item changes while open */
    void         *hover_data;
    bool          in_use;
};

struct Ca_TabBar {
    Ca_Node      *node;
    Ca_DynArray   tab_node_storage;
    Ca_Node     **tab_nodes;
    Ca_DynArray   label_storage;
    Ca_OptionText *labels;
    int           count;
    int           active;
    Ca_TabFn      on_change;
    void         *change_data;
    bool          in_use;
    uint32_t      active_bg;
    uint32_t      inactive_bg;
    uint32_t      active_text;
    uint32_t      inactive_text;
};

struct Ca_TreeNode {
    Ca_Node      *node;
    char          text[CA_LABEL_TEXT_MAX];
    uint32_t      text_color;
    bool          expanded;
    int           depth;
    Ca_TreeToggleFn on_toggle;
    void         *toggle_data;
    bool          in_use;
    bool          is_leaf;
    char          icon[8];
    uint32_t      icon_color;
};

struct Ca_Table {
    Ca_Node      *node;
    int           column_count;
    Ca_DynArray   column_width_storage;
    float        *column_widths;
    bool          in_use;
};

struct Ca_Tooltip {
    Ca_Node      *node;       /* the target element */
    char          text[CA_LABEL_TEXT_MAX];
    char          style[CA_NODE_CLASS_MAX]; /* optional CSS class string */
    float         font_size;  /* resolved from CSS; 0 = use default */
    bool          in_use;
};

struct Ca_CtxMenu {
    Ca_Node      *node;       /* the target element */
    Ca_DynArray   item_storage;
    Ca_OptionText *items;
    int           item_count;
    bool          open;
    int           hover_index;
    float         open_x, open_y;
    Ca_MenuFn     on_select;
    void         *select_data;
    Ca_ContextMenuOpenFn on_open;
    void         *open_data;
    bool          in_use;
};

struct Ca_Modal {
    Ca_Node      *node;       /* modal container node */
    bool          visible;
    uint32_t      overlay_color;
    bool          in_use;
};

struct Ca_Splitter {
    Ca_Node      *node;       /* the splitter container node */
    int           direction;  /* CA_HORIZONTAL or CA_VERTICAL */
    float         ratio;      /* 0.0–1.0: fraction of space for first child */
    float         min_ratio;  /* minimum ratio (default 0.1) */
    float         max_ratio;  /* maximum ratio (default 0.9) */
    float         bar_size;   /* divider thickness in px */
    uint32_t      bar_color;
    uint32_t      bar_hover_color;
    bool          dragging;   /* true while user drags the divider */
    bool          in_use;
    /* User callback fired when the user drags the divider and the
       ratio changes. Allows the application to persist the new ratio
       across rebuilds (since the splitter widget is destroyed and
       recreated when its containing div re-runs its builder). */
    void        (*on_resize)(float ratio, void *user_data);
    void         *user_data;
};

/* Per-frame-in-flight GPU resources for one Ca_Viewport slot. Duplicated
   CA_FRAMES_IN_FLIGHT times so the CPU can start recording slot fi+1's
   command buffer without waiting for the GPU to finish slot fi's work —
   mirrors Ca_Swapchain's Ca_Frame (swapchain.c). */
typedef struct {
    VkImage              color_image;
    VkDeviceMemory       color_memory;
    VkImageView          color_view;
    VkDescriptorSet      desc_set;      /* per-slot descriptor for compositing */
    VkDescriptorPool     desc_pool;
    VkCommandBuffer      cmd;           /* allocated from inst->cmd_pool */
    VkFence              render_fence;
    /* Signalled by the render submit; the swapchain's compositing submit
       waits on this at the GPU level instead of the CPU blocking on
       render_fence, so viewport rendering and swapchain command-buffer
       recording/submission can overlap. */
    VkSemaphore          render_done;
    /* True once THIS slot's color_image has actually completed at least one
       render since it (or its GPU resources) were last created — a freshly
       created/resized image starts UNDEFINED and only becomes safe to
       sample after ca_viewport_render_all's first real pass for this slot;
       compositing must not bind/sample it before then. */
    bool                 has_rendered_once;
} Ca_ViewportFrame;

struct Ca_Viewport {
    Ca_Node             *node;
    Ca_Instance         *instance;
    /* GPU resources duplicated per frame-in-flight slot — see
       Ca_ViewportFrame. Sampler is stateless (no per-frame state to race on)
       so it is NOT duplicated, same as Ca_Swapchain's samplers. */
    Ca_ViewportFrame      frame[CA_FRAMES_IN_FLIGHT];
    VkSampler            sampler;
    VkFormat             format;
    uint32_t             width, height; /* current pixel dimensions */
    /* Callbacks */
    Ca_ViewportRenderFn  on_render;
    void                *render_data;
    Ca_ViewportResizeFn  on_resize;
    void                *resize_data;
    VkClearColorValue    clear_color;
    bool                 in_use;
    bool                 needs_redraw;
    /* Frame-in-flight slot this viewport's NEXT render will use, cycling
       0..CA_FRAMES_IN_FLIGHT-1 (mirrors Ca_Swapchain.current_frame). */
    uint32_t             frame_index;
    /* Slot that was actually submitted on the most recent
       ca_viewport_render_all call — i.e. frame[last_rendered_frame] holds
       the freshest completed-or-in-flight render. Distinct from frame_index
       (which already points at the NEXT slot to use by the time a caller
       outside ca_viewport_render_all could observe it): the swapchain
       compositor needs "what did I just render", not "what will I render
       next", and back-deriving that via (frame_index - 1) mod N at the
       compositor call site would be an easy off-by-one to get wrong. */
    uint32_t             last_rendered_frame;
};

/* Internal: one entry inside a sub-menu (one level of nesting only) */
typedef struct {
    char             label[CA_MENU_LABEL_MAX];
    Ca_MenuActionFn  action;
    void            *action_data;
} Ca_MenuBarSubItem;

typedef struct {
    char                label[CA_MENU_LABEL_MAX];
    Ca_MenuActionFn     action;
    void               *action_data;
    bool                separator;                           /* render as divider line       */
    Ca_DynArray          sub_item_storage;
    Ca_MenuBarSubItem   *sub_items;
    int                 sub_item_count;
} Ca_MenuBarItem;

typedef struct {
    char            label[CA_MENU_LABEL_MAX];
    Ca_DynArray      item_storage;
    Ca_MenuBarItem  *items;
    int             item_count;
    Ca_Node        *header_node;
    int             active_sub;   /* index of item with open sub-menu, -1=none */
} Ca_MenuBarMenu;

struct Ca_MenuBar {
    Ca_Node          *node;
    Ca_DynArray       menu_storage;
    Ca_MenuBarMenu   *menus;
    int               menu_count;
    int               active_menu;   /* -1 = no dropdown open */
    int               hover_item;
    int               hover_sub_item;
    bool              in_use;
    uint32_t          header_highlight;
    uint32_t          dropdown_bg;
    uint32_t          dropdown_border;
    uint32_t          dropdown_hover;
    uint32_t          dropdown_text;
    uint32_t          text_color;
    float             item_font_size; /* dropdown item font size (derived from CSS) */
};

void ca_menubar_dropdown_geometry(Ca_Window *win,
                                  const Ca_Node *header,
                                  float menu_w,
                                  float menu_h,
                                  float *out_x,
                                  float *out_y);
void ca_menubar_submenu_geometry(Ca_Window *win,
                                 float drop_x,
                                 float drop_y,
                                 float menu_w,
                                 float sub_w,
                                 float sub_h,
                                 float parent_offset_y,
                                 float *out_x,
                                 float *out_y);

/* ======================================================
   WINDOW
   ====================================================== */

struct Ca_Window {
    GLFWwindow   *glfw;
    Ca_Instance  *instance;
    VkSurfaceKHR  surface;
    Ca_Swapchain  sc;
    bool          in_use;

    /* UI node tree */
    Ca_Pool       node_pool;
    Ca_Node      *root;
    Ca_DynArray   draw_cmd_storage;
    Ca_DrawCmd   *draw_cmds;
    uint32_t      draw_cmd_count;
    /* Pre-allocated z-sort index (avoids per-frame malloc) */
    Ca_DynArray   sorted_index_storage;
    uint32_t     *sorted_idx;

    /* Backdrop blur — per-window offscreen image holding a blurred snapshot
       of the background.  Created lazily when the first backdrop-blur node
       is painted; destroyed on window shutdown or swapchain recreation.
       The snapshot is captured from the current swapchain image before the
       UI render pass begins (after any bg_render_fn has executed). */
    /* blur_image: result of applying Gaussian blur to the captured background.
       blur_temp:  intermediate image for the horizontal blur pass.
       Both live at swapchain resolution; recreated on resize.              */
    VkImage          blur_image;
    VkDeviceMemory   blur_memory;
    VkImageView      blur_view;
    VkSampler        blur_sampler;
    VkDescriptorSet  blur_desc_set;   /* combined-image-sampler for the blur tex */
    VkDescriptorPool blur_desc_pool;

    VkImage          blur_temp;       /* horizontal-pass intermediate */
    VkDeviceMemory   blur_temp_memory;
    VkImageView      blur_temp_view;
    VkDescriptorSet  blur_temp_desc_set;
    VkDescriptorPool blur_temp_desc_pool;

    uint32_t         blur_image_w;
    uint32_t         blur_image_h;
    bool             blur_image_valid; /* true once the snapshot is up to date */
    /* Incremental paint cache — mirrors draw_cmds for per-node caching */
    Ca_DynArray   paint_cache_storage;
    Ca_DrawCmd   *paint_cache;
    uint32_t      paint_cache_used;
    /* Reusable flex-layout scratch.  One logical slot holds the seven
       per-child float arrays used by layout.c. */
    float         *layout_scratch;
    uint32_t       layout_scratch_capacity;
    uint32_t       layout_scratch_used;
    Ca_DynArray    layout_scratch_storage;
    Ca_DynArray    geometry_snapshot;
    Ca_DynArray    paint_cache_spans;

    /* Widget pools */
    Ca_Pool       label_pool;
    Ca_Pool       button_pool;
    Ca_Pool       input_pool;
    Ca_Pool       checkbox_pool;
    Ca_Pool       radio_pool;
    Ca_Pool       slider_pool;
    Ca_Pool       toggle_pool;
    Ca_Pool       progress_pool;
    Ca_Pool       select_pool;
    Ca_Pool       tabbar_pool;
    Ca_Pool       treenode_pool;
    Ca_Pool       table_pool;
    Ca_Pool       tooltip_pool;
    Ca_Pool       ctxmenu_pool;
    Ca_Pool       modal_pool;
    Ca_Pool       splitter_pool;
    Ca_Pool       viewport_pool;
    Ca_Pool       menubar_pool;

    /* Hover / drag state for interactive widgets */
    Ca_Node      *hovered_node;
    Ca_Node      *drag_node;
    float         drag_start_x;
    float         drag_start_value;

    /* Numeric text-input drag state */
    Ca_TextInput *numeric_drag_input;
    bool          numeric_drag_active;
    float         numeric_drag_start_x;
    double        numeric_drag_start_value;

    /* Generic drag interaction state */
    Ca_Node      *user_drag_node;      /* node being dragged by user drag callbacks */
    float         user_drag_start_x;   /* mouse x when drag began */
    float         user_drag_start_y;   /* mouse y when drag began */
    bool          user_drag_active;    /* true after drag threshold exceeded */

    /* Scrollbar drag state */
    Ca_Node      *scrollbar_drag_node; /* node whose scrollbar is being dragged */
    bool          scrollbar_drag_y;    /* true = Y-axis, false = X-axis */
    float         scrollbar_drag_grab; /* offset from thumb origin to mouse at drag start */

    /* Cached copy of the instance-wide UI scale for fast per-window rendering.
       Do not treat this as independently configurable per window. */
    float         ui_scale;
    /* When non-zero, the static content tree needs to be rescaled by this
       ratio at the start of the next frame (deferred to avoid mid-drag corruption). */
    float         pending_scale_ratio;

    /* Input state (updated by GLFW callbacks each tick) */
    double        mouse_x, mouse_y;
    bool          mouse_buttons[3];       /* [0]=left [1]=right [2]=middle */
    bool          prev_mouse_right;       /* right-button edge detection for context menus (per-window, not shared) */
    bool          mouse_click_this_frame; /* cleared at top of each tick   */
    double        scroll_dx, scroll_dy;   /* accumulated scroll this frame */
    bool          scroll_this_frame;

    /* Keyboard / focus state */
    Ca_Node      *focused_node;           /* NULL = nothing focused */
    Ca_DynArray   char_storage;
    uint32_t     *char_buf;               /* Unicode codepoints this frame */
    uint32_t      char_count;
    Ca_DynArray   key_storage;
    Ca_DynArray   key_action_storage;
    Ca_DynArray   key_mods_storage;
    int          *key_buf;                /* GLFW key codes this frame */
    int          *key_action_buf;
    int          *key_mods_buf;
    uint32_t      key_count;
    bool          key_consumed[CA_KEY_MENU + 1]; /* indexed by Ca_Key */

    /* Render gating: set by ui.c when draw list changes, cleared after submit */
    bool          needs_render;

    /* Debug overlay (toggled by F9) */
    bool          debug_overlay;
    bool          dbg_force_repaint; /* one-shot: force paint pass on F9 toggle */
    uint32_t      dbg_frames_rendered;
    uint32_t      dbg_draw_cmds;
    uint32_t      dbg_rect_instances;
    uint32_t      dbg_ti_instances;
    uint32_t      dbg_batches;
    uint32_t      dbg_node_count;
    uint32_t      dbg_layout_count;    /* cumulative layout passes */
    uint32_t      dbg_dirty_count;     /* nodes dirty this paint pass */
    uint32_t      dbg_transition_count; /* active transitions this tick */

    /* Frame timing (updated in swapchain_frame) */
    double        dbg_frame_time_ms;   /* last frame GPU+present time in ms */
    double        dbg_fps;             /* smoothed frames per second */
    double        dbg_fps_accum;       /* accumulator for FPS calculation */
    uint32_t      dbg_fps_frames;      /* frames counted in current second */
    double        dbg_fps_last_time;   /* last second boundary */

    /* Per-frame user callback (fires after input pass, before paint) */
    void        (*on_frame_fn)(void *user_data);
    void         *on_frame_data;

    /* Background render callback — called before the UI render pass each frame.
       The swapchain is presented with LOAD_OP_LOAD when this is set. */
    Ca_BgRenderFn bg_render_fn;
    void         *bg_render_data;

    /* Custom title bar */
    Ca_Node       *title_bar_node;        /* system-managed title bar container  */
    Ca_Node       *content_root;          /* user content is built here          */
    Ca_Node       *status_bar_node;       /* system-managed status bar container */
    char           title[256];            /* window title text                   */
    bool           titlebar_needs_rebuild;
    bool           statusbar_needs_rebuild;
    /* Status bar user builder + config (NULL fn → bar hidden). */
    void         (*status_bar_fn)(Ca_Window *window, void *user_data);
    void          *status_bar_data;
    float          status_bar_height;     /* pre-scaled: raw_height * ui_scale */
    float          status_bar_raw_height; /* logical height (unscaled) */
    bool           titlebar_maximized;
    Ca_DynArray    titlebar_menu_storage;
    Ca_MenuBarMenu *titlebar_menus;
    int            titlebar_menu_count;
    int            titlebar_restore_x;
    int            titlebar_restore_y;
    int            titlebar_restore_w;
    int            titlebar_restore_h;
    /* Title-bar drag-to-move state (manual implementation) */
    int            titlebar_drag_win_x;    /* window pos at drag start  */
    int            titlebar_drag_win_y;
    double         titlebar_drag_screen_x; /* screen-space cursor at drag start */
    double         titlebar_drag_screen_y;
    bool           titlebar_drag_active;
    bool           titlebar_mouse_down;

    /* Edge / corner resize state (manual implementation for undecorated windows) */
    bool           resize_active;          /* currently resizing                 */
    int            resize_edge;            /* bitmask: 1=left 2=right 4=top 8=bottom */
    int            resize_start_win_x;     /* window pos at drag start            */
    int            resize_start_win_y;
    int            resize_start_win_w;     /* window size at drag start           */
    int            resize_start_win_h;
    double         resize_start_cursor_sx; /* screen-space cursor at drag start   */
    double         resize_start_cursor_sy;
    int            resize_target_x;        /* geometry computed during drag,      */
    int            resize_target_y;        /* applied once on release (avoids      */
    int            resize_target_w;        /* setFrame mid-drag, which cancels     */
    int            resize_target_h;        /* the borderless mouse-tracking session */

    /* Deferred swapchain resize — set by GLFW callbacks, applied at the start
       of the next ca_renderer_frame after in-flight GPU work has completed.
       Avoids calling vkDeviceWaitIdle from inside glfwPollEvents callbacks. */
    bool           pending_swapchain_resize;
    int            pending_sc_w;
    int            pending_sc_h;

    /* Close callback — invoked by ca_window_destroy before the window's
       resources are freed.  Use to null out any pointers held by the caller. */
    void         (*on_close)(Ca_Window *window, void *user_data);
    void          *on_close_data;
};

/* ======================================================
   INSTANCE
   ====================================================== */

#define CA_POPUP_TITLE_MAX 128
#define CA_POPUP_TEXT_MAX 1024

typedef struct Ca_PopupEntry {
    char             title[CA_POPUP_TITLE_MAX];
    char             message[CA_POPUP_TEXT_MAX];
    Ca_PopupButtons  buttons;
    Ca_PopupResultFn on_result;
    void            *result_data;
} Ca_PopupEntry;

struct Ca_Instance {
    Ca_Pool windows;

    /* Popup manager (reserved-window control) */
    Ca_PopupEntry popup_current;
    Ca_DynArray  popup_queue;
    bool        popup_active;
    Ca_Window  *popup_window;
    Ca_PopupResult popup_pending_result;

    /* Vulkan */
    VkInstance               vk_instance;
#ifdef CAUSALITY_VULKAN_VALIDATION
    VkDebugUtilsMessengerEXT vk_debug_messenger;
#endif
    VkPhysicalDevice         vk_gpu;
    VkDevice                 vk_device;
    VkQueue                  gfx_queue;
    VkQueue                  present_queue;
    uint32_t                 gfx_family;
    uint32_t                 present_family;
    VkCommandPool            cmd_pool;
    VmaAllocator              vma;

    /* GPU info (populated at init for debug overlay) */
    char                     gpu_name[256];
    uint32_t                 gpu_type;          /* VkPhysicalDeviceType */
    uint32_t                 vk_api_version;    /* packed Vulkan version */
    uint32_t                 driver_version;
    uint32_t                 vendor_id;
    uint64_t                 gpu_heap_total;    /* total device-local heap bytes */
    uint32_t                 gpu_heap_count;
    VkPresentModeKHR         present_mode;      /* current present mode */
    bool                     draw_indirect_count; /* Vulkan 1.2 drawIndirectCount enabled */
    bool                     descriptor_indexing_supported; /* Vulkan 1.2 bindless features enabled — see ca_gpu_bindless_supported */
    bool                     disable_vsync;     /* from Ca_InstanceDesc — see choose_present_mode() */

    /* External renderer's device-resource predestroy hook — see
       ca_gpu_set_predestroy_callback in ca_gpu.h. */
    void (*gpu_predestroy_fn)(void *user_data);
    void  *gpu_predestroy_data;

    /* Double-buffered event queues: posting may continue during dispatch. */
    Ca_DynArray      event_queue;
    Ca_DynArray      event_dispatch_queue;
    Ca_Mutex        *event_mutex;
    Ca_EventHandler  handlers[CA_EVENT_TYPE_COUNT];

    /* Font config (copied from Ca_InstanceDesc, used on first window init) */
    char  font_path[512];
    char  bold_font_path[512];

    /* Compiled-SPIR-V cache directory (see renderer/shader_cache.h).
       Empty string means lookups always miss — every ca_shader_compile
       call falls back to compiling via shaderc, exactly as if no
       shader_cache_dir had ever existed. Set once by
       ca_shader_cache_init_dir during ca_instance_create, before
       ca_renderer_init runs the first shader compile.
       Read and write are independent: a directory a sandboxed app can
       read but not write (e.g. one pre-seeded read-only by an
       installer) still serves cache hits via shader_cache_writable ==
       false gating writes only, not lookups. */
    char  shader_cache_dir[1024];
    bool  shader_cache_writable;

    /* System defaults are instance-owned; author stylesheet is borrowed. */
    struct Ca_Stylesheet *system_stylesheet;
    struct Ca_Stylesheet *stylesheet;

    /* Background fallback for windows without a per-window override. */
    Ca_BgRenderFn default_bg_render_fn;
    void         *default_bg_render_data;

    /* Shared SSBO descriptor set layout and growable descriptor-pool chunks. */
    VkDescriptorSetLayout ssbo_desc_layout;
    Ca_DynArray           ssbo_desc_pools;
    uint32_t              min_ssbo_align;  /* minStorageBufferOffsetAlignment */

    /* Rect drawing pipeline — created on first window init */
    Ca_RectPipeline  rect_pipeline;

    /* Text pipeline + font atlas — created on first window init */
    Ca_TextPipeline  text_pipeline;
    Ca_Font         *font;   /* NULL until successfully loaded */

    /* Image pipeline — RGBA textured quad (shares text pipeline layout) */
    VkPipeline       image_pipeline;

    /* Backdrop blur pipeline — two-pass separable Gaussian.
       blur_h_pipeline == blur_v_pipeline (same code, direction via push constant).
       blur_pipeline_layout is the VkPipelineLayout for push constants.          */
    VkPipeline       blur_h_pipeline;
    VkPipeline       blur_v_pipeline;
    VkPipelineLayout blur_pipeline_layout;

    /* Stable user-image handles and growable sampled-image descriptor pools. */
    Ca_Pool           images;
    Ca_DynArray       image_desc_pools;

    /* App-level (system) menu bar — set via ca_instance_set_app_menus(). */
    Ca_DynArray    app_menu_storage;
    Ca_MenuBarMenu *app_menus;
    int             app_menu_count;

    /* When true, ca_window_system_tick polls continuously. */
    bool continuous;

    Ca_ProfileHooks profile_hooks;

    /* Earliest requested timed frame, in GLFW monotonic seconds. */
    double frame_deadline;
    bool   frame_deadline_pending;

    /* Instance-wide UI scale applied to every open and future window.
       0.0 / 1.0 both mean "no scaling".  Set via ca_instance_set_scale(). */
    float default_ui_scale;
};

static inline void ca_profile_begin(Ca_Instance *inst, const char *name)
{
    if (inst && inst->profile_hooks.begin)
        inst->profile_hooks.begin(inst->profile_hooks.user_data, name);
}

static inline void ca_profile_end(Ca_Instance *inst, const char *name)
{
    if (inst && inst->profile_hooks.end)
        inst->profile_hooks.end(inst->profile_hooks.user_data, name);
}

/* ======================================================
   THREAD
   ====================================================== */

struct Ca_Thread {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t handle;
#endif
};

struct Ca_Mutex {
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t  handle;
#endif
};

struct Ca_CondVar {
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t     handle;
#endif
};
