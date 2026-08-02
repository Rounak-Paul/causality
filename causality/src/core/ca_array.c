// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "ca_array.h"

#include "causality_config.h"

#include <stdint.h>
#include <string.h>

#define CA_DYN_ARRAY_MIN_CAPACITY 8u

/** Computes a checked byte size for an element count. */
static bool array_byte_size(const Ca_DynArray *array, size_t count,
                            size_t *out_size)
{
    if (!array || !out_size || array->element_size == 0 ||
        count > SIZE_MAX / array->element_size)
        return false;
    *out_size = count * array->element_size;
    return true;
}

/** Reports whether two byte ranges overlap. */
static bool byte_ranges_overlap(const void *first, size_t first_size,
                                const void *second, size_t second_size)
{
    if (!first || !second || first_size == 0 || second_size == 0) return false;
    uintptr_t first_begin = (uintptr_t)first;
    uintptr_t second_begin = (uintptr_t)second;
    if (first_begin > UINTPTR_MAX - first_size ||
        second_begin > UINTPTR_MAX - second_size)
        return true;
    return first_begin < second_begin + second_size &&
           second_begin < first_begin + first_size;
}

/** Computes a checked geometric growth capacity. */
static bool array_growth_capacity(const Ca_DynArray *array, size_t minimum,
                                  size_t *out_capacity)
{
    size_t maximum = SIZE_MAX / array->element_size;
    if (!out_capacity || minimum > maximum) return false;
    size_t capacity = array->capacity;
    if (capacity >= minimum) {
        *out_capacity = capacity;
        return true;
    }
    if (capacity < CA_DYN_ARRAY_MIN_CAPACITY)
        capacity = minimum < CA_DYN_ARRAY_MIN_CAPACITY
            ? CA_DYN_ARRAY_MIN_CAPACITY : minimum;
    while (capacity < minimum) {
        size_t growth = capacity / 2u;
        if (growth < CA_DYN_ARRAY_MIN_CAPACITY)
            growth = CA_DYN_ARRAY_MIN_CAPACITY;
        if (capacity > maximum - growth) {
            capacity = maximum;
            break;
        }
        capacity += growth;
    }
    if (capacity < minimum) return false;
    *out_capacity = capacity;
    return true;
}

/** Preserves a logical, aliased source before a mutating operation. */
static bool array_prepare_source(const Ca_DynArray *array, const void *elements,
                                 size_t count, const void **out_source,
                                 void **out_temporary)
{
    if (!out_source || !out_temporary) return false;
    *out_source = elements;
    *out_temporary = NULL;
    if (count == 0) return true;
    if (!elements) return false;

    size_t source_bytes = 0;
    size_t allocation_bytes = 0;
    size_t logical_bytes = 0;
    if (!array_byte_size(array, count, &source_bytes) ||
        !array_byte_size(array, array->capacity, &allocation_bytes) ||
        !array_byte_size(array, array->count, &logical_bytes))
        return false;
    if (!byte_ranges_overlap(elements, source_bytes,
                             array->data, allocation_bytes))
        return true;

    uintptr_t data_begin = (uintptr_t)array->data;
    uintptr_t source_begin = (uintptr_t)elements;
    if (data_begin > UINTPTR_MAX - logical_bytes ||
        source_begin < data_begin ||
        source_begin > UINTPTR_MAX - source_bytes ||
        source_begin + source_bytes > data_begin + logical_bytes ||
        (source_begin - data_begin) % array->element_size != 0)
        return false;

    void *temporary = CA_MALLOC(source_bytes);
    if (!temporary) return false;
    memcpy(temporary, elements, source_bytes);
    *out_source = temporary;
    *out_temporary = temporary;
    return true;
}

bool ca_dyn_array_init(Ca_DynArray *array, size_t element_size)
{
    if (!array || element_size == 0) return false;
    *array = (Ca_DynArray){ NULL, 0u, 0u, element_size };
    return true;
}

bool ca_dyn_array_valid(const Ca_DynArray *array)
{
    if (!array || array->element_size == 0 ||
        array->count > array->capacity ||
        array->capacity > SIZE_MAX / array->element_size)
        return false;
    return (array->data == NULL) == (array->capacity == 0);
}

void ca_dyn_array_destroy(Ca_DynArray *array)
{
    if (!array) return;
    CA_FREE(array->data);
    array->data = NULL;
    array->count = 0;
    array->capacity = 0;
}

void ca_dyn_array_clear(Ca_DynArray *array)
{
    if (ca_dyn_array_valid(array)) array->count = 0;
}

bool ca_dyn_array_reserve(Ca_DynArray *array, size_t minimum_capacity)
{
    if (!ca_dyn_array_valid(array)) return false;
    if (minimum_capacity <= array->capacity) return true;
    size_t capacity = 0;
    size_t bytes = 0;
    if (!array_growth_capacity(array, minimum_capacity, &capacity) ||
        !array_byte_size(array, capacity, &bytes))
        return false;
    void *data = CA_REALLOC(array->data, bytes);
    if (!data) return false;
    array->data = data;
    array->capacity = capacity;
    return true;
}

bool ca_dyn_array_resize(Ca_DynArray *array, size_t count)
{
    if (!ca_dyn_array_valid(array)) return false;
    size_t old_bytes = 0;
    size_t new_bytes = 0;
    if (!array_byte_size(array, array->count, &old_bytes) ||
        !array_byte_size(array, count, &new_bytes))
        return false;
    if (count > array->capacity && !ca_dyn_array_reserve(array, count))
        return false;
    if (count > array->count)
        memset((unsigned char *)array->data + old_bytes, 0,
               new_bytes - old_bytes);
    array->count = count;
    return true;
}

bool ca_dyn_array_shrink_to_fit(Ca_DynArray *array)
{
    if (!ca_dyn_array_valid(array)) return false;
    if (array->count == array->capacity) return true;
    if (array->count == 0) {
        CA_FREE(array->data);
        array->data = NULL;
        array->capacity = 0;
        return true;
    }
    size_t bytes = 0;
    if (!array_byte_size(array, array->count, &bytes)) return false;
    void *data = CA_REALLOC(array->data, bytes);
    if (!data) return false;
    array->data = data;
    array->capacity = array->count;
    return true;
}

bool ca_dyn_array_assign(Ca_DynArray *array, const void *elements,
                         size_t count)
{
    if (!ca_dyn_array_valid(array) || (count > 0 && !elements)) return false;
    const void *source = NULL;
    void *temporary = NULL;
    if (!array_prepare_source(array, elements, count, &source, &temporary))
        return false;
    if (count > array->capacity && !ca_dyn_array_reserve(array, count)) {
        CA_FREE(temporary);
        return false;
    }
    size_t bytes = 0;
    bool sized = array_byte_size(array, count, &bytes);
    if (sized && bytes > 0) memcpy(array->data, source, bytes);
    if (sized) array->count = count;
    CA_FREE(temporary);
    return sized;
}

bool ca_dyn_array_copy(Ca_DynArray *destination, const Ca_DynArray *source)
{
    if (!ca_dyn_array_valid(destination) || !ca_dyn_array_valid(source) ||
        destination->element_size != source->element_size)
        return false;
    if (destination == source) return true;
    return ca_dyn_array_assign(destination, source->data, source->count);
}

bool ca_dyn_array_append(Ca_DynArray *array, const void *elements, size_t count)
{
    if (!array) return false;
    return ca_dyn_array_insert(array, array->count, elements, count);
}

bool ca_dyn_array_push(Ca_DynArray *array, const void *element)
{
    return ca_dyn_array_append(array, element, 1u);
}

bool ca_dyn_array_insert(Ca_DynArray *array, size_t index,
                         const void *elements, size_t count)
{
    if (!ca_dyn_array_valid(array) || index > array->count ||
        (count > 0 && !elements))
        return false;
    if (count == 0) return true;
    if (count > SIZE_MAX - array->count) return false;
    size_t new_count = array->count + count;
    size_t insert_bytes = 0;
    size_t tail_bytes = 0;
    size_t index_bytes = 0;
    if (!array_byte_size(array, count, &insert_bytes) ||
        !array_byte_size(array, array->count - index, &tail_bytes) ||
        !array_byte_size(array, index, &index_bytes))
        return false;

    const void *source = NULL;
    void *temporary = NULL;
    if (!array_prepare_source(array, elements, count, &source, &temporary))
        return false;
    if (!ca_dyn_array_reserve(array, new_count)) {
        CA_FREE(temporary);
        return false;
    }
    unsigned char *destination = (unsigned char *)array->data + index_bytes;
    memmove(destination + insert_bytes, destination, tail_bytes);
    memcpy(destination, source, insert_bytes);
    array->count = new_count;
    CA_FREE(temporary);
    return true;
}

bool ca_dyn_array_insert_zeroed(Ca_DynArray *array, size_t index, size_t count)
{
    if (!ca_dyn_array_valid(array) || index > array->count) return false;
    if (count == 0) return true;
    if (count > SIZE_MAX - array->count) return false;
    size_t insert_bytes = 0;
    size_t tail_bytes = 0;
    size_t index_bytes = 0;
    if (!array_byte_size(array, count, &insert_bytes) ||
        !array_byte_size(array, array->count - index, &tail_bytes) ||
        !array_byte_size(array, index, &index_bytes) ||
        !ca_dyn_array_reserve(array, array->count + count))
        return false;
    unsigned char *destination = (unsigned char *)array->data + index_bytes;
    memmove(destination + insert_bytes, destination, tail_bytes);
    memset(destination, 0, insert_bytes);
    array->count += count;
    return true;
}

bool ca_dyn_array_erase(Ca_DynArray *array, size_t index, size_t count)
{
    if (!ca_dyn_array_valid(array) || index > array->count ||
        count > array->count - index)
        return false;
    if (count == 0) return true;
    size_t destination_bytes = 0;
    size_t removed_bytes = 0;
    size_t tail_bytes = 0;
    if (!array_byte_size(array, index, &destination_bytes) ||
        !array_byte_size(array, count, &removed_bytes) ||
        !array_byte_size(array, array->count - index - count, &tail_bytes))
        return false;
    unsigned char *destination = (unsigned char *)array->data + destination_bytes;
    memmove(destination, destination + removed_bytes, tail_bytes);
    array->count -= count;
    return true;
}

bool ca_dyn_array_erase_unordered(Ca_DynArray *array, size_t index)
{
    if (!ca_dyn_array_valid(array) || index >= array->count) return false;
    if (index + 1u < array->count) {
        void *destination = (unsigned char *)array->data +
                            index * array->element_size;
        const void *source = (const unsigned char *)array->data +
                             (array->count - 1u) * array->element_size;
        memcpy(destination, source, array->element_size);
    }
    array->count--;
    return true;
}

bool ca_dyn_array_pop(Ca_DynArray *array, void *out_element)
{
    if (!ca_dyn_array_valid(array) || array->count == 0) return false;
    const void *element = (const unsigned char *)array->data +
                          (array->count - 1u) * array->element_size;
    if (out_element) memmove(out_element, element, array->element_size);
    array->count--;
    return true;
}

void *ca_dyn_array_at(Ca_DynArray *array, size_t index)
{
    if (!ca_dyn_array_valid(array) || index >= array->count) return NULL;
    return (unsigned char *)array->data + index * array->element_size;
}

const void *ca_dyn_array_at_const(const Ca_DynArray *array, size_t index)
{
    if (!ca_dyn_array_valid(array) || index >= array->count) return NULL;
    return (const unsigned char *)array->data + index * array->element_size;
}

void *ca_dyn_array_front(Ca_DynArray *array)
{
    return ca_dyn_array_at(array, 0u);
}

void *ca_dyn_array_back(Ca_DynArray *array)
{
    if (!ca_dyn_array_valid(array) || array->count == 0) return NULL;
    return ca_dyn_array_at(array, array->count - 1u);
}

void *ca_dyn_array_release(Ca_DynArray *array, size_t *out_count,
                           size_t *out_capacity)
{
    if (!ca_dyn_array_valid(array)) return NULL;
    void *data = array->data;
    if (out_count) *out_count = array->count;
    if (out_capacity) *out_capacity = array->capacity;
    array->data = NULL;
    array->count = 0;
    array->capacity = 0;
    return data;
}

void ca_dyn_array_swap(Ca_DynArray *first, Ca_DynArray *second)
{
    if (!first || !second || first == second) return;
    Ca_DynArray temporary = *first;
    *first = *second;
    *second = temporary;
}
