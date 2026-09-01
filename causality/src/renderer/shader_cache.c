// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* shader_cache.c — content-hash-keyed on-disk cache for compiled SPIR-V.
   See shader_cache.h for the calling contract. */
#include "shader_cache.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define CA_MKDIR(path)   _mkdir(path)
  #define CA_DIR_READABLE(path) (_access((path), 0) == 0)
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define CA_MKDIR(path)   mkdir((path), 0755)
  #define CA_DIR_READABLE(path) (access((path), R_OK | X_OK) == 0)
#endif

/* Bumped whenever a change to shaderc's invocation (target env,
   optimization level, or this cache's own file format) could make an
   existing cached .spv incompatible or stale. Folding it into the cache
   key means such a change simply misses old entries and re-compiles —
   no explicit cache-versioning or migration step needed anywhere. */
#define CA_SHADER_CACHE_KEY_VERSION "1"

/* FNV-1a 64-bit — not cryptographic, but collision-resistant enough for
   a local content-addressed cache key, with no external dependency. */
static uint64_t ca_shader_cache_hash(const char *stage_tag,
                                     const char *glsl_source)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t prime = 0x100000001b3ull;
    const char *parts[] = { stage_tag, CA_SHADER_CACHE_KEY_VERSION, glsl_source };
    for (size_t p = 0u; p < 3u; ++p) {
        for (const unsigned char *c = (const unsigned char *)parts[p]; *c; ++c) {
            h ^= (uint64_t)*c;
            h *= prime;
        }
        /* Separator byte between fields so e.g. ("ab","c") and ("a","bc")
           never collide. */
        h ^= 0xffu;
        h *= prime;
    }
    return h;
}

static const char *ca_shader_cache_stage_tag(VkShaderStageFlagBits stage)
{
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:   return "vert";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "frag";
    default:                           return "auto";
    }
}

/* Build "<cache_dir>/<16-hex-digit-hash>.spv" into out. Returns false if
   it wouldn't fit (caller treats that identically to "no cache dir"). */
static bool ca_shader_cache_path(const Ca_Instance *instance,
                                 const char *glsl_source,
                                 VkShaderStageFlagBits stage,
                                 char *out, size_t out_size)
{
    if (!instance || instance->shader_cache_dir[0] == '\0' || !glsl_source)
        return false;
    const uint64_t hash = ca_shader_cache_hash(
        ca_shader_cache_stage_tag(stage), glsl_source);
    const int written = snprintf(out, out_size, "%s/%016llx.spv",
                                 instance->shader_cache_dir,
                                 (unsigned long long)hash);
    return written > 0 && (size_t)written < out_size;
}

uint32_t *ca_shader_cache_lookup(Ca_Instance *instance,
                                 const char *glsl_source,
                                 VkShaderStageFlagBits stage,
                                 size_t *out_size)
{
    if (out_size) *out_size = 0u;

    char path[1024];
    if (!ca_shader_cache_path(instance, glsl_source, stage, path, sizeof(path)))
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long size = ftell(f);
    /* A SPIR-V module is a non-empty, word-aligned uint32 stream — a
       size that fails either check cannot be a blob this cache wrote,
       so treat it as a miss rather than handing malformed data to
       vkCreateShaderModule (whose codeSize/pCode alignment requirement
       this also protects). */
    if (size <= 0 || (size % 4) != 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint32_t *bytes = (uint32_t *)CA_MALLOC((size_t)size);
    if (!bytes) { fclose(f); return NULL; }

    const size_t read = fread(bytes, 1u, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        CA_FREE(bytes);
        return NULL;
    }

    if (out_size) *out_size = (size_t)size;
    return bytes;
}

void ca_shader_cache_store(Ca_Instance *instance,
                           const char *glsl_source,
                           VkShaderStageFlagBits stage,
                           const uint32_t *spirv,
                           size_t word_count)
{
    if (!spirv || word_count == 0u) return;
    if (!instance || !instance->shader_cache_writable) return;

    char path[1024];
    if (!ca_shader_cache_path(instance, glsl_source, stage, path, sizeof(path)))
        return;

    /* Write to a per-process temp file then rename into place, so a
       concurrent reader (another Causality instance in another process
       sharing the same cache dir) never observes a partially written
       .spv — rename is atomic on both POSIX and Windows (NTFS) for a
       destination on the same volume as the source. */
    char tmp_path[1040];
    const int written = snprintf(tmp_path, sizeof(tmp_path), "%s.%d.tmp",
                                 path, (int)(uintptr_t)spirv);
    if (written <= 0 || (size_t)written >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;   /* read-only cache dir, disk full, etc. — silent */

    const size_t byte_count = word_count * sizeof(uint32_t);
    const size_t wrote = fwrite(spirv, 1u, byte_count, f);
    const int close_rc = fclose(f);
    if (wrote != byte_count || close_rc != 0) {
        remove(tmp_path);
        return;
    }

    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
    }
}

/* mkdir -p: create each missing path component. Causality has no
   existing recursive-mkdir helper (its platform layer is window/menu-only,
   see src/platform/), so this stays local rather than pulling in a
   dependency for one call site. Ignores failures on intermediate
   components (mkdir on an already-existing directory is expected to
   fail and is harmless) — the caller verifies the end result itself via
   CA_DIR_READABLE / the write probe rather than trusting this loop's
   return value. */
static void ca_shader_cache_mkdir_p(const char *dir)
{
    char partial[1024];
    const size_t len = strlen(dir);
    if (len >= sizeof(partial)) return;
    memcpy(partial, dir, len + 1u);

    for (size_t i = 1u; i < len; ++i) {
        if (partial[i] != '/' && partial[i] != '\\') continue;
        const char saved = partial[i];
        partial[i] = '\0';
        if (partial[0] != '\0') (void)CA_MKDIR(partial);
        partial[i] = saved;
    }
    (void)CA_MKDIR(partial);
}

void ca_shader_cache_init_dir(Ca_Instance *instance, const char *dir)
{
    if (!instance) return;
    instance->shader_cache_dir[0] = '\0';
    instance->shader_cache_writable = false;
    if (!dir || dir[0] == '\0') return;
    if (strlen(dir) >= sizeof(instance->shader_cache_dir)) return;

    if (!CA_DIR_READABLE(dir)) {
        /* Doesn't exist yet (first launch) or genuinely isn't readable.
           Try to create it — this is also the right move for a sandboxed
           app that CAN write its own data directory but hasn't launched
           before, which is the common case, not the exception this
           function exists to tolerate. */
        ca_shader_cache_mkdir_p(dir);
        if (!CA_DIR_READABLE(dir)) return;   /* still unreadable: no caching at all */
    }

    /* Readable (freshly created, pre-existing and writable, or
       pre-seeded read-only by an installer) — lookups can use it either
       way, so record the path unconditionally from here. */
    const size_t len = strlen(dir);
    memcpy(instance->shader_cache_dir, dir, len + 1u);

    /* Write access is a separate, optional capability: probe by actually
       creating a file rather than trusting access()/permission bits,
       which can lie under sandboxes, ACLs, or read-only bind mounts.
       A failed probe leaves shader_cache_writable false — cache_store
       becomes a permanent silent no-op for this instance, while lookups
       above continue to serve any entries already present (e.g. a cache
       shipped read-only alongside the app). */
    char probe[1088];
    const int pw = snprintf(probe, sizeof(probe), "%s/.ca_write_probe", dir);
    if (pw <= 0 || (size_t)pw >= sizeof(probe)) return;
    FILE *f = fopen(probe, "wb");
    if (!f) return;
    fclose(f);
    remove(probe);
    instance->shader_cache_writable = true;
}
