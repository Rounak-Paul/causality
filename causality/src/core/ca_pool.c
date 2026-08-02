// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "ca_pool.h"

#include "causality_config.h"

#include <stdint.h>
#include <string.h>

#define CA_POOL_TARGET_CHUNK_BYTES (16u * 1024u)
#define CA_POOL_MIN_CHUNK_ELEMENTS 8u
#define CA_POOL_MAX_CHUNK_ELEMENTS 256u

typedef struct Ca_PoolChunk {
    unsigned char *data;
    unsigned char *occupied;
} Ca_PoolChunk;

/** Returns whether pool metadata is internally valid. */
static bool pool_valid(const Ca_Pool *pool)
{
    if (!pool || pool->element_size == 0 || pool->chunk_capacity == 0 ||
        !ca_dyn_array_valid(&pool->chunks) ||
        !ca_dyn_array_valid(&pool->free_indices) ||
        pool->chunks.element_size != sizeof(Ca_PoolChunk) ||
        pool->free_indices.element_size != sizeof(size_t) ||
        pool->live_count > pool->slot_count)
        return false;
    if (pool->chunks.count > SIZE_MAX / pool->chunk_capacity) return false;
    return pool->slot_count <= pool->chunks.count * pool->chunk_capacity;
}

/** Locates an element and returns its stable slot index. */
static bool pool_find_index(const Ca_Pool *pool, const void *element,
                            size_t *out_index)
{
    if (!pool_valid(pool) || !element || !out_index) return false;
    uintptr_t address = (uintptr_t)element;
    if (pool->chunk_capacity > SIZE_MAX / pool->element_size) return false;
    size_t chunk_bytes = pool->chunk_capacity * pool->element_size;
    for (size_t i = 0; i < pool->chunks.count; ++i) {
        const Ca_PoolChunk *chunk = ca_dyn_array_at_const(&pool->chunks, i);
        uintptr_t begin = (uintptr_t)chunk->data;
        if (begin > UINTPTR_MAX - chunk_bytes ||
            address < begin || address >= begin + chunk_bytes)
            continue;
        size_t offset = (size_t)(address - begin);
        if (offset % pool->element_size != 0) return false;
        size_t local_index = offset / pool->element_size;
        if (i > (SIZE_MAX - local_index) / pool->chunk_capacity) return false;
        size_t index = i * pool->chunk_capacity + local_index;
        if (index >= pool->slot_count) return false;
        *out_index = index;
        return true;
    }
    return false;
}

/** Allocates and appends one empty storage chunk atomically. */
static bool pool_append_chunk(Ca_Pool *pool)
{
    if (!pool_valid(pool) ||
        pool->chunks.count == SIZE_MAX ||
        pool->chunks.count + 1u > SIZE_MAX / pool->chunk_capacity ||
        pool->chunk_capacity > SIZE_MAX / pool->element_size)
        return false;

    size_t future_slots = (pool->chunks.count + 1u) * pool->chunk_capacity;
    if (!ca_dyn_array_reserve(&pool->chunks, pool->chunks.count + 1u) ||
        !ca_dyn_array_reserve(&pool->free_indices, future_slots))
        return false;

    Ca_PoolChunk chunk = { 0 };
    chunk.data = CA_CALLOC(pool->chunk_capacity, pool->element_size);
    chunk.occupied = CA_CALLOC(pool->chunk_capacity, sizeof(*chunk.occupied));
    if (!chunk.data || !chunk.occupied) {
        CA_FREE(chunk.occupied);
        CA_FREE(chunk.data);
        return false;
    }
    if (!ca_dyn_array_push(&pool->chunks, &chunk)) {
        CA_FREE(chunk.occupied);
        CA_FREE(chunk.data);
        return false;
    }
    return true;
}

size_t ca_pool_recommended_chunk_capacity(size_t element_size)
{
    if (element_size == 0) return 0;
    size_t capacity = CA_POOL_TARGET_CHUNK_BYTES / element_size;
    if (capacity < CA_POOL_MIN_CHUNK_ELEMENTS)
        capacity = CA_POOL_MIN_CHUNK_ELEMENTS;
    if (capacity > CA_POOL_MAX_CHUNK_ELEMENTS)
        capacity = CA_POOL_MAX_CHUNK_ELEMENTS;
    return capacity;
}

bool ca_pool_init(Ca_Pool *pool, size_t element_size, size_t chunk_capacity)
{
    if (!pool || element_size == 0 || chunk_capacity == 0 ||
        chunk_capacity > SIZE_MAX / element_size)
        return false;
    *pool = (Ca_Pool){
        .chunks = CA_DYN_ARRAY_INIT(Ca_PoolChunk),
        .free_indices = CA_DYN_ARRAY_INIT(size_t),
        .element_size = element_size,
        .chunk_capacity = chunk_capacity,
    };
    return true;
}

void ca_pool_destroy(Ca_Pool *pool, Ca_PoolElementDestroyFn destroy_fn,
                     void *user_data)
{
    if (!pool) return;
    if (pool_valid(pool)) {
        for (size_t index = 0; index < pool->slot_count; ++index) {
            if (destroy_fn && ca_pool_slot_live(pool, index))
                destroy_fn(ca_pool_at(pool, index), user_data);
        }
        for (size_t i = 0; i < pool->chunks.count; ++i) {
            Ca_PoolChunk *chunk = ca_dyn_array_at(&pool->chunks, i);
            CA_FREE(chunk->occupied);
            CA_FREE(chunk->data);
        }
    }
    ca_dyn_array_destroy(&pool->free_indices);
    ca_dyn_array_destroy(&pool->chunks);
    pool->element_size = 0;
    pool->chunk_capacity = 0;
    pool->slot_count = 0;
    pool->live_count = 0;
}

void *ca_pool_acquire(Ca_Pool *pool)
{
    if (!pool_valid(pool)) return NULL;
    size_t index = 0;
    if (pool->free_indices.count > 0) {
        if (!ca_dyn_array_pop(&pool->free_indices, &index)) return NULL;
    } else {
        if (pool->slot_count == pool->chunks.count * pool->chunk_capacity &&
            !pool_append_chunk(pool))
            return NULL;
        index = pool->slot_count++;
    }

    size_t chunk_index = index / pool->chunk_capacity;
    size_t local_index = index % pool->chunk_capacity;
    Ca_PoolChunk *chunk = ca_dyn_array_at(&pool->chunks, chunk_index);
    if (!chunk || chunk->occupied[local_index]) return NULL;
    chunk->occupied[local_index] = 1u;
    pool->live_count++;
    void *element = chunk->data + local_index * pool->element_size;
    memset(element, 0, pool->element_size);
    return element;
}

bool ca_pool_release(Ca_Pool *pool, void *element)
{
    size_t index = 0;
    if (!pool_find_index(pool, element, &index)) return false;
    size_t chunk_index = index / pool->chunk_capacity;
    size_t local_index = index % pool->chunk_capacity;
    Ca_PoolChunk *chunk = ca_dyn_array_at(&pool->chunks, chunk_index);
    if (!chunk || !chunk->occupied[local_index] || pool->live_count == 0)
        return false;
    if (!ca_dyn_array_push(&pool->free_indices, &index)) return false;
    chunk->occupied[local_index] = 0u;
    pool->live_count--;
    memset(element, 0, pool->element_size);
    return true;
}

void *ca_pool_at(Ca_Pool *pool, size_t index)
{
    if (!pool_valid(pool) || index >= pool->slot_count) return NULL;
    Ca_PoolChunk *chunk = ca_dyn_array_at(
        &pool->chunks, index / pool->chunk_capacity);
    if (!chunk) return NULL;
    return chunk->data + (index % pool->chunk_capacity) * pool->element_size;
}

const void *ca_pool_at_const(const Ca_Pool *pool, size_t index)
{
    return ca_pool_at((Ca_Pool *)pool, index);
}

bool ca_pool_slot_live(const Ca_Pool *pool, size_t index)
{
    if (!pool_valid(pool) || index >= pool->slot_count) return false;
    const Ca_PoolChunk *chunk = ca_dyn_array_at_const(
        &pool->chunks, index / pool->chunk_capacity);
    return chunk && chunk->occupied[index % pool->chunk_capacity] != 0;
}

bool ca_pool_index(const Ca_Pool *pool, const void *element,
                   size_t *out_index)
{
    return pool_find_index(pool, element, out_index);
}

size_t ca_pool_slot_count(const Ca_Pool *pool)
{
    return pool_valid(pool) ? pool->slot_count : 0u;
}

size_t ca_pool_live_count(const Ca_Pool *pool)
{
    return pool_valid(pool) ? pool->live_count : 0u;
}
