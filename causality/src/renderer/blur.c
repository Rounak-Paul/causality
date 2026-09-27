// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* blur.c — CSS backdrop-filter: blur() implementation.

   Per the CSS Filter Effects spec an element's backdrop is everything
   painted before it, blurred with a Gaussian of standard deviation equal to
   the blur() length, clipped to the element's border box and drawn beneath
   the element's own background. swapchain.c therefore calls
   ca_blur_capture_region at each backdrop element's exact position in paint
   order (outside dynamic rendering), then composites with
   backdrop_pipeline before the element's background and children paint.

   Per capture, confined to the element's region plus the kernel margin:
     1. Blit swapchain -> blur_image at 1/CA_BACKDROP_BLUR_DOWNSAMPLE
        (a 2:1 linear blit is an exact 2x2 box filter; sRGB formats on both
        sides, so filtering and every later pass run in linear light).
     2. Horizontal pass: blur_image -> blur_temp (rows widened by margin).
     3. Vertical pass:   blur_temp  -> blur_image (element region only).
   The composite samples blur_image at gl_FragCoord * uv_scale, so it is
   exact under any paint transform. */

#include "blur.h"
#include "image.h"
#include "shader.h"
#include <math.h>
#include <stdio.h>

#define CA_BACKDROP_BLUR_DOWNSAMPLE 2u
/* Kernel radius cap in downsampled texels (3 sigma of a 64 physical-px
   blur at the default downsample); larger sigmas are truncated and
   renormalised rather than growing the per-fragment loop unbounded. */
#define CA_BACKDROP_BLUR_MAX_RADIUS 96

/**
 * Returns the downsampled extent for one swapchain axis.
 *
 * @param extent Swapchain extent in physical pixels.
 */
static uint32_t blur_extent(uint32_t extent)
{
    return (extent + CA_BACKDROP_BLUR_DOWNSAMPLE - 1u) / CA_BACKDROP_BLUR_DOWNSAMPLE;
}

/**
 * Finds a memory type index satisfying both the resource and property masks.
 *
 * @param gpu   Physical device to query.
 * @param bits  Allowed memory type bits from VkMemoryRequirements.
 * @param props Required property flags.
 * @return Memory type index, or UINT32_MAX if none matches.
 */
static uint32_t find_mem_type(VkPhysicalDevice gpu, uint32_t bits,
                              VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/**
 * Records a single-image layout transition / memory dependency.
 *
 * @param cmd    Command buffer being recorded.
 * @param image  Image to transition.
 * @param old_l  Current layout (UNDEFINED discards contents).
 * @param new_l  Target layout.
 * @param src_s  Source stage mask.
 * @param src_a  Source access mask.
 * @param dst_s  Destination stage mask.
 * @param dst_a  Destination access mask.
 */
static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_l, VkImageLayout new_l,
                          VkPipelineStageFlags2 src_s, VkAccessFlags2 src_a,
                          VkPipelineStageFlags2 dst_s, VkAccessFlags2 dst_a)
{
    VkImageMemoryBarrier2 b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src_s, .srcAccessMask = src_a,
        .dstStageMask        = dst_s, .dstAccessMask = dst_a,
        .oldLayout           = old_l, .newLayout     = new_l,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

/* ---- Shaders ---- */

/* Fullscreen quad over the whole scratch image; the pass's scissor limits
   the fragments actually shaded to the capture region. */
static const char *BLUR_VERT_GLSL =
    "#version 450\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "    const vec2 pos[6] = vec2[](\n"
    "        vec2(-1,-1), vec2(1,-1), vec2(-1,1),\n"
    "        vec2(1,-1),  vec2(1,1),  vec2(-1,1));\n"
    "    const vec2 uv[6] = vec2[](\n"
    "        vec2(0,0), vec2(1,0), vec2(0,1),\n"
    "        vec2(1,0), vec2(1,1), vec2(0,1));\n"
    "    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);\n"
    "    v_uv        = uv[gl_VertexIndex];\n"
    "}\n";

/* One separable Gaussian pass. Adjacent taps are merged into a single
   bilinear fetch at their weighted centroid, halving the fetch count
   without changing the kernel. */
static const char *BLUR_FRAG_GLSL =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src_tex;\n"
    "layout(push_constant) uniform PC {\n"
    "    vec2  texel_size;\n"
    "    float sigma;\n"
    "    int   radius;\n"
    "    int   direction;\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "void main() {\n"
    "    vec4 acc = texture(src_tex, v_uv);\n"
    "    if (pc.radius <= 0) { out_color = acc; return; }\n"
    "    vec2 dir = (pc.direction == 0) ? vec2(pc.texel_size.x, 0.0)\n"
    "                                   : vec2(0.0, pc.texel_size.y);\n"
    "    float k = -0.5 / (pc.sigma * pc.sigma);\n"
    "    float wsum = 1.0;\n"
    "    for (int i = 1; i <= pc.radius; i += 2) {\n"
    "        float i0 = float(i);\n"
    "        float i1 = float(i + 1);\n"
    "        float w0 = exp(i0 * i0 * k);\n"
    "        float w1 = (i + 1 <= pc.radius) ? exp(i1 * i1 * k) : 0.0;\n"
    "        float w  = w0 + w1;\n"
    "        float o  = (i0 * w0 + i1 * w1) / w;\n"
    "        acc  += (texture(src_tex, v_uv + dir * o) +\n"
    "                 texture(src_tex, v_uv - dir * o)) * w;\n"
    "        wsum += 2.0 * w;\n"
    "    }\n"
    "    out_color = acc / wsum;\n"
    "}\n";

/* Composite vertex stage: same instance record and transform as the image
   pipeline (Ca_TextInstance), with uv.xy carrying the fragcoord->UV scale. */
static const char *BACKDROP_VERT_GLSL =
    "#version 450\n"
    "struct ImageData {\n"
    "    vec2 pos;\n"
    "    vec2 size;\n"
    "    vec4 uv;\n"
    "    vec4 color;\n"
    "    vec2 viewport;\n"
    "    vec2 xf_ab;\n"
    "    vec2 xf_cd;\n"
    "    vec2 corner_01;\n"
    "    vec2 corner_23;\n"
    "    vec2 edge_aa_scale_pad;\n"
    "    vec2 _pad1[4];\n"
    "};\n"
    "layout(std430, set = 0, binding = 0) readonly buffer SSB {\n"
    "    ImageData data[];\n"
    "} ssb;\n"
    "layout(location = 0) flat out vec2 v_uv_scale;\n"
    "layout(location = 1) out vec2 v_local;\n"
    "layout(location = 2) flat out vec2 v_size;\n"
    "layout(location = 3) flat out vec4 v_corner_radii;\n"
    "layout(location = 4) flat out float v_edge_aa_scale;\n"
    "void main() {\n"
    "    ImageData d = ssb.data[gl_InstanceIndex];\n"
    "    const vec2 offsets[6] = vec2[6](\n"
    "        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));\n"
    "    vec2 off = offsets[gl_VertexIndex];\n"
    "    vec2 pixel = d.pos + mat2(d.xf_ab, d.xf_cd) * (off * d.size);\n"
    "    gl_Position = vec4((pixel / d.viewport) * 2.0 - 1.0, 0.0, 1.0);\n"
    "    v_uv_scale = d.uv.xy;\n"
    "    v_local = off * d.size;\n"
    "    v_size = d.size;\n"
    "    v_corner_radii = vec4(d.corner_01, d.corner_23);\n"
    "    v_edge_aa_scale = d.edge_aa_scale_pad.x;\n"
    "}\n";

/* Composite fragment stage: the blurred backdrop clipped to the element's
   rounded border box, with the same 1.75-physical-px edge ramp as
   IMAGE_FRAG_GLSL. Alpha is coverage only — the captured swapchain alpha
   channel carries no meaning for the final composite. */
static const char *BACKDROP_FRAG_GLSL =
    "#version 450\n"
    "layout(set = 1, binding = 0) uniform sampler2D tex;\n"
    "layout(location = 0) flat in vec2 v_uv_scale;\n"
    "layout(location = 1) in vec2 v_local;\n"
    "layout(location = 2) flat in vec2 v_size;\n"
    "layout(location = 3) flat in vec4 v_corner_radii;\n"
    "layout(location = 4) flat in float v_edge_aa_scale;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "float roundedBoxSDF(vec2 p, vec2 b, vec4 r) {\n"
    "    float radius = (p.x > 0.0) ?\n"
    "        ((p.y > 0.0) ? r.z : r.y) :\n"
    "        ((p.y > 0.0) ? r.w : r.x);\n"
    "    radius = clamp(radius, 0.0, min(b.x, b.y));\n"
    "    vec2 d = abs(p) - b + vec2(radius);\n"
    "    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - radius;\n"
    "}\n"
    "void main() {\n"
    "    float aa_hw = 0.875 / max(v_edge_aa_scale, 1e-4);\n"
    "    vec2 half_size = v_size * 0.5;\n"
    "    float dist = roundedBoxSDF(v_local - half_size, half_size, v_corner_radii);\n"
    "    float coverage = 1.0 - smoothstep(-aa_hw, aa_hw, dist);\n"
    "    if (coverage < 0.001) discard;\n"
    "    vec3 c = texture(tex, gl_FragCoord.xy * v_uv_scale).rgb;\n"
    "    out_color = vec4(c, coverage);\n"
    "}\n";

/* ---- Pipelines ---- */

typedef struct {
    float   texel_size[2];
    float   sigma;
    int32_t radius;
    int32_t direction;
} BlurPC;

/**
 * Builds one graphics pipeline for dynamic rendering into `color_format`.
 *
 * @param inst         Instance owning the device.
 * @param vert_src     Vertex GLSL source.
 * @param frag_src     Fragment GLSL source.
 * @param layout       Pipeline layout.
 * @param color_format Color attachment format.
 * @param blend        Whether to enable premultiplied-by-alpha src-over blending.
 * @param out          Receives the pipeline on success.
 * @return true on success.
 */
static bool create_pipeline(Ca_Instance *inst, const char *vert_src,
                            const char *frag_src, VkPipelineLayout layout,
                            VkFormat color_format, bool blend,
                            VkPipeline *out)
{
    VkShaderModule vert = ca_shader_compile(inst, vert_src, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst, frag_src, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        fprintf(stderr, "[blur] shader compile failed\n");
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, vert, NULL);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(inst->vk_device, frag, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vert_in = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo input_asm = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo msaa = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_att };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states };
    VkPipelineRenderingCreateInfo rendering_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format };

    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering_ci,
        .stageCount          = 2, .pStages = stages,
        .pVertexInputState   = &vert_in,
        .pInputAssemblyState = &input_asm,
        .pViewportState      = &vp_state,
        .pRasterizationState = &raster,
        .pMultisampleState   = &msaa,
        .pColorBlendState    = &blend_state,
        .pDynamicState       = &dyn,
        .layout              = layout,
        .renderPass          = VK_NULL_HANDLE,
    };
    VkResult r = vkCreateGraphicsPipelines(inst->vk_device, VK_NULL_HANDLE, 1,
                                           &gp_ci, NULL, out);
    vkDestroyShaderModule(inst->vk_device, vert, NULL);
    vkDestroyShaderModule(inst->vk_device, frag, NULL);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreateGraphicsPipelines failed: %d\n", r);
        *out = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool ca_blur_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    if (inst->text_pipeline.layout == VK_NULL_HANDLE) return false;

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(BlurPC),
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &inst->text_pipeline.desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc_range,
    };
    if (vkCreatePipelineLayout(inst->vk_device, &layout_ci, NULL,
                               &inst->blur_pipeline_layout) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreatePipelineLayout failed\n");
        inst->blur_pipeline_layout = VK_NULL_HANDLE;
        return false;
    }

    /* Scratch images are sRGB like the swapchain, so the blit decodes to
       linear, both passes filter in linear light, and the composite's
       sample returns linear RGB for the sRGB swapchain to re-encode. */
    if (!create_pipeline(inst, BLUR_VERT_GLSL, BLUR_FRAG_GLSL,
                         inst->blur_pipeline_layout, VK_FORMAT_R8G8B8A8_SRGB,
                         false, &inst->blur_pipeline) ||
        !create_pipeline(inst, BACKDROP_VERT_GLSL, BACKDROP_FRAG_GLSL,
                         inst->text_pipeline.layout, color_format,
                         true, &inst->backdrop_pipeline)) {
        ca_blur_pipeline_destroy(inst);
        return false;
    }
    return true;
}

void ca_blur_pipeline_destroy(Ca_Instance *inst)
{
    if (inst->backdrop_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(inst->vk_device, inst->backdrop_pipeline, NULL);
        inst->backdrop_pipeline = VK_NULL_HANDLE;
    }
    if (inst->blur_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(inst->vk_device, inst->blur_pipeline, NULL);
        inst->blur_pipeline = VK_NULL_HANDLE;
    }
    if (inst->blur_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(inst->vk_device, inst->blur_pipeline_layout, NULL);
        inst->blur_pipeline_layout = VK_NULL_HANDLE;
    }
}

/* ---- Per-window scratch images ---- */

/**
 * Creates one device-local sRGB scratch image and its view.
 *
 * @param inst Instance owning the device.
 * @param img  Receives the image.
 * @param mem  Receives the backing memory.
 * @param view Receives the image view.
 * @param w    Width in texels.
 * @param h    Height in texels.
 * @return true on success; partial handles are left for the caller's destroy.
 */
static bool create_blur_image(Ca_Instance *inst,
                              VkImage *img, VkDeviceMemory *mem, VkImageView *view,
                              uint32_t w, uint32_t h)
{
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_SRGB,
        .extent        = { w, h, 1 },
        .mipLevels     = 1, .arrayLayers = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(inst->vk_device, &img_ci, NULL, img) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreateImage failed\n");
        *img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(inst->vk_device, *img, &req);
    uint32_t mi = find_mem_type(inst->vk_gpu, req.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi == UINT32_MAX) {
        fprintf(stderr, "[blur] no device-local memory type\n");
        return false;
    }
    VkMemoryAllocateInfo mem_ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mi,
    };
    if (vkAllocateMemory(inst->vk_device, &mem_ai, NULL, mem) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkAllocateMemory failed\n");
        *mem = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(inst->vk_device, *img, *mem, 0) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkBindImageMemory failed\n");
        return false;
    }
    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = *img,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(inst->vk_device, &view_ci, NULL, view) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreateImageView failed\n");
        *view = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/**
 * Allocates and writes a combined-image-sampler descriptor set for `view`.
 *
 * @param inst     Instance owning the descriptor pools.
 * @param view     Image view to bind (SHADER_READ_ONLY_OPTIMAL when sampled).
 * @param sampler  Sampler to bind.
 * @param out_set  Receives the descriptor set.
 * @param out_pool Receives the pool the set came from.
 * @return true on success.
 */
static bool alloc_desc_set(Ca_Instance *inst, VkImageView view, VkSampler sampler,
                           VkDescriptorSet *out_set, VkDescriptorPool *out_pool)
{
    if (!ca_image_descriptor_allocate(inst, inst->text_pipeline.desc_layout,
                                      out_set, out_pool)) {
        fprintf(stderr, "[blur] descriptor set alloc failed\n");
        return false;
    }
    VkDescriptorImageInfo img_info = {
        .sampler     = sampler,
        .imageView   = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wr = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = *out_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &img_info,
    };
    vkUpdateDescriptorSets(inst->vk_device, 1, &wr, 0, NULL);
    return true;
}

bool ca_blur_window_create(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height)
{
    if (inst->blur_pipeline == VK_NULL_HANDLE) return true;
    if (width == 0 || height == 0) return false;
    const uint32_t blur_width  = blur_extent(width);
    const uint32_t blur_height = blur_extent(height);

    if (win->blur_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samp_ci = {
            .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter    = VK_FILTER_LINEAR,
            .minFilter    = VK_FILTER_LINEAR,
            .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod       = 0.0f,
        };
        if (vkCreateSampler(inst->vk_device, &samp_ci, NULL,
                            &win->blur_sampler) != VK_SUCCESS) {
            fprintf(stderr, "[blur] vkCreateSampler failed\n");
            win->blur_sampler = VK_NULL_HANDLE;
            return false;
        }
    }

    if (!create_blur_image(inst, &win->blur_image, &win->blur_memory,
                           &win->blur_view, blur_width, blur_height) ||
        !create_blur_image(inst, &win->blur_temp, &win->blur_temp_memory,
                           &win->blur_temp_view, blur_width, blur_height) ||
        !alloc_desc_set(inst, win->blur_view, win->blur_sampler,
                        &win->blur_desc_set, &win->blur_desc_pool) ||
        !alloc_desc_set(inst, win->blur_temp_view, win->blur_sampler,
                        &win->blur_temp_desc_set, &win->blur_temp_desc_pool)) {
        ca_blur_window_destroy(inst, win);
        return false;
    }

    win->blur_image_w = blur_width;
    win->blur_image_h = blur_height;
    return true;
}

/**
 * Releases the scratch images and descriptor sets, keeping the sampler.
 *
 * @param inst Instance owning the device.
 * @param win  Window whose images are released.
 */
static void destroy_blur_images(Ca_Instance *inst, Ca_Window *win)
{
    if (win->blur_desc_set != VK_NULL_HANDLE) {
        ca_image_descriptor_free(inst, win->blur_desc_pool, win->blur_desc_set);
        win->blur_desc_set  = VK_NULL_HANDLE;
        win->blur_desc_pool = VK_NULL_HANDLE;
    }
    if (win->blur_temp_desc_set != VK_NULL_HANDLE) {
        ca_image_descriptor_free(inst, win->blur_temp_desc_pool,
                                 win->blur_temp_desc_set);
        win->blur_temp_desc_set  = VK_NULL_HANDLE;
        win->blur_temp_desc_pool = VK_NULL_HANDLE;
    }
    if (win->blur_view != VK_NULL_HANDLE) {
        vkDestroyImageView(inst->vk_device, win->blur_view, NULL);
        win->blur_view = VK_NULL_HANDLE;
    }
    if (win->blur_image != VK_NULL_HANDLE) {
        vkDestroyImage(inst->vk_device, win->blur_image, NULL);
        win->blur_image = VK_NULL_HANDLE;
    }
    if (win->blur_memory != VK_NULL_HANDLE) {
        vkFreeMemory(inst->vk_device, win->blur_memory, NULL);
        win->blur_memory = VK_NULL_HANDLE;
    }
    if (win->blur_temp_view != VK_NULL_HANDLE) {
        vkDestroyImageView(inst->vk_device, win->blur_temp_view, NULL);
        win->blur_temp_view = VK_NULL_HANDLE;
    }
    if (win->blur_temp != VK_NULL_HANDLE) {
        vkDestroyImage(inst->vk_device, win->blur_temp, NULL);
        win->blur_temp = VK_NULL_HANDLE;
    }
    if (win->blur_temp_memory != VK_NULL_HANDLE) {
        vkFreeMemory(inst->vk_device, win->blur_temp_memory, NULL);
        win->blur_temp_memory = VK_NULL_HANDLE;
    }
    win->blur_image_w = 0;
    win->blur_image_h = 0;
}

void ca_blur_window_destroy(Ca_Instance *inst, Ca_Window *win)
{
    if (!win) return;
    vkDeviceWaitIdle(inst->vk_device);
    destroy_blur_images(inst, win);
    if (win->blur_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(inst->vk_device, win->blur_sampler, NULL);
        win->blur_sampler = VK_NULL_HANDLE;
    }
}

bool ca_blur_window_resize(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height)
{
    if (win->blur_image != VK_NULL_HANDLE &&
        win->blur_image_w == blur_extent(width) &&
        win->blur_image_h == blur_extent(height)) return true;
    vkDeviceWaitIdle(inst->vk_device);
    destroy_blur_images(inst, win);
    return ca_blur_window_create(inst, win, width, height);
}

/* ---- Per-element capture ---- */

/* Half-open texel rectangle in scratch-image space. */
typedef struct { int32_t x0, y0, x1, y1; } BlurBox;

/**
 * Grows `b` by (mx, my) on each side and clamps it to [0,w)x[0,h).
 *
 * @param b  Box to grow.
 * @param mx Horizontal margin in texels.
 * @param my Vertical margin in texels.
 * @param w  Image width.
 * @param h  Image height.
 * @return The grown, clamped box.
 */
static BlurBox box_grow(BlurBox b, int32_t mx, int32_t my, int32_t w, int32_t h)
{
    BlurBox r = { b.x0 - mx, b.y0 - my, b.x1 + mx, b.y1 + my };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > w) r.x1 = w;
    if (r.y1 > h) r.y1 = h;
    return r;
}

/**
 * Records one separable Gaussian pass writing `dst_box` of `dst_view`.
 *
 * @param inst     Instance owning the blur pipeline.
 * @param win      Window owning the scratch images.
 * @param cmd      Command buffer being recorded.
 * @param src_set  Descriptor set of the image read by this pass.
 * @param dst_view Color attachment written by this pass.
 * @param dst_box  Texel region to write.
 * @param pc       Push constants (direction already set).
 */
static void blur_pass(Ca_Instance *inst, Ca_Window *win, VkCommandBuffer cmd,
                      VkDescriptorSet src_set, VkImageView dst_view,
                      BlurBox dst_box, const BlurPC *pc)
{
    VkRect2D area = {
        { dst_box.x0, dst_box.y0 },
        { (uint32_t)(dst_box.x1 - dst_box.x0), (uint32_t)(dst_box.y1 - dst_box.y0) },
    };
    VkViewport vp = { 0.0f, 0.0f, (float)win->blur_image_w,
                      (float)win->blur_image_h, 0.0f, 1.0f };
    VkRenderingAttachmentInfo attach = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = dst_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = area,
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &attach,
    };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->blur_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inst->blur_pipeline_layout, 0, 1, &src_set, 0, NULL);
    vkCmdPushConstants(cmd, inst->blur_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(*pc), pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

bool ca_blur_capture_region(Ca_Instance *inst, Ca_Window *win,
                            VkCommandBuffer cmd, VkImage swapchain_image,
                            VkExtent2D sc_extent, VkRect2D region,
                            float sigma_px, float uv_scale[2])
{
    if (inst->blur_pipeline == VK_NULL_HANDLE ||
        inst->backdrop_pipeline == VK_NULL_HANDLE ||
        win->blur_image == VK_NULL_HANDLE || win->blur_temp == VK_NULL_HANDLE)
        return false;
    if (win->blur_image_w != blur_extent(sc_extent.width) ||
        win->blur_image_h != blur_extent(sc_extent.height))
        return false;
    if (region.extent.width == 0 || region.extent.height == 0 ||
        !(sigma_px >= 0.0f) || !isfinite(sigma_px))
        return false;

    const int32_t ds = (int32_t)CA_BACKDROP_BLUR_DOWNSAMPLE;
    const int32_t bw = (int32_t)win->blur_image_w;
    const int32_t bh = (int32_t)win->blur_image_h;

    const float sigma = sigma_px / (float)ds;
    int32_t radius = sigma < 0.25f ? 0 : (int32_t)ceilf(sigma * 3.0f);
    if (radius > CA_BACKDROP_BLUR_MAX_RADIUS) radius = CA_BACKDROP_BLUR_MAX_RADIUS;

    /* Output box: the composite region in texels, rounded outward, plus one
       texel so the composite's bilinear footprint never leaves it. */
    BlurBox out = {
        region.offset.x / ds,
        region.offset.y / ds,
        (region.offset.x + (int32_t)region.extent.width  + ds - 1) / ds,
        (region.offset.y + (int32_t)region.extent.height + ds - 1) / ds,
    };
    out = box_grow(out, 1, 1, bw, bh);
    if (out.x1 <= out.x0 || out.y1 <= out.y0) return false;

    /* Taps reach radius+1 texels (merged-pair centroid) plus one more for
       the bilinear footprint. */
    const int32_t margin = radius + 2;
    const BlurBox h_box   = box_grow(out, 0, margin, bw, bh);
    const BlurBox cap_box = box_grow(out, margin, margin, bw, bh);

    int32_t src_x1 = cap_box.x1 * ds, src_y1 = cap_box.y1 * ds;
    if (src_x1 > (int32_t)sc_extent.width)  src_x1 = (int32_t)sc_extent.width;
    if (src_y1 > (int32_t)sc_extent.height) src_y1 = (int32_t)sc_extent.height;
    if (src_x1 <= cap_box.x0 * ds || src_y1 <= cap_box.y0 * ds) return false;

    /* Previous element's composite (fragment read of blur_image) and V pass
       (fragment read of blur_temp) in this same command buffer must finish
       before these images are overwritten. */
    image_barrier(cmd, swapchain_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit2 blit_region = {
        .sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets     = { { cap_box.x0 * ds, cap_box.y0 * ds, 0 }, { src_x1, src_y1, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets     = { { cap_box.x0, cap_box.y0, 0 }, { cap_box.x1, cap_box.y1, 1 } },
    };
    VkBlitImageInfo2 blit_info = {
        .sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage       = swapchain_image,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage       = win->blur_image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount    = 1,
        .pRegions       = &blit_region,
        .filter         = VK_FILTER_LINEAR,
    };
    vkCmdBlitImage2(cmd, &blit_info);

    image_barrier(cmd, swapchain_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cmd, win->blur_temp,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    BlurPC pc = {
        .texel_size = { 1.0f / (float)bw, 1.0f / (float)bh },
        .sigma      = sigma,
        .radius     = radius,
        .direction  = 0,
    };
    blur_pass(inst, win, cmd, win->blur_desc_set, win->blur_temp_view, h_box, &pc);

    image_barrier(cmd, win->blur_temp,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    pc.direction = 1;
    blur_pass(inst, win, cmd, win->blur_temp_desc_set, win->blur_view, out, &pc);

    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    uv_scale[0] = 1.0f / (float)(bw * ds);
    uv_scale[1] = 1.0f / (float)(bh * ds);
    return true;
}
