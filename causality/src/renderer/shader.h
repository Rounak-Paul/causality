// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* shader.h — runtime GLSL → SPIR-V compilation via libshaderc, with an
   on-disk cache (see shader_cache.h) keyed by shader content so a
   byte-identical source skips shaderc entirely on subsequent launches. */
#pragma once

#include "ca_internal.h"

/* Compile a GLSL source string to a VkShaderModule.
   Serves a cached SPIR-V blob when instance has a shader cache
   directory configured (Ca_InstanceDesc::shader_cache_dir) and one
   matches this exact source + stage; otherwise compiles via shaderc and
   best-effort writes the result back to the cache.
   Returns VK_NULL_HANDLE and prints the error on failure. */
VkShaderModule ca_shader_compile(Ca_Instance          *instance,
                                 const char           *glsl_source,
                                 VkShaderStageFlagBits stage);
