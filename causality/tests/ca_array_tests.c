// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "ca_array.h"
#include "ca_pool.h"
#include "causality.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                  \
                    __FILE__, __LINE__, #condition);                            \
            return false;                                                       \
        }                                                                       \
    } while (0)

static size_t g_allocation_attempts;
static size_t g_fail_at_attempt;

/** Allocates test storage and optionally injects one deterministic failure. */
static void *test_malloc(size_t size)
{
    g_allocation_attempts++;
    if (g_fail_at_attempt == g_allocation_attempts) return NULL;
    return malloc(size);
}

/** Allocates zeroed test storage and optionally injects a failure. */
static void *test_calloc(size_t count, size_t size)
{
    g_allocation_attempts++;
    if (g_fail_at_attempt == g_allocation_attempts) return NULL;
    return calloc(count, size);
}

/** Resizes test storage and optionally injects a failure. */
static void *test_realloc(void *data, size_t size)
{
    g_allocation_attempts++;
    if (g_fail_at_attempt == g_allocation_attempts) return NULL;
    return realloc(data, size);
}

/** Releases test storage. */
static void test_free(void *data)
{
    free(data);
}

/** Resets allocator hooks and fault-injection counters. */
static void reset_allocator(void)
{
    ca_set_allocator(NULL, NULL, NULL, NULL);
    g_allocation_attempts = 0;
    g_fail_at_attempt = 0;
}

/** Verifies zero-cost initialization, resize zeroing, and retained storage. */
static bool test_array_lifecycle(void)
{
    Ca_DynArray array;
    CHECK(!ca_dyn_array_init(NULL, sizeof(uint64_t)));
    CHECK(!ca_dyn_array_init(&array, 0));
    CHECK(ca_dyn_array_init(&array, sizeof(uint64_t)));
    CHECK(ca_dyn_array_valid(&array));
    CHECK(array.data == NULL && array.count == 0 && array.capacity == 0);
    CHECK(ca_dyn_array_resize(&array, 3));
    for (size_t i = 0; i < array.count; ++i)
        CHECK(*(uint64_t *)ca_dyn_array_at(&array, i) == 0);
    *(uint64_t *)ca_dyn_array_at(&array, 1) = UINT64_C(0x123456789abcdef0);
    void *retained_data = array.data;
    size_t retained_capacity = array.capacity;
    ca_dyn_array_clear(&array);
    CHECK(array.data == retained_data && array.capacity == retained_capacity);
    CHECK(ca_dyn_array_resize(&array, 2));
    CHECK(*(uint64_t *)ca_dyn_array_front(&array) == 0);
    CHECK(*(uint64_t *)ca_dyn_array_back(&array) == 0);
    ca_dyn_array_clear(&array);
    CHECK(ca_dyn_array_shrink_to_fit(&array));
    CHECK(array.data == NULL && array.capacity == 0);
    ca_dyn_array_destroy(&array);
    CHECK(ca_dyn_array_valid(&array));
    return true;
}

/** Verifies growth, ordering, alias handling, and all mutation operations. */
static bool test_array_mutations(void)
{
    Ca_DynArray array = CA_DYN_ARRAY_INIT(int);
    for (int i = 0; i < 10000; ++i) CHECK(ca_dyn_array_push(&array, &i));
    for (int i = 0; i < 10000; ++i)
        CHECK(*(const int *)ca_dyn_array_at_const(&array, (size_t)i) == i);

    const int *aliased = ca_dyn_array_at_const(&array, 250);
    CHECK(ca_dyn_array_insert(&array, 4, aliased, 50));
    for (int i = 0; i < 50; ++i)
        CHECK(*(int *)ca_dyn_array_at(&array, (size_t)i + 4u) == i + 250);
    CHECK(ca_dyn_array_erase(&array, 4, 50));
    CHECK(ca_dyn_array_erase_unordered(&array, 123));
    CHECK(array.count == 9999);
    int final_value = -1;
    CHECK(ca_dyn_array_pop(&array, &final_value));
    CHECK(ca_dyn_array_insert_zeroed(&array, 2, 3));
    CHECK(*(int *)ca_dyn_array_at(&array, 2) == 0);

    Ca_DynArray copy = CA_DYN_ARRAY_INIT(int);
    CHECK(ca_dyn_array_copy(&copy, &array));
    CHECK(copy.count == array.count);
    CHECK(memcmp(copy.data, array.data, array.count * sizeof(int)) == 0);
    ca_dyn_array_swap(&copy, &array);
    size_t count = 0;
    size_t capacity = 0;
    void *released = ca_dyn_array_release(&copy, &count, &capacity);
    CHECK(released && count == 10001 && capacity >= count);
    CA_FREE(released);
    ca_dyn_array_destroy(&copy);
    ca_dyn_array_destroy(&array);
    return true;
}

/** Verifies invalid and overflowing operations preserve existing contents. */
static bool test_array_failure_atomicity(void)
{
    Ca_DynArray array = CA_DYN_ARRAY_INIT(uint64_t);
    const uint64_t values[] = { 7, 8, 9 };
    CHECK(ca_dyn_array_assign(&array, values, 3));
    void *data = array.data;
    size_t count = array.count;
    size_t capacity = array.capacity;
    CHECK(!ca_dyn_array_reserve(&array, SIZE_MAX));
    CHECK(!ca_dyn_array_resize(&array, SIZE_MAX));
    CHECK(!ca_dyn_array_append(&array, NULL, 1));
    CHECK(!ca_dyn_array_insert(&array, 9, values, 1));
    CHECK(array.data == data && array.count == count &&
          array.capacity == capacity);
    CHECK(memcmp(array.data, values, sizeof(values)) == 0);

    ca_set_allocator(test_malloc, test_calloc, test_realloc, test_free);
    g_allocation_attempts = 0;
    g_fail_at_attempt = 1;
    CHECK(!ca_dyn_array_reserve(&array, capacity + 100));
    CHECK(array.data == data && array.count == count &&
          array.capacity == capacity);
    CHECK(memcmp(array.data, values, sizeof(values)) == 0);
    reset_allocator();
    ca_dyn_array_destroy(&array);
    return true;
}

typedef struct TestObject {
    uint64_t identity;
    unsigned char payload[120];
} TestObject;

/** Verifies pool address stability, reuse, validation, and chunk growth. */
static bool test_pool_lifecycle(void)
{
    Ca_Pool pool;
    CHECK(ca_pool_init(&pool, sizeof(TestObject), 8));
    TestObject *first = ca_pool_acquire(&pool);
    CHECK(first);
    first->identity = UINT64_C(0xfeedface);
    TestObject *objects[257] = { first };
    for (size_t i = 1; i < 257; ++i) {
        objects[i] = ca_pool_acquire(&pool);
        CHECK(objects[i]);
        objects[i]->identity = i;
    }
    CHECK(ca_pool_live_count(&pool) == 257);
    CHECK(ca_pool_slot_count(&pool) == 257);
    CHECK(first->identity == UINT64_C(0xfeedface));
    CHECK(ca_pool_at(&pool, 0) == first);
    CHECK(ca_pool_at(&pool, 257) == NULL);

    TestObject *released = objects[111];
    CHECK(ca_pool_release(&pool, released));
    CHECK(!ca_pool_release(&pool, released));
    CHECK(!ca_pool_release(&pool, &pool));
    CHECK(ca_pool_live_count(&pool) == 256);
    TestObject *reused = ca_pool_acquire(&pool);
    CHECK(reused == released);
    CHECK(reused->identity == 0);
    CHECK(first->identity == UINT64_C(0xfeedface));
    ca_pool_destroy(&pool, NULL, NULL);
    return true;
}

/** Verifies failed chunk growth does not publish partial pool state. */
static bool test_pool_failure_atomicity(void)
{
    Ca_Pool pool;
    CHECK(ca_pool_init(&pool, sizeof(TestObject), 8));
    for (size_t i = 0; i < 8; ++i) CHECK(ca_pool_acquire(&pool));
    size_t slots = ca_pool_slot_count(&pool);
    size_t live = ca_pool_live_count(&pool);

    ca_set_allocator(test_malloc, test_calloc, test_realloc, test_free);
    g_allocation_attempts = 0;
    g_fail_at_attempt = 1;
    CHECK(ca_pool_acquire(&pool) == NULL);
    CHECK(ca_pool_slot_count(&pool) == slots);
    CHECK(ca_pool_live_count(&pool) == live);
    reset_allocator();
    ca_pool_destroy(&pool, NULL, NULL);
    return true;
}

int main(void)
{
    bool ok = test_array_lifecycle() &&
              test_array_mutations() &&
              test_array_failure_atomicity() &&
              test_pool_lifecycle() &&
              test_pool_failure_atomicity();
    reset_allocator();
    if (!ok) return 1;
    puts("causality array and pool tests passed");
    return 0;
}
