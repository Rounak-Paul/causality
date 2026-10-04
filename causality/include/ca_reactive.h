// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ca_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Main-thread reactive values and effects owned by one Ca_Instance.
 *
 * An instance must outlive every call and callback using it. Signal/effect
 * handles become invalid on destruction and must not be used afterward.
 * Callback contexts are borrowed and must outlive their active callbacks.
 * Reads track dependencies only within the same instance.
 *
 * Notifications drain synchronously outside batches and active callbacks.
 * Changes inside a callback wait until it returns; one effect never recursively
 * enters itself. Effects must converge rather than invalidate indefinitely.
 * Subscriber execution order is unspecified. Typed reads return zero, false,
 * or NULL when their underlying read fails.
 */
typedef struct Ca_Instance Ca_Instance;
typedef struct Ca_Signal Ca_Signal;
typedef struct Ca_Effect Ca_Effect;

/** Runs an effect with borrowed user_data; tracked reads establish dependencies. */
typedef void (*Ca_EffectFn)(void *user_data);

/**
 * Returns value_size readable bytes for copying into a computed signal.
 * The result must remain valid until the callback's caller copies it.
 * Computations may read signals and update caller-owned result storage, but
 * must not write signals or create/destroy reactive objects.
 * Derived dependencies must form an acyclic graph within one instance.
 */
typedef const void *(*Ca_ComputeFn)(void *user_data);

/**
 * Creates inst's signal containing value_size bytes, initially copied from
 * initial or zeroed when initial is NULL. Size must be in [1, UINT32_MAX].
 * Returns NULL on invalid arguments or allocation failure.
 */
CA_API Ca_Signal *ca_signal_create(Ca_Instance *inst, size_t value_size,
                                  const void *initial);

/** Creates inst's signal holding initial as an int; NULL on failure. */
CA_API Ca_Signal *ca_signal_int(Ca_Instance *inst, int initial);
/** Creates inst's signal holding initial as a float; NULL on failure. */
CA_API Ca_Signal *ca_signal_float(Ca_Instance *inst, float initial);
/** Creates inst's signal holding initial as a bool; NULL on failure. */
CA_API Ca_Signal *ca_signal_bool(Ca_Instance *inst, bool initial);
/** Creates inst's signal holding initial as a uint32_t; NULL on failure. */
CA_API Ca_Signal *ca_signal_u32(Ca_Instance *inst, uint32_t initial);
/** Creates inst's signal holding the borrowed initial pointer; NULL on failure. */
CA_API Ca_Signal *ca_signal_ptr(Ca_Instance *inst, void *initial);

/** Destroys sig, detaches dependents, and invalidates its handle; NULL is a no-op. */
CA_API void ca_signal_destroy(Ca_Signal *sig);

/**
 * Returns sig's borrowed value and tracks the read in the current effect.
 * Dirty computed values are evaluated before returning. The pointer remains
 * valid until sig is changed or destroyed. Returns NULL for NULL sig or a
 * failed computation. Computed tracking failures retry on a subsequent read.
 */
CA_API const void *ca_signal_get(Ca_Signal *sig);

/** Reads sig as int, tracking the dependency; requires matching storage type. */
CA_API int ca_signal_get_int(Ca_Signal *sig);
/** Reads sig as float, tracking the dependency; requires matching storage type. */
CA_API float ca_signal_get_float(Ca_Signal *sig);
/** Reads sig as bool, tracking the dependency; requires matching storage type. */
CA_API bool ca_signal_get_bool(Ca_Signal *sig);
/** Reads sig as uint32_t, tracking the dependency; requires matching storage type. */
CA_API uint32_t ca_signal_get_u32(Ca_Signal *sig);
/** Reads sig as a pointer, tracking the dependency; requires matching storage type. */
CA_API void *ca_signal_get_ptr(Ca_Signal *sig);

/** Returns sig's current value without tracking this read; otherwise follows ca_signal_get. */
CA_API const void *ca_signal_peek(Ca_Signal *sig);

/**
 * Copies sig's byte count from value, notifying only when bytes differ.
 * value must contain the complete value; NULL sig/value is a no-op.
 * Computed signals are read-only and must not be passed to write/mutation APIs.
 */
CA_API void ca_signal_set(Ca_Signal *sig, const void *value);

/** Writes v to an int signal; NULL sig is a no-op. */
CA_API void ca_signal_set_int(Ca_Signal *sig, int v);
/** Writes v to a float signal; NULL sig is a no-op. */
CA_API void ca_signal_set_float(Ca_Signal *sig, float v);
/** Writes v to a bool signal; NULL sig is a no-op. */
CA_API void ca_signal_set_bool(Ca_Signal *sig, bool v);
/** Writes v to a uint32_t signal; NULL sig is a no-op. */
CA_API void ca_signal_set_u32(Ca_Signal *sig, uint32_t v);
/** Writes the borrowed pointer v to a pointer signal; NULL sig is a no-op. */
CA_API void ca_signal_set_ptr(Ca_Signal *sig, void *v);

/** Returns plain sig's mutable storage, or NULL; call ca_signal_notify after editing it. */
CA_API void *ca_signal_mut(Ca_Signal *sig);

/** Invalidates plain sig's subscribers, including derived chains; NULL/computed sig is a no-op. */
CA_API void ca_signal_notify(Ca_Signal *sig);

/**
 * Creates inst's effect, running fn(user_data) immediately and tracking reads.
 * Returns NULL on invalid arguments, allocation/tracking failure, or destruction
 * during the initial run. Callback side effects are not rolled back on failure.
 */
CA_API Ca_Effect *ca_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data);

/**
 * Cancels eff's pending/frame callbacks and subscriptions immediately.
 * Its active callback may finish; its storage is not recycled until then.
 * NULL eff is a no-op. The caller must discard the destroyed handle.
 */
CA_API void ca_effect_destroy(Ca_Effect *eff);

/** Invalidates eff using the same synchronous/batched rules as signal notification. */
CA_API void ca_effect_invalidate(Ca_Effect *eff);

/**
 * Creates inst's effect with an immediate run and a recurring run each tick.
 * fn may mutate retained widgets but must not require an active widget builder.
 * Recurring iteration preserves registration order. Additions during iteration
 * receive their immediate run, then join recurring iteration on the next tick.
 * Removal cancels callbacks that have not started. Tracked signal changes may
 * also run the effect between ticks. Failure follows ca_effect's contract.
 */
CA_API Ca_Effect *ca_frame_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data);

/**
 * Creates inst's derived signal and computes its initial value once using
 * fn(user_data). Subsequent dependency changes invalidate the entire derived
 * chain; each dirty value recomputes once when read, including inside a batch.
 * Dependents may run even when recomputation produces equal bytes.
 * Returns NULL on invalid arguments or allocation/computation failure.
 */
CA_API Ca_Signal *ca_computed(Ca_Instance *inst, size_t value_size,
                             Ca_ComputeFn fn, void *user_data);

/**
 * Begins inst's nested batch. Returns true when opened; false on invalid inst,
 * allocation failure, or depth exhaustion, leaving nesting unchanged.
 * Match every successful begin with ca_batch_end; batches defer effects only.
 */
CA_API bool ca_batch_begin(Ca_Instance *inst);

/** Ends one successful batch; the outermost end flushes. Unmatched/NULL ends are no-ops. */
CA_API void ca_batch_end(Ca_Instance *inst);

/**
 * Runs fn(user_data) without adding dependencies to inst's current effect,
 * then drains notifications after restoring tracking. Nested computations and
 * new effects track their own dependencies. Existing dependencies remain.
 * Returns false without invoking fn on invalid arguments, allocation failure,
 * or depth exhaustion; true when fn completed. Calls may nest.
 */
CA_API bool ca_untrack(Ca_Instance *inst, Ca_EffectFn fn, void *user_data);

#ifdef __cplusplus
}
#endif
