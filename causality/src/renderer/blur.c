// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* blur.c — backdrop-filter: blur() implementation.

   Strategy (all within the existing command buffer, before the UI render pass):

   1. Transition swapchain image → TRANSFER_SRC
   2. Copy (vkCmdBlitImage) swapchain → blur_temp    (H-blur source)
   3. Transition blur_temp → COLOR_ATTACHMENT, run H-blur pass → blur_image
   4. Transition blur_image → COLOR_ATTACHMENT, run V-blur pass → blur_temp
   5. Transition blur_temp → SHADER_READ_ONLY (this is the final blurred result)

   Wait — to keep the dual-pass simple and to avoid needing blur_image twice,
   we do:
     Pass A: Read blur_temp (snapshot), write blur_image   (H pass)
     Pass B: Read blur_image, write blur_temp              (V pass)
   Then blur_temp holds the final result and we sample from it.

   To keep descriptor sets stable we always sample from blur_desc_set which
   points to blur_image.  So instead:
     Pass A (H): Read snapshot copy in blur_temp → write blur_image
     Pass B (V): Read blur_image → write blur_temp, then blit blur_temp → blur_image

   That's three image transitions too many.  Simplest correct approach:

   We use blur_image as the "source snapshot copy" and blur_temp as scratch.
     1. Copy swapchain → blur_image  (as a copy/blit)
     2. H-pass: read blur_image, write blur_temp
     3. V-pass: read blur_temp, write blur_image
     4. blur_image is now the fully blurred result (sampled during UI pass)

   blur_desc_set → blur_image  (final result, sampled by backdrop quads).
   blur_temp_desc_set → blur_temp (intermediate, only read by V-pass shader).

   The two pipelines (H, V) share a push-constant: blur radius in pixels.
   They use the same fragment shader with a direction flag.
   The composite pipeline is just the existing image_pipeline (image textured
   quad with a rounded-rect clip applied in the fragment shader).
*/

#include "blur.h"
#include "pipeline.h"
#include "image.h"
#include "shader.h"
#include <string.h>
#include <stdio.h>

#define CA_BACKDROP_BLUR_SCALE_DIVISOR 4u

/* Return the reduced backdrop texture dimension for one swapchain axis. */
static uint32_t blur_extent(uint32_t extent)
{
    return (extent + CA_BACKDROP_BLUR_SCALE_DIVISOR - 1u) /
           CA_BACKDROP_BLUR_SCALE_DIVISOR;
}

/* ---- Memory helper ---- */

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

static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_l, VkImageLayout new_l,
                          VkPipelineStageFlags2 src_s, VkAccessFlags2 src_a,
                          VkPipelineStageFlags2 dst_s, VkAccessFlags2 dst_a)
{
    VkImageMemoryBarrier2 b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src_s, .srcAccessMask       = src_a,
        .dstStageMask        = dst_s, .dstAccessMask       = dst_a,
        .oldLayout           = old_l, .newLayout           = new_l,
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

/* ---- Blur shaders ---- */

/* Vertex: procedural fullscreen quad; outputs UV in [0,1]. */
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

/* Fragment: fixed-cost separable blur over a downsampled backdrop cache.
   push_constant.direction: 0 = horizontal, 1 = vertical.
   push_constant.blur_radius: CSS radius in cache pixels.                  */
static const char *BLUR_FRAG_GLSL =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src_tex;\n"
    "layout(push_constant) uniform PC {\n"
    "    vec2  texel_size;    /* 1/width, 1/height */\n"
    "    float blur_radius;   /* CSS radius in cache pixels */\n"
    "    int   direction;     /* 0=H, 1=V          */\n"
    "} pc;\n"
    "layout(location = 0) in  vec2 v_uv;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "void main() {\n"
    "    vec2 dir = (pc.direction == 0) ? vec2(pc.texel_size.x, 0.0)\n"
    "                                     : vec2(0.0, pc.texel_size.y);\n"
    "    float spread = max(pc.blur_radius * 0.4, 0.5);\n"
    "    float w[9] = float[](0.0542,0.0816,0.1065,0.1213,0.1283,0.1213,0.1065,0.0816,0.0542);\n"
    "    vec4 color = vec4(0.0);\n"
    "    for (int i = 0; i < 9; ++i) color += texture(src_tex, v_uv + dir * (float(i) - 4.0) * spread) * w[i];\n"
    "    out_color = color;\n"
    "}\n";

/* ---- Blur pipeline ---- */

typedef struct {
    float texel_size[2];
    float blur_radius;
    int   direction;
} BlurPC;

bool ca_blur_pipeline_create(Ca_Instance *inst, VkFormat color_format)
{
    /* Push constant range */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(BlurPC),
    };

    /* Pipeline layout: set 0 = sampler only (reuse text_pipeline.desc_layout) */
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &inst->text_pipeline.desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pc_range,
    };
    VkPipelineLayout blur_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(inst->vk_device, &layout_ci, NULL, &blur_layout) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreatePipelineLayout failed\n");
        return false;
    }

    VkShaderModule vert = ca_shader_compile(inst, BLUR_VERT_GLSL, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = ca_shader_compile(inst, BLUR_FRAG_GLSL, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!vert || !frag) {
        fprintf(stderr, "[blur] shader compile failed\n");
        if (vert) vkDestroyShaderModule(inst->vk_device, vert, NULL);
        if (frag) vkDestroyShaderModule(inst->vk_device, frag, NULL);
        vkDestroyPipelineLayout(inst->vk_device, blur_layout, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
    };

    VkPipelineVertexInputStateCreateInfo   vert_in  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo input_asm = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo      vp_state = {
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
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_att };
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states };
    /* Use R8G8B8A8_UNORM internally for the blur images (not sRGB — we want linear sampling) */
    VkFormat blur_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &blur_fmt };

    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering_ci,
        .stageCount          = 2, .pStages = stages,
        .pVertexInputState   = &vert_in,
        .pInputAssemblyState = &input_asm,
        .pViewportState      = &vp_state,
        .pRasterizationState = &raster,
        .pMultisampleState   = &msaa,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dyn,
        .layout              = blur_layout,
        .renderPass          = VK_NULL_HANDLE,
    };

    /* We create H and V pipelines with the same GLSL (direction is a push constant).
       So we just create one pipeline and store it in both slots; direction is set at draw time. */
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(inst->vk_device, VK_NULL_HANDLE, 1, &gp_ci, NULL, &pipeline);
    vkDestroyShaderModule(inst->vk_device, vert, NULL);
    vkDestroyShaderModule(inst->vk_device, frag, NULL);

    if (r != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreateGraphicsPipelines failed: %d\n", r);
        vkDestroyPipelineLayout(inst->vk_device, blur_layout, NULL);
        return false;
    }

    /* Store pipeline in both H and V slots (same code, direction differs at push time) */
    inst->blur_h_pipeline      = pipeline;
    inst->blur_v_pipeline      = pipeline; /* same VkPipeline handle */
    inst->blur_pipeline_layout = blur_layout;

    printf("[blur] blur pipeline created\n");
    return true;
}

void ca_blur_pipeline_destroy(Ca_Instance *inst)
{
    if (inst->blur_h_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(inst->vk_device, inst->blur_h_pipeline, NULL);
        inst->blur_h_pipeline = VK_NULL_HANDLE;
        inst->blur_v_pipeline = VK_NULL_HANDLE; /* same handle */
    }
    if (inst->blur_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(inst->vk_device, inst->blur_pipeline_layout, NULL);
        inst->blur_pipeline_layout = VK_NULL_HANDLE;
    }
}

/* ---- Per-window blur image lifecycle ---- */

static bool create_blur_image(Ca_Instance *inst,
                              VkImage *img, VkDeviceMemory *mem, VkImageView *view,
                              uint32_t w, uint32_t h)
{
    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
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
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(inst->vk_device, *img, &req);
    uint32_t mi = find_mem_type(inst->vk_gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi == UINT32_MAX) { fprintf(stderr, "[blur] no memory type\n"); return false; }
    VkMemoryAllocateInfo mem_ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = mi,
    };
    if (vkAllocateMemory(inst->vk_device, &mem_ai, NULL, mem) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkAllocateMemory failed\n");
        return false;
    }
    vkBindImageMemory(inst->vk_device, *img, *mem, 0);

    VkImageViewCreateInfo view_ci = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = *img,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(inst->vk_device, &view_ci, NULL, view) != VK_SUCCESS) {
        fprintf(stderr, "[blur] vkCreateImageView failed\n");
        return false;
    }
    return true;
}

static bool alloc_desc_set(Ca_Instance *inst, VkImageView view, VkSampler sampler,
                           VkDescriptorSet *out_set,
                           VkDescriptorPool *out_pool)
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
                           uint32_t width, uint32_t height, VkFormat format)
{
    (void)format; /* we always use UNORM internally */

    if (!inst->blur_h_pipeline) return true; /* pipeline not created yet — lazy */
    if (width == 0 || height == 0) return false;
    const uint32_t blur_width = blur_extent(width);
    const uint32_t blur_height = blur_extent(height);

    /* Sampler (linear, clamp) */
    VkSamplerCreateInfo samp_ci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = VK_LOD_CLAMP_NONE,
    };
    if (!win->blur_sampler) {
        if (vkCreateSampler(inst->vk_device, &samp_ci, NULL, &win->blur_sampler) != VK_SUCCESS) {
            fprintf(stderr, "[blur] vkCreateSampler failed\n");
            return false;
        }
    }

    if (!create_blur_image(inst, &win->blur_image, &win->blur_memory, &win->blur_view,
                           blur_width, blur_height))
        return false;
    if (!create_blur_image(inst, &win->blur_temp, &win->blur_temp_memory, &win->blur_temp_view,
                           blur_width, blur_height))
        return false;

    if (!alloc_desc_set(inst, win->blur_view, win->blur_sampler,
                        &win->blur_desc_set, &win->blur_desc_pool))
        return false;
    if (!alloc_desc_set(inst, win->blur_temp_view, win->blur_sampler,
                        &win->blur_temp_desc_set, &win->blur_temp_desc_pool))
        return false;

    win->blur_image_w     = blur_width;
    win->blur_image_h     = blur_height;
    win->blur_image_valid = false;

    printf("[blur] window blur images created (%ux%u for %ux%u swapchain)\n",
           blur_width, blur_height, width, height);
    return true;
}

void ca_blur_window_destroy(Ca_Instance *inst, Ca_Window *win)
{
    if (!win) return;
    vkDeviceWaitIdle(inst->vk_device);

    if (win->blur_desc_set != VK_NULL_HANDLE) {
        ca_image_descriptor_free(inst, win->blur_desc_pool, win->blur_desc_set);
        win->blur_desc_set = VK_NULL_HANDLE;
        win->blur_desc_pool = VK_NULL_HANDLE;
    }
    if (win->blur_temp_desc_set != VK_NULL_HANDLE) {
        ca_image_descriptor_free(inst, win->blur_temp_desc_pool,
                                 win->blur_temp_desc_set);
        win->blur_temp_desc_set = VK_NULL_HANDLE;
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
    if (win->blur_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(inst->vk_device, win->blur_sampler, NULL);
        win->blur_sampler = VK_NULL_HANDLE;
    }
    win->blur_image_w     = 0;
    win->blur_image_h     = 0;
    win->blur_image_valid = false;
}

bool ca_blur_window_resize(Ca_Instance *inst, Ca_Window *win,
                           uint32_t width, uint32_t height, VkFormat format)
{
    if (win->blur_image_w == blur_extent(width) &&
        win->blur_image_h == blur_extent(height)) return true;
    /* Destroy and recreate; keep the sampler across resizes */
    VkSampler saved_sampler = win->blur_sampler;
    win->blur_sampler = VK_NULL_HANDLE; /* prevent ca_blur_window_destroy from destroying it */
    ca_blur_window_destroy(inst, win);
    win->blur_sampler = saved_sampler;
    return ca_blur_window_create(inst, win, width, height, format);
}

/* ---- Per-frame: capture background and blur ---- */

void ca_blur_capture_and_blur(Ca_Instance *inst, Ca_Window *win,
                              VkCommandBuffer cmd,
                              VkImage swapchain_image,
                              uint32_t sc_width, uint32_t sc_height,
                              float blur_radius)
{
    if (!inst->blur_h_pipeline) return;
    if (!win->blur_image || !win->blur_temp) return;

    /* Ensure blur images are the right size */
    const uint32_t blur_width = blur_extent(sc_width);
    const uint32_t blur_height = blur_extent(sc_height);
    if (win->blur_image_w != blur_width || win->blur_image_h != blur_height) {
        /* Can't resize mid-command-buffer — skip this frame.
           Resize is handled by swapchain recreation code. */
        return;
    }

    VkExtent2D extent = { blur_width, blur_height };
    VkViewport vp = { 0, 0, (float)blur_width, (float)blur_height, 0.0f, 1.0f };
    VkRect2D   scissor = { {0,0}, extent };

    BlurPC pc;
    pc.texel_size[0] = 1.0f / (float)blur_width;
    pc.texel_size[1] = 1.0f / (float)blur_height;
    pc.blur_radius   = blur_radius / (float)CA_BACKDROP_BLUR_SCALE_DIVISOR;

    VkPipelineLayout blur_layout = inst->blur_pipeline_layout;

    /* ---- Step 1: copy swapchain → blur_image (snapshot) ----
       The swapchain image is already in COLOR_ATTACHMENT_OPTIMAL at this point.
       We need to transition it to TRANSFER_SRC, blit, then restore.          */
    image_barrier(cmd, swapchain_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkImageBlit2 blit_region = {
        .sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets     = { {0, 0, 0}, {(int32_t)sc_width, (int32_t)sc_height, 1} },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets     = { {0, 0, 0}, {(int32_t)blur_width, (int32_t)blur_height, 1} },
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

    /* Restore swapchain image to COLOR_ATTACHMENT_OPTIMAL */
    image_barrier(cmd, swapchain_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    /* Transition blur_image to SHADER_READ (source for H-pass) */
    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    /* ---- Step 2: H-pass — read blur_image, write blur_temp ---- */
    image_barrier(cmd, win->blur_temp,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo h_attach = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = win->blur_temp_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo h_ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { {0,0}, extent },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &h_attach,
    };
    vkCmdBeginRendering(cmd, &h_ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->blur_h_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blur_layout,
                            0, 1, &win->blur_desc_set, 0, NULL);
    pc.direction = 0; /* horizontal */
    vkCmdPushConstants(cmd, blur_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlurPC), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);

    /* ---- Step 3: V-pass — read blur_temp, write blur_image ---- */
    image_barrier(cmd, win->blur_temp,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo v_attach = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = win->blur_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo v_ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { {0,0}, extent },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &v_attach,
    };
    vkCmdBeginRendering(cmd, &v_ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inst->blur_v_pipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blur_layout,
                            0, 1, &win->blur_temp_desc_set, 0, NULL);
    pc.direction = 1; /* vertical */
    vkCmdPushConstants(cmd, blur_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlurPC), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);

    /* Transition blur_image to SHADER_READ_ONLY for use during the UI render pass */
    image_barrier(cmd, win->blur_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    win->blur_image_valid = true;
}
