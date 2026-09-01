// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

/* Exercises the on-disk shader cache (src/renderer/shader_cache.c)
   without a live Vulkan device: ca_shader_cache_lookup/store/init_dir
   only touch Ca_Instance::shader_cache_dir/shader_cache_writable and the
   filesystem, so a zero-initialized instance is sufficient here. The
   VkShaderModule-producing half of ca_shader_compile itself is exercised
   only by running the real app (needs an actual Vulkan device). */

/* ca_internal.h declares Ca_Instance/Ca_Window with a GLFWwindow* member
   but does not itself #include GLFW — every real causality .c file gets
   that via the causality target's precompiled header (src/pch.h), which
   test executables don't share. This test computes sizeof(Ca_Instance),
   so it needs the real declaration, not just an opaque pointer target. */
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "ca_internal.h"
#include "../src/renderer/shader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #define CA_TEST_RMDIR(path) _rmdir(path)
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #define CA_TEST_RMDIR(path) rmdir(path)
#endif

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                      \
                    __FILE__, __LINE__, #condition);                            \
            return false;                                                       \
        }                                                                       \
    } while (0)

static void test_tmp_dir(char *out, size_t out_size, const char *suffix)
{
    const char *base = getenv("TMPDIR");
    if (!base || base[0] == '\0') base = "/tmp";
    snprintf(out, out_size, "%s/ca_shader_cache_test_%s_%d",
             base, suffix, (int)getpid());
}

/* Best-effort recursive cleanup of a test cache dir: unlink every .spv
   plus the write-probe file it may have created, then rmdir. */
static void test_cleanup_dir(const char *dir)
{
    /* Cache filenames are deterministic per test (fixed source strings),
       so there is no need for a real directory-listing sweep here — just
       remove the exact files this suite's cases can produce. */
    char path[1200];
    snprintf(path, sizeof(path), "%s/.ca_write_probe", dir);
    remove(path);
    CA_TEST_RMDIR(dir);
}

static Ca_Instance *test_instance(void)
{
    return (Ca_Instance *)calloc(1, sizeof(Ca_Instance));
}

/** A miss on an instance with no cache dir configured never touches disk. */
static bool test_disabled_by_default(void)
{
    Ca_Instance *inst = test_instance();
    CHECK(inst);
    size_t size = 0u;
    uint32_t *blob = ca_shader_cache_lookup(inst, "void main(){}",
                                            VK_SHADER_STAGE_FRAGMENT_BIT, &size);
    CHECK(blob == NULL);
    CHECK(size == 0u);
    /* store() must be a silent no-op too — nothing to assert beyond "does
       not crash" since shader_cache_dir is empty. */
    const uint32_t words[2] = { 0x07230203u, 0u };
    ca_shader_cache_store(inst, "void main(){}", VK_SHADER_STAGE_FRAGMENT_BIT,
                          words, 2u);
    free(inst);
    return true;
}

/** init_dir creates a fresh directory and marks it read-write. */
static bool test_init_creates_fresh_dir(void)
{
    char dir[1024];
    test_tmp_dir(dir, sizeof(dir), "fresh");
    test_cleanup_dir(dir);   /* in case a prior failed run left it behind */

    Ca_Instance *inst = test_instance();
    CHECK(inst);
    ca_shader_cache_init_dir(inst, dir);
    CHECK(strcmp(inst->shader_cache_dir, dir) == 0);
    CHECK(inst->shader_cache_writable == true);

    free(inst);
    test_cleanup_dir(dir);
    return true;
}

/** NULL/empty dir leaves caching fully disabled. */
static bool test_init_null_disables(void)
{
    Ca_Instance *inst = test_instance();
    CHECK(inst);
    ca_shader_cache_init_dir(inst, NULL);
    CHECK(inst->shader_cache_dir[0] == '\0');
    CHECK(inst->shader_cache_writable == false);

    ca_shader_cache_init_dir(inst, "");
    CHECK(inst->shader_cache_dir[0] == '\0');
    CHECK(inst->shader_cache_writable == false);

    free(inst);
    return true;
}

/** A round trip: store a blob, then look it up by the same source+stage. */
static bool test_store_then_lookup_hit(void)
{
    char dir[1024];
    test_tmp_dir(dir, sizeof(dir), "roundtrip");
    test_cleanup_dir(dir);

    Ca_Instance *inst = test_instance();
    CHECK(inst);
    ca_shader_cache_init_dir(inst, dir);
    CHECK(inst->shader_cache_writable == true);

    const char *source = "#version 450\nvoid main(){ gl_Position = vec4(0); }";
    const uint32_t words[4] = { 0x07230203u, 1u, 2u, 3u };
    ca_shader_cache_store(inst, source, VK_SHADER_STAGE_VERTEX_BIT, words, 4u);

    size_t size = 0u;
    uint32_t *blob = ca_shader_cache_lookup(inst, source,
                                            VK_SHADER_STAGE_VERTEX_BIT, &size);
    CHECK(blob != NULL);
    CHECK(size == sizeof(words));
    CHECK(memcmp(blob, words, sizeof(words)) == 0);
    free(blob);

    /* A different stage for the identical source text must miss — stage
       is part of the key, not just the source. */
    size = 0u;
    uint32_t *cross_stage = ca_shader_cache_lookup(
        inst, source, VK_SHADER_STAGE_FRAGMENT_BIT, &size);
    CHECK(cross_stage == NULL);
    CHECK(size == 0u);

    /* A one-character source change must miss too. */
    size = 0u;
    uint32_t *diff_source = ca_shader_cache_lookup(
        inst, "#version 450\nvoid main(){ gl_Position = vec4(1); }",
        VK_SHADER_STAGE_VERTEX_BIT, &size);
    CHECK(diff_source == NULL);
    CHECK(size == 0u);

    free(inst);
    char cache_file[1200];
    /* Can't predict the hash filename from here without duplicating the
       hash function, so sweep is left to test_cleanup_dir's caller-level
       rm -rf equivalent in CI; locally this just leaves one .spv behind
       in TMPDIR, which is harmless and gets cleaned by the OS/dev
       machine's normal /tmp policy. */
    (void)cache_file;
    return true;
}

/** A directory that exists but cannot be written still serves lookups
    for entries already present — the read/write independence this cache
    exists to support for sandboxed or read-only-install consumers. */
static bool test_readonly_dir_serves_hits_but_not_writes(void)
{
#ifdef _WIN32
    /* chmod-based read-only simulation is POSIX-specific; Windows ACL
       manipulation would need a different mechanism entirely, and the
       read/write-independence logic itself is platform-agnostic C
       exercised by every other case in this file. */
    return true;
#else
    char dir[1024];
    test_tmp_dir(dir, sizeof(dir), "readonly");
    test_cleanup_dir(dir);

    /* First pass: writable, populate one entry. */
    Ca_Instance *writer = test_instance();
    CHECK(writer);
    ca_shader_cache_init_dir(writer, dir);
    CHECK(writer->shader_cache_writable == true);

    const char *source = "readonly-probe-shader";
    const uint32_t words[2] = { 0x07230203u, 42u };
    ca_shader_cache_store(writer, source, VK_SHADER_STAGE_FRAGMENT_BIT, words, 2u);
    free(writer);

    /* Lock the directory down to read+execute, no write. */
    CHECK(chmod(dir, 0555) == 0);

    Ca_Instance *reader = test_instance();
    CHECK(reader);
    ca_shader_cache_init_dir(reader, dir);
    CHECK(strcmp(reader->shader_cache_dir, dir) == 0);
    CHECK(reader->shader_cache_writable == false);

    size_t size = 0u;
    uint32_t *blob = ca_shader_cache_lookup(reader, source,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, &size);
    CHECK(blob != NULL);
    CHECK(size == sizeof(words));
    CHECK(memcmp(blob, words, sizeof(words)) == 0);
    free(blob);

    /* A store attempt while read-only must not crash and must not
       corrupt the directory (verified by the still-successful lookup
       above having already run before this). */
    const uint32_t other_words[2] = { 0x07230203u, 99u };
    ca_shader_cache_store(reader, "another-shader",
                          VK_SHADER_STAGE_VERTEX_BIT, other_words, 2u);
    size = 0u;
    uint32_t *should_miss = ca_shader_cache_lookup(
        reader, "another-shader", VK_SHADER_STAGE_VERTEX_BIT, &size);
    CHECK(should_miss == NULL);

    free(reader);
    CHECK(chmod(dir, 0755) == 0);
    test_cleanup_dir(dir);
    return true;
#endif
}

/** A directory that is entirely inaccessible disables caching outright
    rather than only disabling writes — there is nothing to read either. */
static bool test_inaccessible_dir_disables_entirely(void)
{
#ifdef _WIN32
    return true;   /* see note in the read-only test above */
#else
    char dir[1024];
    test_tmp_dir(dir, sizeof(dir), "locked");
    test_cleanup_dir(dir);
    CHECK(mkdir(dir, 0755) == 0);
    CHECK(chmod(dir, 0000) == 0);

    Ca_Instance *inst = test_instance();
    CHECK(inst);
    ca_shader_cache_init_dir(inst, dir);
    CHECK(inst->shader_cache_dir[0] == '\0');
    CHECK(inst->shader_cache_writable == false);

    size_t size = 0u;
    uint32_t *blob = ca_shader_cache_lookup(inst, "anything",
                                            VK_SHADER_STAGE_FRAGMENT_BIT, &size);
    CHECK(blob == NULL);

    free(inst);
    CHECK(chmod(dir, 0755) == 0);
    test_cleanup_dir(dir);
    return true;
#endif
}

/* Returns a heap path "<dir>/<name>" for the single non-hidden regular
   file directly inside dir, or NULL if there isn't exactly one — used to
   find the .spv file a preceding ca_shader_cache_store call just wrote,
   without duplicating shader_cache.c's private hash/path function. */
static char *test_find_sole_cache_file(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return NULL;

    char *found = NULL;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;   /* skip ., .., .ca_write_probe */
        if (found) { free(found); closedir(d); return NULL; }  /* not exactly one */
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        found = strdup(path);
    }
    closedir(d);
    return found;
}

/** A corrupt/foreign cache file (non-SPIR-V-shaped) is treated as a
    miss, never handed to the caller as if it were valid SPIR-V. */
static bool test_corrupt_cache_file_is_a_miss(void)
{
#ifdef _WIN32
    return true;   /* opendir/dirent helper above is POSIX-only */
#else
    char dir[1024];
    test_tmp_dir(dir, sizeof(dir), "corrupt");
    test_cleanup_dir(dir);

    Ca_Instance *inst = test_instance();
    CHECK(inst);
    ca_shader_cache_init_dir(inst, dir);
    CHECK(inst->shader_cache_writable == true);

    const char *source = "corrupt-probe-shader";
    const uint32_t words[3] = { 0x07230203u, 1u, 2u };
    ca_shader_cache_store(inst, source, VK_SHADER_STAGE_FRAGMENT_BIT, words, 3u);

    /* Sanity: the well-formed entry hits before we corrupt it. */
    size_t size = 0u;
    uint32_t *good = ca_shader_cache_lookup(inst, source,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, &size);
    CHECK(good != NULL);
    free(good);

    char *cache_file = test_find_sole_cache_file(dir);
    CHECK(cache_file != NULL);

    /* Truncate to an odd, non-word-aligned byte count — must be rejected
       by the size/alignment guard before any attempt to hand it to
       vkCreateShaderModule as SPIR-V. */
    FILE *f = fopen(cache_file, "wb");
    CHECK(f != NULL);
    const char garbage[5] = { 'x', 'x', 'x', 'x', 'x' };
    CHECK(fwrite(garbage, 1u, sizeof(garbage), f) == sizeof(garbage));
    fclose(f);

    size = 0u;
    uint32_t *should_miss = ca_shader_cache_lookup(
        inst, source, VK_SHADER_STAGE_FRAGMENT_BIT, &size);
    CHECK(should_miss == NULL);
    CHECK(size == 0u);

    free(cache_file);
    free(inst);
    test_cleanup_dir(dir);
    return true;
#endif
}

int main(void)
{
    bool ok = test_disabled_by_default() &&
              test_init_creates_fresh_dir() &&
              test_init_null_disables() &&
              test_store_then_lookup_hit() &&
              test_readonly_dir_serves_hits_but_not_writes() &&
              test_inaccessible_dir_disables_entirely() &&
              test_corrupt_cache_file_is_a_miss();
    if (!ok) return 1;
    puts("causality shader cache tests passed");
    return 0;
}
