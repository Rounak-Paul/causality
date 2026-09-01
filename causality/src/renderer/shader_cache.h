// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* shader_cache.h — on-disk cache for compiled SPIR-V, keyed by shader
   source content so a byte-identical GLSL string never pays shaderc's
   compile cost twice across process launches. */
#pragma once

#include "ca_internal.h"

/*
 * Look up a cached SPIR-V blob for the given GLSL source and stage.
 *
 * Returns NULL (and sets *out_size to 0) on any miss: no cache directory
 * configured on the instance, no file for this content hash, or a read
 * failure — callers always fall back to compiling. On a hit, the
 * returned buffer is heap-allocated and CA_FREE-owned by the caller.
 *
 * instance  Owning Ca_Instance; inst->shader_cache_dir[0] == '\0' means
 *           caching is disabled and this always misses.
 * glsl_source  Null-terminated GLSL source text.
 * stage        Shader stage the source was written for.
 * out_size     Set to the blob size in bytes on a hit.
 * Returns      Heap-allocated SPIR-V bytes, or NULL.
 */
uint32_t *ca_shader_cache_lookup(Ca_Instance *instance,
                                 const char *glsl_source,
                                 VkShaderStageFlagBits stage,
                                 size_t *out_size);

/*
 * Best-effort write of a freshly compiled SPIR-V blob to the cache.
 *
 * Never fails loudly: a read-only cache directory, a full disk, or any
 * other write error is silently ignored, since the caller already has a
 * working VkShaderModule from this compile regardless of whether the
 * cache write succeeds.
 *
 * instance     Owning Ca_Instance; no-op if caching is disabled.
 * glsl_source  Null-terminated GLSL source text (same key as lookup).
 * stage        Shader stage the source was written for.
 * spirv        SPIR-V words to persist.
 * word_count   Number of uint32_t words in spirv.
 */
void ca_shader_cache_store(Ca_Instance *instance,
                           const char *glsl_source,
                           VkShaderStageFlagBits stage,
                           const uint32_t *spirv,
                           size_t word_count);

/*
 * Resolve and create (mkdir -p) the shader cache directory for an
 * instance being created, copying it into inst->shader_cache_dir and
 * setting inst->shader_cache_writable independently.
 *
 * Called once from ca_instance_create, before ca_renderer_init runs any
 * shader compiles. Read and write are checked separately: an unreadable
 * or uncreatable directory (bad permissions, read-only filesystem above
 * it) leaves shader_cache_dir empty and disables caching entirely,
 * exactly like passing dir == NULL. A directory that is readable but not
 * writable (e.g. pre-seeded read-only by an installer) still populates
 * shader_cache_dir so lookups can serve hits — shader_cache_writable
 * just stays false, so ca_shader_cache_store becomes a permanent no-op.
 * Neither case ever fails instance creation.
 *
 * instance  Instance being initialised.
 * dir       Directory path from Ca_InstanceDesc::shader_cache_dir, or NULL.
 */
void ca_shader_cache_init_dir(Ca_Instance *instance, const char *dir);
