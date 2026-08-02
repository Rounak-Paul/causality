// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ca_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Owns a contiguous, growable sequence of equally sized values. */
typedef struct Ca_DynArray {
    void   *data;
    size_t  count;
    size_t  capacity;
    size_t  element_size;
} Ca_DynArray;

/** Initializes a typed empty array without allocating storage. */
#define CA_DYN_ARRAY_INIT(type) { NULL, 0u, 0u, sizeof(type) }

/**
 * Initializes empty array storage.
 *
 * @param array Array storage to initialize.
 * @param element_size Size of one element in bytes.
 * @return True when the arguments are valid.
 */
CA_API bool ca_dyn_array_init(Ca_DynArray *array, size_t element_size);

/**
 * Checks whether array metadata describes a valid initialized state.
 *
 * @param array Array to inspect.
 * @return True when the array is structurally valid.
 */
CA_API bool ca_dyn_array_valid(const Ca_DynArray *array);

/**
 * Releases owned storage while preserving the configured element size.
 *
 * @param array Array to destroy, or NULL.
 */
CA_API void ca_dyn_array_destroy(Ca_DynArray *array);

/**
 * Removes every logical element while retaining storage.
 *
 * @param array Initialized array.
 */
CA_API void ca_dyn_array_clear(Ca_DynArray *array);

/**
 * Ensures capacity for at least the requested number of elements.
 *
 * @param array Initialized array.
 * @param minimum_capacity Required element capacity.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_reserve(Ca_DynArray *array, size_t minimum_capacity);

/**
 * Changes the logical count, zero-initializing newly exposed elements.
 *
 * @param array Initialized array.
 * @param count New logical element count.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_resize(Ca_DynArray *array, size_t count);

/**
 * Reduces capacity to the current count.
 *
 * @param array Initialized array.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_shrink_to_fit(Ca_DynArray *array);

/**
 * Replaces the contents with a sequence of values.
 *
 * @param array Initialized destination array.
 * @param elements Source values, or NULL when count is zero.
 * @param count Number of source elements.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_assign(Ca_DynArray *array, const void *elements,
                                size_t count);

/**
 * Copies a compatible array into another array.
 *
 * @param destination Initialized destination array.
 * @param source Initialized source array with the same element size.
 * @return True on success; false leaves the destination unchanged.
 */
CA_API bool ca_dyn_array_copy(Ca_DynArray *destination,
                              const Ca_DynArray *source);

/**
 * Appends a sequence of values.
 *
 * @param array Initialized destination array.
 * @param elements Source values, or NULL when count is zero.
 * @param count Number of values to append.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_append(Ca_DynArray *array, const void *elements,
                                size_t count);

/**
 * Appends one value.
 *
 * @param array Initialized destination array.
 * @param element Source value matching the configured element size.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_push(Ca_DynArray *array, const void *element);

/**
 * Inserts values before a position or at the end.
 *
 * @param array Initialized destination array.
 * @param index Insertion position in the inclusive range [0, count].
 * @param elements Source values, or NULL when count is zero.
 * @param count Number of values to insert.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_insert(Ca_DynArray *array, size_t index,
                                const void *elements, size_t count);

/**
 * Inserts zero-initialized elements before a position or at the end.
 *
 * @param array Initialized destination array.
 * @param index Insertion position in the inclusive range [0, count].
 * @param count Number of elements to insert.
 * @return True on success; false leaves the array unchanged.
 */
CA_API bool ca_dyn_array_insert_zeroed(Ca_DynArray *array, size_t index,
                                       size_t count);

/**
 * Removes a range while preserving element order.
 *
 * @param array Initialized array.
 * @param index First element to remove.
 * @param count Number of elements to remove.
 * @return True when the range is valid and was removed.
 */
CA_API bool ca_dyn_array_erase(Ca_DynArray *array, size_t index, size_t count);

/**
 * Removes one element by moving the final element into its slot.
 *
 * @param array Initialized array.
 * @param index Element to remove.
 * @return True when the index was valid.
 */
CA_API bool ca_dyn_array_erase_unordered(Ca_DynArray *array, size_t index);

/**
 * Removes the final element and optionally copies it to the caller.
 *
 * @param array Initialized array.
 * @param out_element Optional destination for one element.
 * @return True when an element was removed.
 */
CA_API bool ca_dyn_array_pop(Ca_DynArray *array, void *out_element);

/**
 * Returns a mutable element pointer.
 *
 * @param array Initialized array.
 * @param index Zero-based element index.
 * @return Element pointer, or NULL for an invalid index or array.
 */
CA_API void *ca_dyn_array_at(Ca_DynArray *array, size_t index);

/**
 * Returns a read-only element pointer.
 *
 * @param array Initialized array.
 * @param index Zero-based element index.
 * @return Element pointer, or NULL for an invalid index or array.
 */
CA_API const void *ca_dyn_array_at_const(const Ca_DynArray *array,
                                         size_t index);

/** Returns the first element, or NULL when empty or invalid. */
CA_API void *ca_dyn_array_front(Ca_DynArray *array);

/** Returns the final element, or NULL when empty or invalid. */
CA_API void *ca_dyn_array_back(Ca_DynArray *array);

/**
 * Transfers the backing allocation to the caller and empties the array.
 *
 * @param array Initialized array.
 * @param out_count Optional destination for the logical element count.
 * @param out_capacity Optional destination for the allocation capacity.
 * @return Previous allocation, which must be released with CA_FREE.
 */
CA_API void *ca_dyn_array_release(Ca_DynArray *array, size_t *out_count,
                                  size_t *out_capacity);

/** Exchanges two complete array values without allocating. */
CA_API void ca_dyn_array_swap(Ca_DynArray *first, Ca_DynArray *second);

#ifdef __cplusplus
}
#endif
