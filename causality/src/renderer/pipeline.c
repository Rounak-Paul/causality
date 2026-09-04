// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* pipeline.c — Vulkan graphics pipelines for instanced rendering.

   Design:
     - No vertex buffers. Each rect/glyph/image is a 6-vertex procedural
       quad driven by per-instance data in a storage buffer (SSBO).
     - Instance data is indexed via gl_InstanceIndex in the vertex shader.
     - Multiple quads with the same scissor state are batched into a
       single vkCmdDraw(6, N, 0, firstInstance) call.
     - Viewport / scissor are dynamic states.
     - Alpha blending enabled for semi-transparent widgets.
     - Built for dynamic rendering (no VkRenderPass object required).      */

#include "pipeline.h"
#include "shader.h"
#include "font.h"
#include <string.h>

/* ======================================================
   Embedded GLSL sources
   ====================================================== */

/* Vertex shader:
     Generates a 2-triangle quad from gl_VertexIndex (0-5).
     Per-instance data (pos, size, color, etc.) is read from an SSBO
     indexed by gl_InstanceIndex.  Pixel-space coordinates are converted
     to NDC using the viewport size stored in each instance.           */
static const char *VERT_GLSL =
    "#version 450\n"
    "\n"
    "/* std430 layout — must match Ca_RectPushConst exactly */\n"
    "struct RectData {\n"
    "    vec2  pos;            /* offset   0 */\n"
    "    vec2  size;           /* offset   8 */\n"
    "    vec4  color;          /* offset  16 */\n"
    "    vec2  viewport;       /* offset  32 */\n"
    "    vec2  xf_ab;          /* offset  40 — transform col 0 (a, b) */\n"
    "    vec4  corner_radii;   /* offset  48 — tl, tr, br, bl */\n"
    "    vec4  border_color;   /* offset  64 */\n"
    "    vec4  color2;         /* offset  80 — gradient end / shadow tint */\n"
    "    float border_width;   /* offset  96 */\n"
    "    float blur_radius;    /* offset 100 */\n"
    "    uint  draw_mode;      /* offset 104 */\n"
    "    float gradient_angle; /* offset 108 — degrees */\n"
    "    float gradient_cx;    /* offset 112 — radial center 0..1 */\n"
    "    float gradient_cy;    /* offset 116 */\n"
    "    vec2  xf_cd;          /* offset 120 — transform col 1 (c, d) */\n"
    "};\n"
    "\n"
    "layout(std430, set = 0, binding = 0) readonly buffer SSB {\n"
    "    RectData data[];\n"
    "} ssb;\n"
    "\n"
    "layout(location = 0) out vec4  v_color;\n"
    "layout(location = 1) out vec2  v_local;\n"
    "layout(location = 2) out vec2  v_size;\n"
    "layout(location = 3) out vec4  v_corner_radii;\n"
    "layout(location = 4) out float v_border_w;\n"
    "layout(location = 5) out vec4  v_border_color;\n"
    "layout(location = 6) out vec4  v_color2;\n"
    "layout(location = 7) out float v_blur;\n"
    "layout(location = 8) flat out uint v_mode;\n"
    "layout(location = 9) out float v_grad_angle;\n"
    "layout(location = 10) out vec2 v_grad_center;\n"
    "layout(location = 11) out vec2 v_node_pos;\n"
    "\n"
    "void main() {\n"
    "    RectData d = ssb.data[gl_InstanceIndex];\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)\n"
    "    );\n"
    "    vec2 off   = offsets[gl_VertexIndex];\n"
    "    vec2 local = off * d.size;\n"
    /* mat2(col0, col1) — translation is pre-folded into d.pos on the CPU.
       v_local stays in untransformed node space so the fragment shader's
       rounded-corner and border SDF keeps measuring the real geometry. */
    "    vec2 xf    = mat2(d.xf_ab, d.xf_cd) * local;\n"
    "    vec2 pixel = d.pos + xf;\n"
    "    vec2 ndc   = (pixel / d.viewport) * 2.0 - 1.0;\n"
    "    gl_Position   = vec4(ndc, 0.0, 1.0);\n"
    "    v_color        = d.color;\n"
    "    v_local        = local;\n"
    "    v_size         = d.size;\n"
    "    v_corner_radii = d.corner_radii;\n"
    "    v_border_w     = d.border_width;\n"
    "    v_border_color = d.border_color;\n"
    "    v_color2       = d.color2;\n"
    "    v_blur         = d.blur_radius;\n"
    "    v_mode         = d.draw_mode;\n"
    "    v_grad_angle   = d.gradient_angle;\n"
    "    v_grad_center  = vec2(d.gradient_cx, d.gradient_cy);\n"
    /* Absolute node-space position (pre-viewport-NDC), so the fragment
       shader can test against a clip rect pushed in the same space
       without needing the clip origin relative to each instance. */
    "    v_node_pos     = pixel;\n"
    "}\n";

/* Fragment shader: rounded-rectangle SDF with anti-aliased edges.
   Output: linear-space color for an sRGB framebuffer.  CSS hex values
   are sRGB-encoded; we linearise per channel here so blending happens
   in linear light and the hardware re-encodes on write.               */
static const char *FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(location = 0) in  vec4  v_color;\n"
    "layout(location = 1) in  vec2  v_local;\n"
    "layout(location = 2) in  vec2  v_size;\n"
    "layout(location = 3) in  vec4  v_corner_radii;  /* tl, tr, br, bl */\n"
    "layout(location = 4) in  float v_border_w;\n"
    "layout(location = 5) in  vec4  v_border_color;\n"
    "layout(location = 6) in  vec4  v_color2;\n"
    "layout(location = 7) in  float v_blur;\n"
    "layout(location = 8) in  flat uint  v_mode;\n"
    "layout(location = 9) in  float v_grad_angle;\n"
    "layout(location = 10) in vec2  v_grad_center;\n"
    "layout(location = 11) in vec2  v_node_pos;\n"
    "layout(location = 0) out vec4  out_color;\n"
    "\n"
    /* Clip rect shared by every instance in the current batch (all instances
       in one vkCmdDraw share one hardware scissor, so one push constant is
       enough — no per-instance clip data needed). radius == 0 behaves as a
       plain rectangle clip, matching the pre-existing scissor-only clip. */
    "layout(push_constant) uniform ClipPC {\n"
    "    vec2  clip_pos;\n"
    "    vec2  clip_size;\n"
    "    float clip_radius;\n"
    "    float _clip_pad0;\n"
    "} clip_pc;\n"
    "\n"
    "#define MODE_NORMAL      0u\n"
    "#define MODE_SHADOW      1u\n"
    "#define MODE_LINEAR_GRAD 2u\n"
    "#define MODE_RADIAL_GRAD 3u\n"
    "\n"
    "vec3 srgb_to_linear(vec3 c) {\n"
    "    bvec3 cut = lessThan(c, vec3(0.04045));\n"
    "    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, vec3(cut));\n"
    "}\n"
    "\n"
    "float roundedClipSDF(vec2 abs_pos) {\n"
    "    vec2 half_size = clip_pc.clip_size * 0.5;\n"
    "    float radius   = min(clip_pc.clip_radius, min(half_size.x, half_size.y));\n"
    "    vec2  center   = clip_pc.clip_pos + half_size;\n"
    "    vec2  q = abs(abs_pos - center) - (half_size - vec2(radius));\n"
    "    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;\n"
    "}\n"
    "\n"
    "/* Per-corner rounded-box SDF.  p is centred on the box (half-size b).\n"
    "   corner_radii = (tl, tr, br, bl). */\n"
    "float roundedBoxSDF(vec2 p, vec2 b, vec4 r) {\n"
    "    /* Pick radius based on quadrant: tl=(-,-) tr=(+,-) br=(+,+) bl=(-,+) */\n"
    "    float rx = (p.x > 0.0) ?\n"
    "               ((p.y > 0.0) ? r.z : r.y) :\n"
    "               ((p.y > 0.0) ? r.w : r.x);\n"
    "    rx = min(rx, min(b.x, b.y));  /* clamp so radius never exceeds half-size */\n"
    "    vec2 d = abs(p) - b + vec2(rx);\n"
    "    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - rx;\n"
    "}\n"
    "\n"
    "/* SDF-based Gaussian shadow approximation.  sigma = blur / 2. */\n"
    "float shadowAlpha(float d, float blur) {\n"
    "    if (blur < 0.5) return (d < 0.0) ? 1.0 : 0.0;\n"
    "    float sigma = blur * 0.5;\n"
    "    return exp(-max(d, 0.0) * max(d, 0.0) / (2.0 * sigma * sigma));\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    /* ---- Rounded clip: an ancestor overflow:hidden panel with a\n"
    "       corner-radius masks this fragment to its curve, not just its\n"
    "       bounding box. The hardware scissor already pre-culls to that box\n"
    "       (clip_radius == 0 skips this test entirely — the common case). */\n"
    "    if (clip_pc.clip_radius > 0.0 && roundedClipSDF(v_node_pos) > 0.0) discard;\n"
    "\n"
    "    vec2  p    = v_local - v_size * 0.5;\n"
    "    vec4  r    = v_corner_radii;\n"
    "    vec2  box_half = v_size * 0.5;\n"
    "\n"
    "    /* ---- Shadow mode: SDF Gaussian falloff, no border, transparent fill ---- */\n"
    "    if (v_mode == MODE_SHADOW) {\n"
    "        float d   = roundedBoxSDF(p, box_half, r);\n"
    "        float alp = shadowAlpha(d, v_blur) * v_color.a;\n"
    "        out_color = vec4(srgb_to_linear(v_color.rgb), alp);\n"
    "        return;\n"
    "    }\n"
    "\n"
    "    /* ---- Outer shape SDF (shared by all non-shadow modes) ----\n"
    "       AA ramp width is derived from the screen-space derivative of the\n"
    "       SDF itself (fwidth), not a fixed logical-pixel constant, so edges\n"
    "       stay one *physical* pixel wide at any DPI/content-scale factor\n"
    "       instead of collapsing to a hard step when scale == 1.0. ---- */\n"
    "    float d_outer  = roundedBoxSDF(p, box_half, r);\n"
    "    float fw_outer = max(fwidth(d_outer), 1e-4);\n"
    "    float aa_outer = 1.0 - smoothstep(-fw_outer, fw_outer, d_outer);\n"
    "    if (aa_outer < 0.001) discard;\n"
    "\n"
    "    /* ---- Compute fill color (solid, linear-gradient, radial-gradient) ---- */\n"
    "    vec4 fill_color;\n"
    "    if (v_mode == MODE_LINEAR_GRAD) {\n"
    "        /* Angle 0 = top→bottom, 90 = left→right (CSS convention) */\n"
    "        float rad = v_grad_angle * 3.14159265 / 180.0;\n"
    "        vec2  dir = vec2(sin(rad), cos(rad));\n"
    "        /* Project centred position onto gradient axis — t in [-0.5, 0.5] → [0,1] */\n"
    "        float proj = dot(p / box_half, dir) * 0.5 + 0.5;\n"
    "        proj = clamp(proj, 0.0, 1.0);\n"
    "        fill_color = mix(v_color, v_color2, proj);\n"
    "    } else if (v_mode == MODE_RADIAL_GRAD) {\n"
    "        /* v_grad_center is 0..1 in rect space; convert to centred */\n"
    "        vec2 center = (v_grad_center - 0.5) * v_size;\n"
    "        float dist  = length(p - center);\n"
    "        float radius = length(box_half);  /* use half-diagonal as max radius */\n"
    "        float t = clamp(dist / radius, 0.0, 1.0);\n"
    "        fill_color = mix(v_color, v_color2, t);\n"
    "    } else {\n"
    "        fill_color = v_color;\n"
    "    }\n"
    "\n"
    "    vec3 fill_lin   = srgb_to_linear(fill_color.rgb);\n"
    "    vec3 border_lin = srgb_to_linear(v_border_color.rgb);\n"
    "\n"
    "    /* ---- Border ----\n"
    "       aa_inner (interior coverage) and bm (ring coverage) are\n"
    "       complementary and sum to aa_outer, so this is a straight two-way\n"
    "       coverage-weighted mix, not an \"over\" composite — no premultiply\n"
    "       needed. (A prior version built this via premultiplied-style\n"
    "       `fill + border * (1 - fill.a)` without actually premultiplying\n"
    "       fill.rgb by fill.a first, which produced RGB far brighter than\n"
    "       either input once the SRC_ALPHA/ONE_MINUS_SRC_ALPHA blend state\n"
    "       scaled that inflated RGB by the combined output alpha again —\n"
    "       visible as a solid, oversaturated tint instead of a thin ring.) */\n"
    "    if (v_border_w > 0.0) {\n"
    "        vec4 inner_r    = max(r - vec4(v_border_w), vec4(0.0));\n"
    "        vec2 inner_half = max(box_half - vec2(v_border_w), vec2(0.0));\n"
    "        float d_inner   = roundedBoxSDF(p, inner_half, inner_r);\n"
    "        float fw_inner  = max(fwidth(d_inner), 1e-4);\n"
    "        float aa_inner  = 1.0 - smoothstep(-fw_inner, fw_inner, d_inner);\n"
    "        float bm        = aa_outer - aa_inner;\n"
    "        float out_a     = fill_color.a * aa_inner + v_border_color.a * bm;\n"
    "        vec3  out_rgb   = (out_a > 0.0001)\n"
    "            ? (fill_lin * (fill_color.a * aa_inner) +\n"
    "               border_lin * (v_border_color.a * bm)) / out_a\n"
    "            : fill_lin;\n"
    "        out_color = vec4(out_rgb, out_a);\n"
    "    } else {\n"
    "        out_color = vec4(fill_lin, fill_color.a * aa_outer);\n"
    "    }\n"
    "}\n";

/* ======================================================
   Pipeline creation
   ====================================================== */

bool ca_rect_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    /* Compile shaders at runtime */
    VkShaderModule vert = ca_shader_compile(inst, VERT_GLSL,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst, FRAG_GLSL,
                                            VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, vert, NULL);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, frag, NULL);
        return false;
    }

    /* Pipeline layout — set 0 = SSBO, plus a small fragment-stage push
       constant carrying the rounded clip rect shared by the current batch
       (all instances in one vkCmdDraw share one hardware scissor, so one
       push constant per batch is sufficient — see Ca_ClipPushConst). */
    VkPushConstantRange clip_pcr = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(Ca_ClipPushConst),
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &inst->ssbo_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &clip_pcr,
    };
    if (vkCreatePipelineLayout(inst->vk_device, &layout_ci, NULL,
                               &inst->rect_pipeline.layout) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] vkCreatePipelineLayout failed\n");
        vkDestroyShaderModule(inst->vk_device, vert, NULL);
        vkDestroyShaderModule(inst->vk_device, frag, NULL);
        return false;
    }

    /* Shader stages */
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName  = "main",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName  = "main",
        },
    };

    /* No vertex bindings — quad generated from gl_VertexIndex */
    VkPipelineVertexInputStateCreateInfo vert_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    /* Viewport and scissor are set dynamically per frame */
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,          /* rects can face either way */
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    /* Standard over-compositing (src-alpha blending) */
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };

    /* Dynamic rendering — attach format must match the swapchain */
    VkPipelineRenderingCreateInfo rendering_ci = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &color_format,
    };

    VkGraphicsPipelineCreateInfo pipeline_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering_ci,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vert_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState   = &multisample,
        .pColorBlendState    = &color_blend,
        .pDynamicState       = &dynamic_state,
        .layout              = inst->rect_pipeline.layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    VkResult r = vkCreateGraphicsPipelines(inst->vk_device, VK_NULL_HANDLE,
                                           1, &pipeline_ci, NULL,
                                           &inst->rect_pipeline.pipeline);
    /* Shader modules are no longer needed after pipeline creation */
    vkDestroyShaderModule(inst->vk_device, vert, NULL);
    vkDestroyShaderModule(inst->vk_device, frag, NULL);

    if (r != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] vkCreateGraphicsPipelines failed: %d\n", r);
        vkDestroyPipelineLayout(inst->vk_device, inst->rect_pipeline.layout, NULL);
        inst->rect_pipeline.layout = VK_NULL_HANDLE;
        return false;
    }

    printf("[pipeline] rect pipeline created (format %d)\n", color_format);
    return true;
}

void ca_rect_pipeline_destroy(Ca_Instance *inst)
{
    if (inst->rect_pipeline.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(inst->vk_device, inst->rect_pipeline.pipeline, NULL);
        inst->rect_pipeline.pipeline = VK_NULL_HANDLE;
    }
    if (inst->rect_pipeline.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(inst->vk_device, inst->rect_pipeline.layout, NULL);
        inst->rect_pipeline.layout = VK_NULL_HANDLE;
    }
}

/* ======================================================
   Text pipeline — textured glyph quads
   ====================================================== */

/* Vertex shader: same procedural-quad trick, but outputs UV coordinates
   computed from the SSBO instance data (s0,t0,s1,t1).               */
static const char *TEXT_VERT_GLSL =
    "#version 450\n"
    "\n"
    "struct TextData {\n"
    "    vec2 pos;\n"
    "    vec2 size;\n"
    "    vec4 uv;       // (s0, t0, s1, t1)\n"
    "    vec4 color;\n"
    "    vec2 viewport;\n"
    "    vec2 xf_ab;    // transform col 0 (a, b)\n"
    "    vec2 xf_cd;    // transform col 1 (c, d)\n"
    /* Padding MUST be declared as vec2, never vec4: in std430 a vec4 is
       16-byte aligned, which would insert 8 bytes of padding after xf_cd
       (offset 64) and inflate this struct to 144 bytes, while the C-side
       Ca_TextInstance is 128. The shader indexes the SSBO by struct
       stride, so the mismatch would misread every glyph after the first. */
    "    vec2 _pad1[7];\n"
    "};\n"
    "\n"
    "layout(std430, set = 0, binding = 0) readonly buffer SSB {\n"
    "    TextData data[];\n"
    "} ssb;\n"
    "\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "layout(location = 1) out vec4 v_color;\n"
    "\n"
    "void main() {\n"
    "    TextData d = ssb.data[gl_InstanceIndex];\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));\n"
    "    vec2 off   = offsets[gl_VertexIndex];\n"
    /* Same transform convention as the rect pipeline: 2x2 about the glyph
       quad's local origin, translation pre-folded into d.pos. */
    "    vec2 xf    = mat2(d.xf_ab, d.xf_cd) * (off * d.size);\n"
    "    vec2 pixel = d.pos + xf;\n"
    "    vec2 ndc   = (pixel / d.viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_uv    = d.uv.xy + off * (d.uv.zw - d.uv.xy);\n"
    "    v_color = d.color;\n"
    "}\n";

/* Fragment shader: text coverage compositing.

   The atlas stores either grayscale coverage with equal RGB channels or LCD
   coverage with independent RGB channels.  The shader preserves per-channel
   LCD source coverage while keeping the pipeline on core premultiplied-alpha
   blending, so Causality gets crisp 1x text without depending on platform
   fonts or optional dual-source blend features. */
static const char *TEXT_FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(set = 1, binding = 0) uniform sampler2D font_atlas;\n"
    "\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 1) in  vec4 v_color;\n"
    "\n"
    "layout(location = 0) out vec4 out_color;\n"
    "\n"
    "vec3 srgb_to_linear(vec3 c) {\n"
    "    bvec3 cutoff = lessThan(c, vec3(0.04045));\n"
    "    vec3 lo = c / 12.92;\n"
    "    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));\n"
    "    return mix(hi, lo, vec3(cutoff));\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 sample_cov = texture(font_atlas, v_uv);\n"
    "    vec3 cov = sample_cov.rgb;\n"
    "    float a = max(max(cov.r, cov.g), cov.b) * v_color.a;\n"
    "    vec3 col_lin = srgb_to_linear(v_color.rgb);\n"
    "    out_color = vec4(col_lin * cov * v_color.a, a);\n"
    "}\n";

bool ca_text_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    Ca_TextPipeline *tp = &inst->text_pipeline;

    /* Descriptor set layout: binding 0 = combined image sampler */
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dsl_ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &binding,
    };
    if (vkCreateDescriptorSetLayout(inst->vk_device, &dsl_ci, NULL,
                                    &tp->desc_layout) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] vkCreateDescriptorSetLayout failed\n");
        return false;
    }

    /* Descriptor pool */
    VkDescriptorPoolSize pool_sz = {
        .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = 1,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_sz,
    };
    if (vkCreateDescriptorPool(inst->vk_device, &pool_ci, NULL,
                               &tp->desc_pool) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] vkCreateDescriptorPool failed\n");
        return false;
    }

    /* Descriptor set */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = tp->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &tp->desc_layout,
    };
    if (vkAllocateDescriptorSets(inst->vk_device, &ds_ai,
                                 &tp->desc_set) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] vkAllocateDescriptorSets failed\n");
        return false;
    }

    /* Push constant range: none (instance data in SSBO) */

    /* Pipeline layout: set 0 = SSBO, set 1 = sampler */
    VkDescriptorSetLayout set_layouts[2] = {
        inst->ssbo_desc_layout, tp->desc_layout
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 2,
        .pSetLayouts            = set_layouts,
    };
    if (vkCreatePipelineLayout(inst->vk_device, &layout_ci, NULL,
                               &tp->layout) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] text vkCreatePipelineLayout failed\n");
        return false;
    }

    /* Compile shaders */
    VkShaderModule vert = ca_shader_compile(inst, TEXT_VERT_GLSL,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst, TEXT_FRAG_GLSL,
                                            VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        fprintf(stderr, "[pipeline] text shader compilation failed\n");
        if (vert != VK_NULL_HANDLE)
            vkDestroyShaderModule(inst->vk_device, vert, NULL);
        if (frag != VK_NULL_HANDLE)
            vkDestroyShaderModule(inst->vk_device, frag, NULL);
        vkDestroyPipelineLayout(inst->vk_device, tp->layout, NULL);
        tp->layout = VK_NULL_HANDLE;
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vert,
          .pName  = "main" },
        { .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = frag,
          .pName  = "main" },
    };

    /* Reuse the same fixed-function state as the rect pipeline */
    VkPipelineVertexInputStateCreateInfo   vert_input  = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo input_asm   = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo      viewport    = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster      = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f };
    VkPipelineMultisampleStateCreateInfo   multisample = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };

    /* Premultiplied-alpha text blend.  RGB may contain independent LCD
       coverage, while alpha carries conservative whole-pixel coverage. */
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states,
    };

    /* Dynamic rendering attachment */
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &color_format,
    };

    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vert_input,
        .pInputAssemblyState = &input_asm,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dyn,
        .layout              = tp->layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    VkResult res = vkCreateGraphicsPipelines(inst->vk_device, VK_NULL_HANDLE,
                                              1, &gp_ci, NULL, &tp->pipeline);
    vkDestroyShaderModule(inst->vk_device, vert, NULL);
    vkDestroyShaderModule(inst->vk_device, frag, NULL);

    if (res != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] vkCreateGraphicsPipelines (text) failed: %d\n", res);
        vkDestroyPipelineLayout(inst->vk_device, tp->layout, NULL);
        tp->layout = VK_NULL_HANDLE;
        return false;
    }

    printf("[pipeline] text pipeline created (format %d)\n", color_format);
    return true;
}

void ca_text_pipeline_update_font(Ca_Instance *inst)
{
    if (!inst->font || inst->text_pipeline.desc_set == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo img_info = {
        .sampler     = inst->font->sampler,
        .imageView   = inst->font->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = inst->text_pipeline.desc_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &img_info,
    };
    vkUpdateDescriptorSets(inst->vk_device, 1, &write, 0, NULL);
}

void ca_text_pipeline_destroy(Ca_Instance *inst)
{
    Ca_TextPipeline *tp = &inst->text_pipeline;
    if (tp->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(inst->vk_device, tp->pipeline, NULL);
        tp->pipeline = VK_NULL_HANDLE;
    }
    if (tp->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(inst->vk_device, tp->layout, NULL);
        tp->layout = VK_NULL_HANDLE;
    }
    if (tp->desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(inst->vk_device, tp->desc_pool, NULL);
        tp->desc_pool = VK_NULL_HANDLE;
    }
    if (tp->desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(inst->vk_device, tp->desc_layout, NULL);
        tp->desc_layout = VK_NULL_HANDLE;
    }
}

/* ======================================================
   Image pipeline — RGBA textured quads with optional rounded clipping.
   Shares the text pipeline's layout (descriptor set layout + push constants).
   ====================================================== */

static const char *IMAGE_VERT_GLSL =
    "#version 450\n"
    "\n"
    "struct ImageData {\n"
    "    vec2 pos;\n"
    "    vec2 size;\n"
    "    vec4 uv;\n"
    "    vec4 color;\n"
    "    vec2 viewport;\n"
    "    vec2 xf_ab;\n"
    "    vec2 xf_cd;\n"
    "    vec2 corner_01; // tl, tr\n"
    "    vec2 corner_23; // br, bl\n"
    "    vec2 _pad1[5];\n"
    "};\n"
    "\n"
    "layout(std430, set = 0, binding = 0) readonly buffer SSB {\n"
    "    ImageData data[];\n"
    "} ssb;\n"
    "\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "layout(location = 1) out vec4 v_color;\n"
    "layout(location = 2) out vec2 v_local;\n"
    "layout(location = 3) out vec2 v_size;\n"
    "layout(location = 4) out vec4 v_corner_radii;\n"
    "\n"
    "void main() {\n"
    "    ImageData d = ssb.data[gl_InstanceIndex];\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));\n"
    "    vec2 off = offsets[gl_VertexIndex];\n"
    "    vec2 xf = mat2(d.xf_ab, d.xf_cd) * (off * d.size);\n"
    "    vec2 pixel = d.pos + xf;\n"
    "    vec2 ndc = (pixel / d.viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_uv = d.uv.xy + off * (d.uv.zw - d.uv.xy);\n"
    "    v_color = d.color;\n"
    "    v_local = off * d.size;\n"
    "    v_size = d.size;\n"
    "    v_corner_radii = vec4(d.corner_01, d.corner_23);\n"
    "}\n";

static const char *IMAGE_FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(set = 1, binding = 0) uniform sampler2D tex;\n"
    "\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 1) in  vec4 v_color;\n"
    "layout(location = 2) in  vec2 v_local;\n"
    "layout(location = 3) in  vec2 v_size;\n"
    "layout(location = 4) in  vec4 v_corner_radii;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "\n"
    "vec3 srgb_to_linear(vec3 c) {\n"
    "    bvec3 cutoff = lessThan(c, vec3(0.04045));\n"
    "    vec3 lo = c / 12.92;\n"
    "    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));\n"
    "    return mix(hi, lo, vec3(cutoff));\n"
    "}\n"
    "\n"
    "float roundedBoxSDF(vec2 p, vec2 b, vec4 r) {\n"
    "    float radius = (p.x > 0.0) ?\n"
    "        ((p.y > 0.0) ? r.z : r.y) :\n"
    "        ((p.y > 0.0) ? r.w : r.x);\n"
    "    radius = clamp(radius, 0.0, min(b.x, b.y));\n"
    "    vec2 d = abs(p) - b + vec2(radius);\n"
    "    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - radius;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 half_size = v_size * 0.5;\n"
    "    float distance = roundedBoxSDF(v_local - half_size, half_size,\n"
    "                                   v_corner_radii);\n"
    "    float fw       = max(fwidth(distance), 1e-4);\n"
    "    float coverage = 1.0 - smoothstep(-fw, fw, distance);\n"
    "    if (coverage < 0.001) discard;\n"
    "    /* Texture is bound as R8G8B8A8_SRGB so sampling returns linear RGB. */\n"
    "    vec4 t = texture(tex, v_uv);\n"
    "    vec3 tint_lin = srgb_to_linear(v_color.rgb);\n"
    "    out_color = vec4(t.rgb * tint_lin, t.a * v_color.a * coverage);\n"
    "}\n";

bool ca_image_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    if (inst->text_pipeline.layout == VK_NULL_HANDLE) return false;

    VkShaderModule vert = ca_shader_compile(inst, IMAGE_VERT_GLSL,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst, IMAGE_FRAG_GLSL,
                                            VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, vert, NULL);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, frag, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
    };

    VkPipelineVertexInputStateCreateInfo   vert_input  = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo input_asm   = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo      viewport    = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster      = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f };
    VkPipelineMultisampleStateCreateInfo   multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_att };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states };
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format };

    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vert_input,
        .pInputAssemblyState = &input_asm,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dyn,
        .layout              = inst->text_pipeline.layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    VkResult res = vkCreateGraphicsPipelines(inst->vk_device, VK_NULL_HANDLE,
                                              1, &gp_ci, NULL,
                                              &inst->image_pipeline);
    vkDestroyShaderModule(inst->vk_device, vert, NULL);
    vkDestroyShaderModule(inst->vk_device, frag, NULL);

    if (res != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] image pipeline creation failed: %d\n", res);
        return false;
    }

    printf("[pipeline] image pipeline created (format %d)\n", color_format);
    return true;
}

void ca_image_pipeline_destroy(Ca_Instance *inst)
{
    if (inst->image_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(inst->vk_device, inst->image_pipeline, NULL);
        inst->image_pipeline = VK_NULL_HANDLE;
    }
}

/* ======================================================
   Shared SSBO descriptor set layout (instanced rendering)
   ====================================================== */

bool ca_ssbo_layout_create(Ca_Instance *inst)
{
    ca_dyn_array_init(&inst->ssbo_desc_pools, sizeof(VkDescriptorPool));
    /* Query min alignment for debug reporting and future buffer partitioning. */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(inst->vk_gpu, &props);
    inst->min_ssbo_align = (uint32_t)props.limits.minStorageBufferOffsetAlignment;
    if (inst->min_ssbo_align == 0) inst->min_ssbo_align = 256;

    /* Descriptor set layout: binding 0 = storage buffer */
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &binding,
    };
    if (vkCreateDescriptorSetLayout(inst->vk_device, &ci, NULL,
                                    &inst->ssbo_desc_layout) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] SSBO descriptor set layout creation failed\n");
        ca_dyn_array_destroy(&inst->ssbo_desc_pools);
        return false;
    }

    printf("[pipeline] SSBO layout created (min_align=%u)\n", inst->min_ssbo_align);
    return true;
}

void ca_ssbo_layout_destroy(Ca_Instance *inst)
{
    VkDescriptorPool *pools = (VkDescriptorPool *)inst->ssbo_desc_pools.data;
    for (size_t i = 0; i < inst->ssbo_desc_pools.count; ++i) {
        if (pools[i] != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(inst->vk_device, pools[i], NULL);
    }
    ca_dyn_array_destroy(&inst->ssbo_desc_pools);
    if (inst->ssbo_desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(inst->vk_device, inst->ssbo_desc_layout, NULL);
        inst->ssbo_desc_layout = VK_NULL_HANDLE;
    }
}

enum { CA_SSBO_DESCRIPTOR_POOL_CHUNK = 64 };

/** Allocate one SSBO descriptor from a reusable growable pool set. */
static bool ssbo_descriptor_allocate(Ca_Instance *inst, Ca_Frame *frame)
{
    VkDescriptorPool *pools = (VkDescriptorPool *)inst->ssbo_desc_pools.data;
    for (size_t i = 0; i < inst->ssbo_desc_pools.count; ++i) {
        VkDescriptorSetAllocateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pools[i],
            .descriptorSetCount = 1,
            .pSetLayouts = &inst->ssbo_desc_layout,
        };
        VkResult result = vkAllocateDescriptorSets(inst->vk_device, &info,
                                                    &frame->ssbo_set);
        if (result == VK_SUCCESS) {
            frame->ssbo_pool = pools[i];
            return true;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY &&
            result != VK_ERROR_FRAGMENTED_POOL)
            return false;
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = CA_SSBO_DESCRIPTOR_POOL_CHUNK,
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = CA_SSBO_DESCRIPTOR_POOL_CHUNK,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(inst->vk_device, &pool_info, NULL, &pool) !=
        VK_SUCCESS)
        return false;
    if (!ca_dyn_array_push(&inst->ssbo_desc_pools, &pool)) {
        vkDestroyDescriptorPool(inst->vk_device, pool, NULL);
        return false;
    }
    VkDescriptorSetAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &inst->ssbo_desc_layout,
    };
    if (vkAllocateDescriptorSets(inst->vk_device, &info, &frame->ssbo_set) !=
        VK_SUCCESS)
        return false;
    frame->ssbo_pool = pool;
    return true;
}

/** Allocates a mapped SSBO without publishing partial frame state. */
static bool instance_buffer_allocate(Ca_Instance *inst, VkDeviceSize size,
                                     VkBuffer *out_buffer,
                                     VkDeviceMemory *out_memory,
                                     void **out_mapped)
{
    *out_buffer = VK_NULL_HANDLE;
    *out_memory = VK_NULL_HANDLE;
    *out_mapped = NULL;
    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(inst->vk_device, &buf_ci, NULL, out_buffer) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] instance buffer creation failed\n");
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(inst->vk_device, *out_buffer, &req);
    uint32_t mem_type = ca_gpu_find_memory_type(inst, req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "[pipeline] no suitable memory type for instance buffer\n");
        vkDestroyBuffer(inst->vk_device, *out_buffer, NULL);
        *out_buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(inst->vk_device, &mem_ai, NULL, out_memory) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] instance buffer memory alloc failed\n");
        vkDestroyBuffer(inst->vk_device, *out_buffer, NULL);
        *out_buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(inst->vk_device, *out_buffer, *out_memory, 0) !=
            VK_SUCCESS ||
        vkMapMemory(inst->vk_device, *out_memory, 0, size, 0, out_mapped) !=
            VK_SUCCESS) {
        fprintf(stderr, "[pipeline] instance buffer bind/map failed\n");
        vkFreeMemory(inst->vk_device, *out_memory, NULL);
        vkDestroyBuffer(inst->vk_device, *out_buffer, NULL);
        *out_buffer = VK_NULL_HANDLE;
        *out_memory = VK_NULL_HANDLE;
        *out_mapped = NULL;
        return false;
    }
    return true;
}

bool ca_instance_buf_ensure(Ca_Instance *inst, Ca_Frame *f,
                            uint32_t minimum_slots)
{
    if (!inst || !f) return false;
    if (minimum_slots == 0) minimum_slots = 1;
    if (f->instance_buf != VK_NULL_HANDLE &&
        f->instance_capacity >= minimum_slots)
        return true;

    uint32_t capacity = f->instance_capacity > 0
        ? f->instance_capacity : 256u;
    while (capacity < minimum_slots) {
        uint32_t growth = capacity / 2u;
        if (growth < 256u) growth = 256u;
        if (capacity > UINT32_MAX - growth) {
            capacity = minimum_slots;
            break;
        }
        capacity += growth;
    }
    if ((VkDeviceSize)capacity > UINT64_MAX / CA_INSTANCE_SLOT_SIZE)
        return false;
    VkDeviceSize size = (VkDeviceSize)capacity * CA_INSTANCE_SLOT_SIZE;

    VkBuffer new_buffer;
    VkDeviceMemory new_memory;
    void *new_mapped;
    if (!instance_buffer_allocate(inst, size, &new_buffer, &new_memory,
                                  &new_mapped))
        return false;

    if (f->ssbo_set == VK_NULL_HANDLE) {
        if (!ssbo_descriptor_allocate(inst, f)) {
            fprintf(stderr, "[pipeline] SSBO descriptor set alloc failed\n");
            vkUnmapMemory(inst->vk_device, new_memory);
            vkFreeMemory(inst->vk_device, new_memory, NULL);
            vkDestroyBuffer(inst->vk_device, new_buffer, NULL);
            return false;
        }
    }
    VkDescriptorBufferInfo buf_info = {
        .buffer = new_buffer,
        .offset = 0,
        .range  = size,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = f->ssbo_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo     = &buf_info,
    };
    vkUpdateDescriptorSets(inst->vk_device, 1, &write, 0, NULL);

    if (f->instance_mapped)
        vkUnmapMemory(inst->vk_device, f->instance_mem);
    if (f->instance_buf != VK_NULL_HANDLE)
        vkDestroyBuffer(inst->vk_device, f->instance_buf, NULL);
    if (f->instance_mem != VK_NULL_HANDLE)
        vkFreeMemory(inst->vk_device, f->instance_mem, NULL);
    f->instance_buf = new_buffer;
    f->instance_mem = new_memory;
    f->instance_mapped = new_mapped;
    f->instance_capacity = capacity;
    return true;
}

bool ca_instance_buf_create(Ca_Instance *inst, Ca_Frame *f)
{
    return ca_instance_buf_ensure(inst, f, 256u);
}

void ca_instance_buf_destroy(Ca_Instance *inst, Ca_Frame *f)
{
    if (f->instance_mapped) {
        vkUnmapMemory(inst->vk_device, f->instance_mem);
        f->instance_mapped = NULL;
    }
    if (f->ssbo_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(inst->vk_device, f->ssbo_pool, 1, &f->ssbo_set);
        f->ssbo_set = VK_NULL_HANDLE;
        f->ssbo_pool = VK_NULL_HANDLE;
    }
    if (f->instance_buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(inst->vk_device, f->instance_buf, NULL);
        f->instance_buf = VK_NULL_HANDLE;
    }
    if (f->instance_mem != VK_NULL_HANDLE) {
        vkFreeMemory(inst->vk_device, f->instance_mem, NULL);
        f->instance_mem = VK_NULL_HANDLE;
    }
    f->instance_capacity = 0;
}
