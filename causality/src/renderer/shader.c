/* shader.c — compile GLSL string literals to SPIR-V at runtime using libshaderc
   (libshaderc_combined ships with the LunarG Vulkan SDK)               */
#include "shader.h"

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

VkShaderModule ca_shader_compile(VkDevice              device,
                                 const char           *glsl_source,
                                 VkShaderStageFlagBits stage)
{
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

    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shaderc_result_get_length(result),
        .pCode    = (const uint32_t *)shaderc_result_get_bytes(result),
    };

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, NULL, &mod) != VK_SUCCESS) {
        fprintf(stderr, "[shader] vkCreateShaderModule failed\n");
        mod = VK_NULL_HANDLE;
    }

    shaderc_result_release(result);
    return mod;
}
