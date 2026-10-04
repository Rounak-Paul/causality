// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "ca_reactive.h"
#include "ca_internal.h"
#include <stdio.h>
#include <stdlib.h>

#include "../src/reactive/reactive.h"

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); return false; } } while (0)

/** Holds callback mutation actions and execution counts. */
typedef struct EffectProbe {
    Ca_Instance *instance;
    Ca_Effect *self;
    Ca_Effect *remove;
    Ca_Effect *created;
    struct EffectProbe *child;
    Ca_Signal *signal;
    Ca_Signal *read_after;
    int calls;
    int active;
    int max_active;
    bool armed;
    bool destroy_self;
    size_t spawn_count;
} EffectProbe;

/** Tracks signal reads and optionally destroys or creates effects during execution. */
static void observe(void *user_data)
{
    EffectProbe *probe = user_data;
    ++probe->calls;
    ++probe->active;
    if (probe->active > probe->max_active) probe->max_active = probe->active;
    if (probe->signal) (void)ca_signal_get_int(probe->signal);
    if (probe->armed) {
        probe->armed = false;
        if (probe->remove) ca_effect_destroy(probe->remove);
        if (probe->destroy_self) ca_effect_destroy(probe->self);
        if (probe->child) {
            size_t count = probe->spawn_count ? probe->spawn_count : 1;
            for (size_t i = 0; i < count; ++i) {
                probe->created = ca_frame_effect(probe->instance, observe, probe->child);
                if (!probe->created) break;
            }
        }
        if (probe->signal) ca_signal_set_int(probe->signal, 2);
    }
    if (probe->read_after) (void)ca_signal_get_int(probe->read_after);
    --probe->active;
}

/** Verifies self-removal cannot skip a surviving frame effect. */
static bool test_frame_removal(void)
{
    Ca_Instance instance = {0};
    EffectProbe first = { .instance = &instance, .destroy_self = true };
    EffectProbe second = { .instance = &instance };
    EffectProbe third = { .instance = &instance };
    first.self = ca_frame_effect(&instance, observe, &first);
    second.self = ca_frame_effect(&instance, observe, &second);
    third.self = ca_frame_effect(&instance, observe, &third);
    CHECK(first.self && second.self && third.self);
    first.armed = true;
    ca_reactive_run_frame_effects(&instance);
    CHECK(first.calls == 2 && second.calls == 2 && third.calls == 2);
    ca_reactive_run_frame_effects(&instance);
    CHECK(first.calls == 2 && second.calls == 3 && third.calls == 3);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Verifies newly registered frame effects receive only their immediate run this frame. */
static bool test_frame_addition(void)
{
    Ca_Instance instance = {0};
    EffectProbe child = { .instance = &instance };
    EffectProbe parent = { .instance = &instance, .child = &child };
    parent.self = ca_frame_effect(&instance, observe, &parent);
    CHECK(parent.self);
    parent.armed = true;
    ca_reactive_run_frame_effects(&instance);
    CHECK(parent.created && parent.calls == 2 && child.calls == 1);
    ca_reactive_run_frame_effects(&instance);
    CHECK(parent.calls == 3 && child.calls == 2);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Verifies iteration remains stable across frame-array and effect-pool growth. */
static bool test_frame_growth(void)
{
    Ca_Instance instance = {0};
    EffectProbe child = { .instance = &instance };
    EffectProbe parent = { .instance = &instance, .child = &child, .spawn_count = 512 };
    CHECK(ca_frame_effect(&instance, observe, &parent));
    parent.armed = true;
    ca_reactive_run_frame_effects(&instance);
    CHECK(parent.created && parent.calls == 2 && child.calls == 512);
    ca_reactive_run_frame_effects(&instance);
    CHECK(parent.calls == 3 && child.calls == 1024);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Verifies writes inside an effect never recursively enter that effect. */
static bool test_reentrant_notification(void)
{
    Ca_Instance instance = {0};
    Ca_Signal *signal = ca_signal_int(&instance, 0);
    CHECK(signal);
    EffectProbe probe = { .instance = &instance, .signal = signal };
    probe.self = ca_effect(&instance, observe, &probe);
    CHECK(probe.self);
    probe.armed = true;
    ca_signal_set_int(signal, 1);
    CHECK(probe.calls == 3 && probe.max_active == 1);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Holds a computed dependency and its stable result buffer. */
typedef struct ComputedProbe {
    Ca_Signal *source;
    int result;
    int calls;
} ComputedProbe;

/** Derives twice the source value without changing reactive state. */
static const void *compute_double(void *user_data)
{
    ComputedProbe *probe = user_data;
    ++probe->calls;
    probe->result = 2 * ca_signal_get_int(probe->source);
    return &probe->result;
}

/** Records the current derived value when its dependency invalidates. */
static void observe_computed(void *user_data)
{
    ComputedProbe *probe = user_data;
    ++probe->calls;
    probe->result = ca_signal_get_int(probe->source);
}

/** Verifies derived chains notify leaves and reads inside a batch are current. */
static bool test_computed_chain(void)
{
    Ca_Instance instance = {0};
    Ca_Signal *source = ca_signal_int(&instance, 1);
    CHECK(source);
    ComputedProbe first = { .source = source };
    Ca_Signal *twice = ca_computed(&instance, sizeof(int), compute_double, &first);
    CHECK(twice);
    ComputedProbe second = { .source = twice };
    Ca_Signal *four_times = ca_computed(&instance, sizeof(int), compute_double, &second);
    CHECK(four_times);
    ComputedProbe observer = { .source = four_times };
    CHECK(ca_effect(&instance, observe_computed, &observer));
    CHECK(observer.calls == 1 && observer.result == 4);
    CHECK(first.calls == 1 && second.calls == 1);
    ca_signal_set_int(source, 2);
    CHECK(observer.calls == 2 && observer.result == 8);
    CHECK(first.calls == 2 && second.calls == 2);
    CHECK(ca_batch_begin(&instance));
    ca_signal_set_int(source, 3);
    CHECK(ca_signal_get_int(four_times) == 12);
    CHECK(observer.calls == 2);
    ca_signal_set_int(source, 4);
    CHECK(ca_signal_get_int(four_times) == 16);
    ca_batch_end(&instance);
    CHECK(observer.calls == 3 && observer.result == 16);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Verifies an active destroyed effect cannot lend its tracking slot to a replacement. */
static bool test_active_slot_lifetime(void)
{
    Ca_Instance instance = {0};
    Ca_Signal *source = ca_signal_int(&instance, 0);
    Ca_Signal *other = ca_signal_int(&instance, 0);
    CHECK(source && other);
    EffectProbe child = { .instance = &instance, .signal = other };
    EffectProbe parent = { .instance = &instance, .signal = source,
        .child = &child, .read_after = source, .destroy_self = true };
    parent.self = ca_effect(&instance, observe, &parent);
    CHECK(parent.self);
    parent.armed = true;
    ca_signal_set_int(source, 1);
    CHECK(parent.calls == 2 && child.calls == 1 && parent.created);
    ca_signal_set_int(source, 3);
    CHECK(child.calls == 1);
    ca_signal_set_int(other, 1);
    CHECK(child.calls == 2);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Writes a signal from an untracked scope. */
static void write_untracked(void *user_data)
{
    ca_signal_set_int(user_data, 1);
}

/** Verifies notification leaves the untrack scope before rerunning subscribers. */
static bool test_untrack_notification(void)
{
    Ca_Instance instance = {0};
    Ca_Signal *signal = ca_signal_int(&instance, 0);
    CHECK(signal);
    EffectProbe probe = { .instance = &instance, .signal = signal };
    CHECK(ca_effect(&instance, observe, &probe));
    CHECK(ca_untrack(&instance, write_untracked, signal));
    CHECK(probe.calls == 2);
    ca_signal_set_int(signal, 2);
    CHECK(probe.calls == 3);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Verifies nested batches, invalidation deduplication, and cancellation before flush. */
static bool test_batch_cancellation(void)
{
    Ca_Instance instance = {0};
    Ca_Signal *signal = ca_signal_int(&instance, 0);
    CHECK(signal);
    EffectProbe first = { .instance = &instance, .signal = signal };
    EffectProbe second = { .instance = &instance, .signal = signal };
    first.self = ca_effect(&instance, observe, &first);
    second.self = ca_effect(&instance, observe, &second);
    CHECK(first.self && second.self);
    CHECK(ca_batch_begin(&instance) && ca_batch_begin(&instance));
    ca_signal_set_int(signal, 1);
    ca_signal_set_int(signal, 2);
    ca_effect_invalidate(second.self);
    ca_effect_destroy(first.self);
    CHECK(first.calls == 1 && second.calls == 1);
    ca_batch_end(&instance);
    CHECK(second.calls == 1);
    ca_batch_end(&instance);
    CHECK(first.calls == 1 && second.calls == 2);
    ca_batch_end(&instance);
    CHECK(second.calls == 2);
    ca_reactive_shutdown(&instance);
    return true;
}

/** Verifies runtimes are independently owned and shutdown can be repeated. */
static bool test_instance_isolation(void)
{
    Ca_Instance first = {0}, second = {0};
    Ca_Signal *a = ca_signal_int(&first, 0);
    Ca_Signal *b = ca_signal_int(&second, 0);
    CHECK(a && b && first.reactive && second.reactive && first.reactive != second.reactive);
    EffectProbe pa = { .instance = &first, .signal = a };
    EffectProbe pb = { .instance = &second, .signal = b };
    CHECK(ca_effect(&first, observe, &pa) && ca_effect(&second, observe, &pb));
    ca_signal_set_int(a, 1);
    CHECK(pa.calls == 2 && pb.calls == 1);
    ca_reactive_shutdown(&first);
    CHECK(!first.reactive);
    ca_reactive_shutdown(&first);
    ca_signal_set_int(b, 1);
    CHECK(pb.calls == 2);
    ca_reactive_shutdown(&second);
    return true;
}

/** Reads a derived value without subscribing its caller. */
static void read_derived_untracked(void *user_data)
{
    ComputedProbe *probe = user_data;
    probe->result = ca_signal_get_int(probe->source);
}

/** Verifies an untracked derived read retains the computation's own dependencies. */
static bool test_untrack_computed(void)
{
    Ca_Instance instance = {0};
    Ca_Signal *source = ca_signal_int(&instance, 1);
    CHECK(source);
    ComputedProbe computation = { .source = source };
    Ca_Signal *derived = ca_computed(&instance, sizeof(int), compute_double, &computation);
    CHECK(derived);
    ComputedProbe reader = { .source = derived };
    ca_signal_set_int(source, 2);
    CHECK(ca_untrack(&instance, read_derived_untracked, &reader));
    CHECK(reader.result == 4);
    ca_signal_set_int(source, 3);
    CHECK(ca_signal_get_int(derived) == 6);
    ca_reactive_shutdown(&instance);
    return true;
}

static size_t allocation_attempts;
static size_t live_blocks;
static size_t fail_attempt;
static bool failure_injected;

/** Injects one allocation failure and records owned blocks. */
static void *checked_malloc(size_t size)
{
    if (++allocation_attempts == fail_attempt) { failure_injected = true; return NULL; }
    void *data = malloc(size);
    if (data) ++live_blocks;
    return data;
}

/** Injects one zero-allocation failure and records owned blocks. */
static void *checked_calloc(size_t count, size_t size)
{
    if (++allocation_attempts == fail_attempt) { failure_injected = true; return NULL; }
    void *data = calloc(count, size);
    if (data) ++live_blocks;
    return data;
}

/** Injects one resize failure without changing the previous allocation. */
static void *checked_realloc(void *data, size_t size)
{
    if (++allocation_attempts == fail_attempt) { failure_injected = true; return NULL; }
    bool new_block = data == NULL;
    void *resized = realloc(data, size);
    if (resized && new_block) ++live_blocks;
    return resized;
}

/** Counts and frees an owned allocation. */
static void checked_free(void *data)
{
    if (data) --live_blocks;
    free(data);
}

/** Verifies unused hooks do not allocate and every creation failure is releasable. */
static bool test_allocation_failures(void)
{
    ca_set_allocator(checked_malloc, checked_calloc, checked_realloc, checked_free);
    Ca_Instance unused = {0};
    ca_reactive_flush(&unused);
    ca_reactive_run_frame_effects(&unused);
    ca_batch_end(&unused);
    ca_reactive_shutdown(&unused);
    CHECK(!ca_effect(&unused, NULL, NULL));
    CHECK(!ca_signal_create(&unused, 0, NULL));
    CHECK(!ca_untrack(&unused, NULL, NULL));
    CHECK(!unused.reactive && allocation_attempts == 0);
    fail_attempt = 1;
    CHECK(!ca_batch_begin(&unused) && !unused.reactive);
    allocation_attempts = 0;
    failure_injected = false;
    CHECK(!ca_untrack(&unused, write_untracked, NULL) && !unused.reactive);
    bool completed = false;
    for (size_t failure = 1; failure <= 64; ++failure) {
        allocation_attempts = 0;
        failure_injected = false;
        fail_attempt = failure;
        Ca_Instance instance = {0};
        Ca_Signal *source = ca_signal_int(&instance, 0);
        if (source) {
            ComputedProbe computed = { .source = source };
            Ca_Signal *derived = ca_computed(&instance, sizeof(int), compute_double, &computed);
            EffectProbe probe = { .instance = &instance, .signal = derived ? derived : source };
            Ca_Effect *effect = ca_frame_effect(&instance, observe, &probe);
            if (effect) {
                size_t before = allocation_attempts;
                ca_signal_set_int(source, 1);
                ca_reactive_run_frame_effects(&instance);
                CHECK(allocation_attempts == before);
            }
        }
        ca_reactive_shutdown(&instance);
        CHECK(!instance.reactive && live_blocks == 0);
        if (!failure_injected) { completed = true; break; }
    }
    ca_set_allocator(NULL, NULL, NULL, NULL);
    CHECK(completed);
    return true;
}

/** Holds a dependency switch that exposes allocation while retracking a live effect. */
typedef struct SwitchingProbe {
    Ca_Signal *selector;
    Ca_Signal *left;
    Ca_Signal *right;
    int calls;
    int result;
} SwitchingProbe;

/** Reads the selected dependency and records its current value. */
static void observe_switch(void *user_data)
{
    SwitchingProbe *probe = user_data;
    ++probe->calls;
    Ca_Signal *source = ca_signal_get_int(probe->selector) ? probe->right : probe->left;
    probe->result = ca_signal_get_int(source);
}

/** Verifies a failed live dependency change retries and restores later notifications. */
static bool test_tracking_retry(void)
{
    allocation_attempts = fail_attempt = 0;
    failure_injected = false;
    ca_set_allocator(checked_malloc, checked_calloc, checked_realloc, checked_free);
    Ca_Instance instance = {0};
    SwitchingProbe probe = {
        .selector = ca_signal_int(&instance, 0),
        .left = ca_signal_int(&instance, 10),
        .right = ca_signal_int(&instance, 20),
    };
    CHECK(probe.selector && probe.left && probe.right);
    CHECK(ca_effect(&instance, observe_switch, &probe));
    CHECK(probe.calls == 1 && probe.result == 10);
    fail_attempt = allocation_attempts + 1;
    ca_signal_set_int(probe.selector, 1);
    CHECK(failure_injected && probe.calls == 2);
    ca_reactive_flush(&instance);
    CHECK(probe.calls == 3 && probe.result == 20);
    ca_signal_set_int(probe.right, 30);
    CHECK(probe.calls == 4 && probe.result == 30);
    ca_reactive_shutdown(&instance);
    CHECK(live_blocks == 0);
    ca_set_allocator(NULL, NULL, NULL, NULL);
    return true;
}

/** Runs the complete headless ownership, scheduling, and fault-injection suite. */
int main(void)
{
    bool passed = test_frame_removal() && test_frame_addition() && test_frame_growth() &&
        test_reentrant_notification() && test_computed_chain() &&
        test_active_slot_lifetime() && test_untrack_notification() &&
        test_batch_cancellation() && test_instance_isolation() &&
        test_untrack_computed() && test_allocation_failures() && test_tracking_retry();
    return passed ? 0 : 1;
}
