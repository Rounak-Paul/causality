// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#pragma once

#include "ca_array.h"

#include <stdbool.h>
#include <stddef.h>

typedef void (*Ca_PoolElementDestroyFn)(void *element, void *user_data);

/** Owns chunked element storage whose live element addresses never move. */
typedef struct Ca_Pool {
    Ca_DynArray chunks;
    Ca_DynArray free_indices;
    size_t      element_size;
    size_t      chunk_capacity;
    size_t      slot_count;
    size_t      live_count;
} Ca_Pool;

/**
 * Initializes an empty stable-address pool.
 *
 * @param pool Pool storage to initialize.
 * @param element_size Size of one element in bytes.
 * @param chunk_capacity Number of elements allocated per chunk.
 * @return True when the arguments are valid.
 */
bool ca_pool_init(Ca_Pool *pool, size_t element_size, size_t chunk_capacity);

/**
 * Destroys every chunk and optionally finalizes live elements first.
 *
 * @param pool Pool to destroy, or NULL.
 * @param destroy_fn Optional live-element finalizer.
 * @param user_data User value passed to the finalizer.
 */
void ca_pool_destroy(Ca_Pool *pool, Ca_PoolElementDestroyFn destroy_fn,
                     void *user_data);

/**
 * Acquires one zero-initialized, pointer-stable element.
 *
 * @param pool Initialized pool.
 * @return Element pointer, or NULL when allocation fails.
 */
void *ca_pool_acquire(Ca_Pool *pool);

/**
 * Returns a live element to the pool and zeroes its storage.
 *
 * @param pool Initialized pool.
 * @param element Element previously acquired from this pool.
 * @return True when a live element was released.
 */
bool ca_pool_release(Ca_Pool *pool, void *element);

/**
 * Returns a slot by stable creation index, including currently free slots.
 *
 * @param pool Initialized pool.
 * @param index Slot index in the range [0, slot_count).
 * @return Slot pointer, or NULL for an invalid index or pool.
 */
void *ca_pool_at(Ca_Pool *pool, size_t index);

/** Read-only variant of ca_pool_at. */
const void *ca_pool_at_const(const Ca_Pool *pool, size_t index);

/** Returns whether a slot index currently names a live element. */
bool ca_pool_slot_live(const Ca_Pool *pool, size_t index);

/**
 * Resolves a pool-owned element to its stable creation index.
 *
 * @param pool Initialized pool.
 * @param element Element previously acquired from the pool.
 * @param out_index Destination for the slot index.
 * @return True when the pointer belongs to a published pool slot.
 */
bool ca_pool_index(const Ca_Pool *pool, const void *element,
                   size_t *out_index);

/** Returns the high-water slot count used for indexed iteration. */
size_t ca_pool_slot_count(const Ca_Pool *pool);

/** Returns the number of currently live elements. */
size_t ca_pool_live_count(const Ca_Pool *pool);

/** Selects a cache-conscious default chunk size for an element type. */
size_t ca_pool_recommended_chunk_capacity(size_t element_size);

#define CA_POOL_AT(pool, type, index) \
    ((type *)ca_pool_at(&(pool), (index)))

#define CA_POOL_AT_CONST(pool, type, index) \
    ((const type *)ca_pool_at_const(&(pool), (index)))
