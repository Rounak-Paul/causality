// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* shader.c — compile GLSL string literals to SPIR-V at runtime using libshaderc
   (libshaderc_combined ships with the LunarG Vulkan SDK), with an
   on-disk cache in front of the compiler — see shader_cache.h. */
#include "shader.h"
#include "shader_cache.h"

#include <shaderc/shaderc.h>
#include <string.h>

static shaderc_shader_kind stage_to_shaderc(VkShaderStageFlagBits stage)
{
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:   return shaderc_glsl_vertex_shader;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return shaderc_glsl_fragment_shader;
    default:                           return shaderc_glsl_infer_from_source;
    }
}

/* Wrap a cached/compiled SPIR-V blob into a VkShaderModule. Shared by
   both the cache-hit and freshly-compiled paths so module creation
   failure is handled identically either way. */
static VkShaderModule ca_shader_make_module(VkDevice device,
                                            const uint32_t *spirv,
                                            size_t byte_size)
{
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = byte_size,
        .pCode    = spirv,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, NULL, &mod) != VK_SUCCESS) {
        fprintf(stderr, "[shader] vkCreateShaderModule failed\n");
        return VK_NULL_HANDLE;
    }
    return mod;
}

VkShaderModule ca_shader_compile(Ca_Instance          *instance,
                                 const char           *glsl_source,
                                 VkShaderStageFlagBits stage)
{
    VkDevice device = ca_gpu_device(instance);

    size_t cached_size = 0u;
    uint32_t *cached = ca_shader_cache_lookup(instance, glsl_source, stage,
                                              &cached_size);
    if (cached) {
        VkShaderModule mod = ca_shader_make_module(device, cached, cached_size);
        CA_FREE(cached);
        if (mod != VK_NULL_HANDLE) return mod;
        /* Cache blob failed to load as a valid module (corrupt file,
           foreign SPIR-V dumped at the same path, etc.) — fall through
           and recompile from source instead of failing the caller. */
    }

    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    if (!compiler) {
        fprintf(stderr, "[shader] shaderc_compiler_initialize failed\n");
        return VK_NULL_HANDLE;
    }

    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts,
        shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(opts,
        shaderc_optimization_level_performance);

    shaderc_compilation_result_t result = shaderc_compile_into_spv(
        compiler,
        glsl_source, strlen(glsl_source),
        stage_to_shaderc(stage),
        "shader", "main",
        opts);

    shaderc_compile_options_release(opts);
    shaderc_compiler_release(compiler);

    if (shaderc_result_get_compilation_status(result) !=
            shaderc_compilation_status_success) {
        fprintf(stderr, "[shader] compile error:\n%s\n",
                shaderc_result_get_error_message(result));
        shaderc_result_release(result);
        return VK_NULL_HANDLE;
    }

    const size_t byte_size = shaderc_result_get_length(result);
    const uint32_t *spirv = (const uint32_t *)shaderc_result_get_bytes(result);
    VkShaderModule mod = ca_shader_make_module(device, spirv, byte_size);
    if (mod != VK_NULL_HANDLE) {
        ca_shader_cache_store(instance, glsl_source, stage,
                              spirv, byte_size / sizeof(uint32_t));
    }

    shaderc_result_release(result);
    return mod;
}
