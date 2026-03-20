/* pipeline.c — Vulkan graphics pipeline for drawing filled rectangles.

   Design:
     - No vertex buffers. Each rect is a 6-vertex procedural quad driven
       entirely by push constants (pos, size, color, viewport size).
     - Viewport / scissor are dynamic states.
     - Alpha blending enabled for future semi-transparent widgets.
     - Built for dynamic rendering (no VkRenderPass object required).      */

#include "pipeline.h"
#include "shader.h"
#include "font.h"

/* ======================================================
   Embedded GLSL sources
   ====================================================== */

/* Vertex shader:
     Generates a 2-triangle quad from gl_VertexIndex (0-5).
     Converts pixel-space coordinates to NDC using the viewport size
     passed in push constants.  Outputs local position, size, and
     corner radius to the fragment shader for SDF evaluation.          */
static const char *VERT_GLSL =
    "#version 450\n"
    "\n"
    "layout(push_constant) uniform PC {\n"
    "    vec2  pos;\n"
    "    vec2  size;\n"
    "    vec4  color;\n"
    "    vec2  viewport;\n"
    "    float corner_radius;\n"
    "    float border_width;\n"
    "    vec4  border_color;\n"
    "} pc;\n"
    "\n"
    "layout(location = 0) out vec4  v_color;\n"
    "layout(location = 1) out vec2  v_local;\n"
    "layout(location = 2) out vec2  v_size;\n"
    "layout(location = 3) out float v_radius;\n"
    "layout(location = 4) out float v_border_w;\n"
    "layout(location = 5) out vec4  v_border_color;\n"
    "\n"
    "void main() {\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)\n"
    "    );\n"
    "    vec2 off   = offsets[gl_VertexIndex];\n"
    "    vec2 pixel = pc.pos + off * pc.size;\n"
    "    vec2 ndc   = (pixel / pc.viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_color  = pc.color;\n"
    "    v_local  = off * pc.size;\n"
    "    v_size   = pc.size;\n"
    "    v_radius = pc.corner_radius;\n"
    "    v_border_w     = pc.border_width;\n"
    "    v_border_color = pc.border_color;\n"
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

    /* Push constant range — vertex+fragment stages read pos/size/color/viewport/border */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(Ca_RectPushConst),
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc_range,
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
   computed from the push-constant uv rect (s0,t0,s1,t1).               */
static const char *TEXT_VERT_GLSL =
    "#version 450\n"
    "\n"
    "layout(push_constant) uniform PC {\n"
    "    vec2 pos;\n"
    "    vec2 size;\n"
    "    vec4 uv;       // (s0, t0, s1, t1)\n"
    "    vec4 color;\n"
    "    vec2 viewport;\n"
    "} pc;\n"
    "\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "layout(location = 1) out vec4 v_color;\n"
    "\n"
    "void main() {\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));\n"
    "    vec2 off   = offsets[gl_VertexIndex];\n"
    "    vec2 pixel = pc.pos + off * pc.size;\n"
    "    vec2 ndc   = (pixel / pc.viewport) * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    v_uv    = pc.uv.xy + off * (pc.uv.zw - pc.uv.xy);\n"
    "    v_color = pc.color;\n"
    "}\n";

/* Fragment shader: samples the R8 font atlas; alpha = atlas red channel. */
static const char *TEXT_FRAG_GLSL =
    "#version 450\n"
    "\n"
    "layout(set = 0, binding = 0) uniform sampler2D font_atlas;\n"
    "\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 1) in  vec4 v_color;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "\n"
    "void main() {\n"
    "    float a    = texture(font_atlas, v_uv).r;\n"
    "    out_color  = vec4(v_color.rgb, v_color.a * a);\n"
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

    /* Push constant range: full Ca_TextPushConst (56 bytes) */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(Ca_TextPushConst),
    };

    /* Pipeline layout */
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &tp->desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc_range,
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
    "layout(set = 0, binding = 0) uniform sampler2D tex;\n"
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
