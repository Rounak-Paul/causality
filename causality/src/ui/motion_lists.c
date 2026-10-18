// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "motion_lists.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Ca_MotionListKind kind;
    uint32_t          count;
    uint64_t          hash;
    void             *items;
} PoolEntry;

static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static PoolEntry  *g_entries;
static uint32_t    g_count, g_capacity;
static bool        g_exit_registered;

static const size_t k_item_size[CA_MOTION_LIST_KIND_COUNT] = {
    [CA_MOTION_LIST_TRANSITION_PROPERTY]  = sizeof(int),
    [CA_MOTION_LIST_TRANSITION_DURATION]  = sizeof(float),
    [CA_MOTION_LIST_TRANSITION_DELAY]     = sizeof(float),
    [CA_MOTION_LIST_TRANSITION_TIMING]    = sizeof(Ca_EasingDef),
    [CA_MOTION_LIST_ANIMATION_NAME]       = sizeof(uint32_t),
    [CA_MOTION_LIST_ANIMATION_DURATION]   = sizeof(float),
    [CA_MOTION_LIST_ANIMATION_DELAY]      = sizeof(float),
    [CA_MOTION_LIST_ANIMATION_TIMING]     = sizeof(Ca_EasingDef),
    [CA_MOTION_LIST_ANIMATION_ITERATIONS] = sizeof(float),
    [CA_MOTION_LIST_ANIMATION_DIRECTION]  = sizeof(uint8_t),
    [CA_MOTION_LIST_ANIMATION_FILL]       = sizeof(uint8_t),
    [CA_MOTION_LIST_ANIMATION_PLAY_STATE] = sizeof(uint8_t),
};

static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) { }
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

/** FNV-1a over the list's bytes, seeded with its kind. */
static uint64_t hash_items(Ca_MotionListKind kind, const void *items, size_t bytes)
{
    uint64_t hash = 14695981039346656037ull ^ (uint64_t)kind;
    const unsigned char *p = items;
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static void release_pool(void)
{
    lock();
    for (uint32_t i = 0; i < g_count; ++i) free(g_entries[i].items);
    free(g_entries);
    g_entries = NULL;
    g_count = g_capacity = 0;
    unlock();
}

uint32_t ca_motion_list_intern(Ca_MotionListKind kind, const void *items, uint32_t count)
{
    if (count == 0 || !items || (unsigned)kind >= CA_MOTION_LIST_KIND_COUNT) return 0;
    const size_t bytes = k_item_size[kind] * count;
    const uint64_t hash = hash_items(kind, items, bytes);

    lock();
    uint32_t handle = 0;
    for (uint32_t i = 0; i < g_count; ++i) {
        const PoolEntry *e = &g_entries[i];
        if (e->hash == hash && e->kind == kind && e->count == count &&
            memcmp(e->items, items, bytes) == 0) {
            handle = i + 1u;
            break;
        }
    }
    if (!handle) {
        if (g_count == g_capacity) {
            const uint32_t capacity = g_capacity ? g_capacity * 2u : 32u;
            PoolEntry *grown = realloc(g_entries, sizeof(PoolEntry) * capacity);
            if (grown) { g_entries = grown; g_capacity = capacity; }
        }
        void *copy = g_count < g_capacity ? malloc(bytes) : NULL;
        if (copy) {
            memcpy(copy, items, bytes);
            g_entries[g_count++] = (PoolEntry){ kind, count, hash, copy };
            handle = g_count;
            if (!g_exit_registered) {
                g_exit_registered = true;
                atexit(release_pool);
            }
        }
    }
    unlock();
    return handle;
}

const void *ca_motion_list_items(uint32_t handle, uint32_t *count)
{
    const void *items = NULL;
    uint32_t n = 0;
    lock();
    if (handle >= 1u && handle <= g_count) {
        items = g_entries[handle - 1u].items;
        n = g_entries[handle - 1u].count;
    }
    unlock();
    if (count) *count = n;
    return items;
}
