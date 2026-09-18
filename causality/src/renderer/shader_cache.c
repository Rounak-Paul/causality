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
  #include <process.h>
  #include <windows.h>
  #define CA_MKDIR(path)   _mkdir(path)
  #define CA_DIR_READABLE(path) (_access((path), 0) == 0)
  #define CA_GETPID()      _getpid()
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define CA_MKDIR(path)   mkdir((path), 0755)
  #define CA_DIR_READABLE(path) (access((path), R_OK | X_OK) == 0)
  #define CA_GETPID()      getpid()
#endif

/*
 * Force a written-but-still-buffered file to stable storage.
 *
 * The caller must have flushed stdio first; this drives the kernel's own
 * page cache out to the device. On macOS plain fsync only schedules the
 * write with the drive, so F_FULLFSYNC is used where available and fsync
 * is the fallback when the filesystem does not implement it.
 *
 * f        Open stream, already fflush-ed.
 * Returns  true if the bytes are durable.
 */
static bool ca_file_sync(FILE *f)
{
#ifdef _WIN32
    const int fd = _fileno(f);
    if (fd < 0) return false;
    const HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(h) != 0;
#else
    const int fd = fileno(f);
    if (fd < 0) return false;
  #ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC, 0) == 0) return true;
  #endif
    return fsync(fd) == 0;
#endif
}

/*
 * Force a directory's entries to stable storage so a rename into it
 * survives power loss.
 *
 * Best-effort: Windows has no directory handle to sync through this API,
 * and some filesystems reject opening a directory for this purpose. A
 * failure only means the rename may need replaying, which the cache
 * tolerates by recompiling.
 *
 * dir  Directory path to sync.
 */
static void ca_dir_sync(const char *dir)
{
#ifndef _WIN32
    if (!dir || dir[0] == '\0') return;
    const int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    (void)fsync(fd);
    (void)close(fd);
#else
    (void)dir;
#endif
}

/* Bumped whenever a change to shaderc's invocation (target env,
   optimization level, or this cache's own file format) could make an
   existing cached .spv incompatible or stale. Folding it into the cache
   key means such a change simply misses old entries and re-compiles —
   no explicit cache-versioning or migration step needed anywhere. */
#define CA_SHADER_CACHE_KEY_VERSION "1"

/* First word of every SPIR-V module (SPIR-V spec 2.3, "Physical Layout").
   A module written on a big-endian host appears byte-reversed here; such a
   blob is not consumable by this process, so it is rejected as a miss. */
#define CA_SPIRV_MAGIC 0x07230203u

/* Words in the SPIR-V physical-layout header preceding the instruction
   stream: magic, version, generator, id bound, schema. */
#define CA_SPIRV_HEADER_WORDS 5u

/*
 * Report whether a blob read from the cache is a structurally intact
 * SPIR-V module for this host.
 *
 * A file interrupted mid-write (power loss, a full disk, a killed
 * process) frequently survives as a correctly-magicked prefix whose
 * instruction stream simply stops partway through. Handing such a blob to
 * vkCreateShaderModule is not a recoverable error path: drivers may fault
 * inside their own parser rather than returning a result code, so the
 * damage has to be caught here, before the call. Validating the header
 * and walking the instruction stream to confirm it ends exactly on an
 * instruction boundary rejects every truncated prefix without a SPIR-V
 * parser.
 *
 * words       Blob contents.
 * word_count  Number of uint32_t words in words.
 * Returns     true if the blob is safe to pass to vkCreateShaderModule.
 */
static bool ca_spirv_blob_intact(const uint32_t *words, size_t word_count)
{
    if (!words || word_count < CA_SPIRV_HEADER_WORDS) return false;
    if (words[0] != CA_SPIRV_MAGIC) return false;

    /* The id bound is one past the largest result id in the module; zero
       is impossible and an absurd value signals a corrupt header rather
       than a module this cache wrote. */
    const uint32_t id_bound = words[3];
    if (id_bound == 0u || id_bound > (1u << 24)) return false;

    /* Each instruction is a word-count in the high 16 bits followed by
       that many words total. A truncated file runs past the end mid-walk;
       an intact one lands exactly on word_count. */
    size_t offset = CA_SPIRV_HEADER_WORDS;
    while (offset < word_count) {
        const uint32_t instruction_words = words[offset] >> 16;
        if (instruction_words == 0u) return false;
        if (instruction_words > word_count - offset) return false;
        offset += instruction_words;
    }
    return offset == word_count;
}

/* Cache container format. The payload a cache file holds is the SPIR-V
   module; everything before it exists so a damaged file is detectable.

   Structural validation of SPIR-V alone cannot do that job: a write
   interrupted exactly on an instruction boundary leaves a module that
   parses cleanly but whose body is missing, and the resulting undefined
   forward references fault inside the driver's own parser. Recording the
   intended length and a digest of the bytes makes every short or altered
   file a detectable miss regardless of where it was cut. */
#define CA_SHADER_CACHE_FILE_MAGIC 0x43414353u   /* "SCAC" little-endian */
#define CA_SHADER_CACHE_FILE_VERSION 1u

/* magic, version, payload byte length, then the 64-bit digest as two
   words (low, high) — 5 words, keeping the payload uint32-aligned. */
#define CA_SHADER_CACHE_FILE_HEADER_WORDS 5u

/* FNV-1a 64-bit over raw bytes — the same construction as the cache key
   below, used here to detect damage rather than to identify content. */
static uint64_t ca_shader_cache_digest(const void *data, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0u; i < size; ++i) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

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
    /* A cache file is a word-aligned header plus a word-aligned SPIR-V
       payload — a size failing either check cannot be a file this cache
       wrote, so treat it as a miss rather than handing malformed data to
       vkCreateShaderModule (whose codeSize/pCode alignment requirement
       this also protects). */
    const long header_bytes =
        (long)(CA_SHADER_CACHE_FILE_HEADER_WORDS * sizeof(uint32_t));
    if (size <= header_bytes || (size % 4) != 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint32_t *file_words = (uint32_t *)CA_MALLOC((size_t)size);
    if (!file_words) { fclose(f); return NULL; }

    const size_t read = fread(file_words, 1u, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        CA_FREE(file_words);
        return NULL;
    }

    /* Reject anything not written by this version of this cache before
       trusting the length or digest fields that follow. */
    if (file_words[0] != CA_SHADER_CACHE_FILE_MAGIC ||
        file_words[1] != CA_SHADER_CACHE_FILE_VERSION) {
        CA_FREE(file_words);
        return NULL;
    }

    /* The recorded length is what catches a file cut short: a truncated
       write leaves fewer payload bytes on disk than the header promises,
       whether or not the cut happened to land on a SPIR-V instruction
       boundary. */
    const uint32_t payload_bytes = file_words[2];
    if (payload_bytes == 0u || (payload_bytes % sizeof(uint32_t)) != 0u ||
        (size_t)payload_bytes != (size_t)size - (size_t)header_bytes) {
        CA_FREE(file_words);
        return NULL;
    }

    const uint64_t recorded_digest =
        (uint64_t)file_words[3] | ((uint64_t)file_words[4] << 32);
    const uint32_t *payload = file_words + CA_SHADER_CACHE_FILE_HEADER_WORDS;
    if (ca_shader_cache_digest(payload, payload_bytes) != recorded_digest) {
        CA_FREE(file_words);
        return NULL;
    }

    /* Length and digest agree, so the payload is the exact blob that was
       stored. Structure is still checked because a file of the right
       length and digest could have been written from a foreign or
       already-damaged source. */
    if (!ca_spirv_blob_intact(payload, payload_bytes / sizeof(uint32_t))) {
        CA_FREE(file_words);
        return NULL;
    }

    /* Hand back just the payload; the caller frees what it receives and
       knows nothing of this container. */
    uint32_t *spirv = (uint32_t *)CA_MALLOC(payload_bytes);
    if (!spirv) {
        CA_FREE(file_words);
        return NULL;
    }
    memcpy(spirv, payload, payload_bytes);
    CA_FREE(file_words);

    if (out_size) *out_size = (size_t)payload_bytes;
    return spirv;
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
       destination on the same volume as the source.

       The name carries the process id, not a heap address: two processes
       compiling the same shader can otherwise pick the same temp path and
       write over each other, which is exactly the torn file this rename
       exists to prevent. The source pointer is kept alongside it so two
       compiles within one process still differ. */
    char tmp_path[1040];
    const int written = snprintf(tmp_path, sizeof(tmp_path), "%s.%ld.%d.tmp",
                                 path, (long)CA_GETPID(),
                                 (int)((uintptr_t)spirv & 0xffffff));
    if (written <= 0 || (size_t)written >= sizeof(tmp_path)) return;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;   /* read-only cache dir, disk full, etc. — silent */

    const size_t byte_count = word_count * sizeof(uint32_t);
    if (byte_count > 0xffffffffull) { fclose(f); remove(tmp_path); return; }

    /* Header first, so a reader can tell a complete file from one cut
       short by comparing the recorded length and digest against what is
       actually on disk. */
    const uint64_t digest = ca_shader_cache_digest(spirv, byte_count);
    const uint32_t header[CA_SHADER_CACHE_FILE_HEADER_WORDS] = {
        CA_SHADER_CACHE_FILE_MAGIC,
        CA_SHADER_CACHE_FILE_VERSION,
        (uint32_t)byte_count,
        (uint32_t)(digest & 0xffffffffull),
        (uint32_t)(digest >> 32),
    };
    const size_t wrote_header = fwrite(header, 1u, sizeof(header), f);
    const size_t wrote = (wrote_header == sizeof(header))
                             ? fwrite(spirv, 1u, byte_count, f)
                             : 0u;

    /* Flush stdio's buffer into the kernel, then force the kernel's own
       page cache to stable storage before the rename. Without this the
       rename can reach disk while the data behind it has not, leaving a
       correctly named file full of zeros or stale bytes after a power
       loss — the exact shape of corruption ca_spirv_blob_intact then has
       to reject on every subsequent launch. */
    bool durable = (wrote == byte_count) && (fflush(f) == 0);
    if (durable) durable = ca_file_sync(f);
    const int close_rc = fclose(f);
    if (!durable || close_rc != 0) {
        remove(tmp_path);
        return;
    }

    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return;
    }

    /* The rename is a directory mutation and is itself only durable once
       the directory entry is synced; otherwise a power loss can resurrect
       the pre-rename state and orphan the temp file. */
    ca_dir_sync(instance->shader_cache_dir);
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
