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
    "struct RectData {\n"
    "    vec2  pos;\n"
    "    vec2  size;\n"
    "    vec4  color;\n"
    "    vec2  viewport;\n"
    "    float corner_radius;\n"
    "    float border_width;\n"
    "    vec4  border_color;\n"
    "};\n"
    "\n"
    "layout(std430, set = 0, binding = 0) readonly buffer SSB {\n"
    "    RectData data[];\n"
    "} ssb;\n"
    "\n"
    "layout(location = 0) out vec4  v_color;\n"
    "layout(location = 1) out vec2  v_local;\n"
    "layout(location = 2) out vec2  v_size;\n"
    "layout(location = 3) out float v_radius;\n"
    "layout(location = 4) out float v_border_w;\n"
    "layout(location = 5) out vec4  v_border_color;\n"
    "\n"
    "void main() {\n"
    "    RectData d = ssb.data[gl_InstanceIndex];\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)\n"
    "    );\n"
    "    vec2 off   = offsets[gl_VertexIndex];\n"
    "    vec2 pixel = d.pos + off * d.size;\n"
    "    vec2 ndc   = (pixel / d.viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color  = d.color;\n"
    "    v_local  = off * d.size;\n"
    "    v_size   = d.size;\n"
    "    v_radius = d.corner_radius;\n"
    "    v_border_w     = d.border_width;\n"
    "    v_border_color = d.border_color;\n"
    "}\n";

/* Fragment shader: rounded-rectangle SDF with anti-aliased edges.
   When corner_radius is 0, falls back to plain colour output.         */
static const char *FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(location = 0) in  vec4  v_color;\n"
    "layout(location = 1) in  vec2  v_local;\n"
    "layout(location = 2) in  vec2  v_size;\n"
    "layout(location = 3) in  float v_radius;\n"
    "layout(location = 4) in  float v_border_w;\n"
    "layout(location = 5) in  vec4  v_border_color;\n"
    "layout(location = 0) out vec4  out_color;\n"
    "\n"
    "float roundedBoxSDF(vec2 p, vec2 b, float r) {\n"
    "    vec2 d = abs(p) - b + vec2(r);\n"
    "    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - r;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 p = v_local - v_size * 0.5;\n"
    "    float d_outer = roundedBoxSDF(p, v_size * 0.5, v_radius);\n"
    "    float aa_outer = 1.0 - smoothstep(-0.5, 0.5, d_outer);\n"
    "    if (v_border_w > 0.0) {\n"
    "        float inner_r = max(v_radius - v_border_w, 0.0);\n"
    "        vec2 inner_half = v_size * 0.5 - vec2(v_border_w);\n"
    "        float d_inner = roundedBoxSDF(p, max(inner_half, vec2(0.0)), inner_r);\n"
    "        float aa_inner = 1.0 - smoothstep(-0.5, 0.5, d_inner);\n"
    "        float border_mask = aa_outer - aa_inner;\n"
    "        vec4 fill = vec4(v_color.rgb, v_color.a * aa_inner);\n"
    "        vec4 border = vec4(v_border_color.rgb, v_border_color.a * border_mask);\n"
    "        out_color = fill + border * (1.0 - fill.a);\n"
    "    } else if (v_radius > 0.0) {\n"
    "        out_color = vec4(v_color.rgb, v_color.a * aa_outer);\n"
    "    } else {\n"
    "        out_color = v_color;\n"
    "    }\n"
    "}\n";

/* ======================================================
   Pipeline creation
   ====================================================== */

bool ca_rect_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    /* Compile shaders at runtime */
    VkShaderModule vert = ca_shader_compile(inst->vk_device, VERT_GLSL,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst->vk_device, FRAG_GLSL,
                                            VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, vert, NULL);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, frag, NULL);
        return false;
    }

    /* Pipeline layout — set 0 = SSBO (no push constants) */
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &inst->ssbo_desc_layout,
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
    "    vec2 _pad;\n"
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
    "    vec2 pixel = d.pos + off * d.size;\n"
    "    vec2 ndc   = (pixel / d.viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_uv    = d.uv.xy + off * (d.uv.zw - d.uv.xy);\n"
    "    v_color = d.color;\n"
    "}\n";

/* Fragment shader: samples the R8 font atlas; alpha = atlas red channel.
   Apple-style rendering: gamma-correct alpha for fuller, smoother glyphs
   without hinting, preserving all glyph shape detail.                   */
static const char *TEXT_FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(set = 1, binding = 0) uniform sampler2D font_atlas;\n"
    "\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 1) in  vec4 v_color;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "\n"
    "void main() {\n"
    "    float a = texture(font_atlas, v_uv).r;\n"
    "    a = pow(a, 0.45);\n"
    "    out_color = vec4(v_color.rgb, v_color.a * a);\n"
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
    VkShaderModule vert = ca_shader_compile(inst->vk_device, TEXT_VERT_GLSL,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst->vk_device, TEXT_FRAG_GLSL,
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

    /* Alpha blending */
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
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
   Image pipeline — RGBA textured quads (same vertex shader as text,
   different fragment shader that samples all 4 channels).
   Shares the text pipeline's layout (descriptor set layout + push constants).
   ====================================================== */

static const char *IMAGE_FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(set = 1, binding = 0) uniform sampler2D tex;\n"
    "\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 1) in  vec4 v_color;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "\n"
    "void main() {\n"
    "    vec4 t = texture(tex, v_uv);\n"
    "    out_color = vec4(t.rgb, t.a * v_color.a);\n"
    "}\n";

bool ca_image_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    if (inst->text_pipeline.layout == VK_NULL_HANDLE) return false;

    VkShaderModule vert = ca_shader_compile(inst->vk_device, TEXT_VERT_GLSL,
                                            VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst->vk_device, IMAGE_FRAG_GLSL,
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

static uint32_t find_memory_type_pipe(VkPhysicalDevice gpu, uint32_t type_bits,
                                      VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

bool ca_ssbo_layout_create(Ca_Instance *inst)
{
    /* Query min alignment for dynamic SSBO offsets */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(inst->vk_gpu, &props);
    inst->min_ssbo_align = (uint32_t)props.limits.minStorageBufferOffsetAlignment;
    if (inst->min_ssbo_align == 0) inst->min_ssbo_align = 256;

    /* Descriptor set layout: binding 0 = dynamic storage buffer */
    VkDescriptorSetLayoutBinding binding = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
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
        return false;
    }

    /* Descriptor pool: one SSBO set per frame-in-flight per window */
    VkDescriptorPoolSize pool_sz = {
        .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        .descriptorCount = CA_MAX_WINDOWS * CA_FRAMES_IN_FLIGHT,
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = CA_MAX_WINDOWS * CA_FRAMES_IN_FLIGHT,
        .poolSizeCount = 1,
        .pPoolSizes    = &pool_sz,
    };
    if (vkCreateDescriptorPool(inst->vk_device, &pool_ci, NULL,
                               &inst->ssbo_desc_pool) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] SSBO descriptor pool creation failed\n");
        return false;
    }

    printf("[pipeline] SSBO layout created (min_align=%u)\n", inst->min_ssbo_align);
    return true;
}

void ca_ssbo_layout_destroy(Ca_Instance *inst)
{
    if (inst->ssbo_desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(inst->vk_device, inst->ssbo_desc_pool, NULL);
        inst->ssbo_desc_pool = VK_NULL_HANDLE;
    }
    if (inst->ssbo_desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(inst->vk_device, inst->ssbo_desc_layout, NULL);
        inst->ssbo_desc_layout = VK_NULL_HANDLE;
    }
}

bool ca_instance_buf_create(Ca_Instance *inst, Ca_Frame *f)
{
    /* Create host-visible coherent buffer for instance data */
    VkBufferCreateInfo buf_ci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = CA_INSTANCE_BUF_SIZE,
        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(inst->vk_device, &buf_ci, NULL, &f->instance_buf) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] instance buffer creation failed\n");
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(inst->vk_device, f->instance_buf, &req);
    VkMemoryAllocateInfo mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = find_memory_type_pipe(inst->vk_gpu, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (vkAllocateMemory(inst->vk_device, &mem_ai, NULL, &f->instance_mem) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] instance buffer memory alloc failed\n");
        return false;
    }
    vkBindBufferMemory(inst->vk_device, f->instance_buf, f->instance_mem, 0);
    vkMapMemory(inst->vk_device, f->instance_mem, 0, CA_INSTANCE_BUF_SIZE, 0,
                &f->instance_mapped);

    /* Allocate and write descriptor set pointing to this buffer */
    VkDescriptorSetAllocateInfo ds_ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = inst->ssbo_desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &inst->ssbo_desc_layout,
    };
    if (vkAllocateDescriptorSets(inst->vk_device, &ds_ai, &f->ssbo_set) != VK_SUCCESS) {
        fprintf(stderr, "[pipeline] SSBO descriptor set alloc failed\n");
        return false;
    }
    VkDescriptorBufferInfo buf_info = {
        .buffer = f->instance_buf,
        .offset = 0,
        .range  = CA_INSTANCE_BUF_SIZE,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = f->ssbo_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        .pBufferInfo     = &buf_info,
    };
    vkUpdateDescriptorSets(inst->vk_device, 1, &write, 0, NULL);
    return true;
}

void ca_instance_buf_destroy(Ca_Instance *inst, Ca_Frame *f)
{
    if (f->instance_mapped) {
        vkUnmapMemory(inst->vk_device, f->instance_mem);
        f->instance_mapped = NULL;
    }
    if (f->ssbo_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(inst->vk_device, inst->ssbo_desc_pool,
                             1, &f->ssbo_set);
        f->ssbo_set = VK_NULL_HANDLE;
    }
    if (f->instance_buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(inst->vk_device, f->instance_buf, NULL);
        f->instance_buf = VK_NULL_HANDLE;
    }
    if (f->instance_mem != VK_NULL_HANDLE) {
        vkFreeMemory(inst->vk_device, f->instance_mem, NULL);
        f->instance_mem = VK_NULL_HANDLE;
    }
}
