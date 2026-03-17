/* shader.h — runtime GLSL → SPIR-V compilation via libshaderc */
#pragma once

#include "ca_internal.h"

/* Compile a GLSL source string to a VkShaderModule.
   Returns VK_NULL_HANDLE and prints the error on failure. */
VkShaderModule ca_shader_compile(VkDevice              device,
                                 const char           *glsl_source,
                                 VkShaderStageFlagBits stage);
