// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.
//
// ca_reactive.h — fine-grained reactivity (signals, effects, computed).
//
// This is the reactivity primitive in causality.
//
// Mental model (same as Solid / Leptos / Vue ref):
//
//   Ca_Signal *count = ca_signal_int(inst, 0);
//
//   ca_effect(inst, my_render, count_ctx);   // tracks deps automatically
//
//   ca_signal_set_int(count, 5);             // re-runs only effects that
//                                            // read `count`
//
// Reads inside an effect (or builder, or computed) are *automatically
// tracked*. When any tracked signal changes, the effect re-runs on the
// next ca_instance_tick(). Use ca_untrack to read without subscribing,
// and ca_batch_begin / ca_batch_end to coalesce many sets into one
// re-run.
//
// Threading: signals are main-thread-only by contract. To push data
// from a worker thread, post it to the main thread first (e.g. via
// the event bus or a thread-safe queue) and call ca_signal_set_*
// from there.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ca_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ca_Instance Ca_Instance;
typedef struct Ca_Signal   Ca_Signal;
typedef struct Ca_Effect   Ca_Effect;

/*
 * Effect body callback.  Any signal read inside fn is automatically tracked;
 * the effect re-runs on the next tick when a tracked signal changes.
 *
 * user_data  Caller-supplied context pointer passed to ca_effect.
 */
typedef void (*Ca_EffectFn)(void *user_data);

/*
 * Computed value callback.  Returns a pointer to the newly computed value.
 *
 * user_data  Caller-supplied context pointer passed to ca_computed.
 * Returns    Pointer to the new value; must stay valid until the next call.
 */
typedef const void *(*Ca_ComputeFn)(void *user_data);

/* ---- Lifecycle ---- */

/*
 * Create a raw signal holding value_size bytes.
 *
 * inst        Owning Ca_Instance (signal is freed by ca_instance_destroy).
 * value_size  Size of the stored value in bytes.
 * initial     Initial value (NULL = zero-initialised).
 * Returns     Newly created Ca_Signal.
 */
CA_API Ca_Signal *ca_signal_create(Ca_Instance *inst,
                                   size_t value_size,
                                   const void *initial);

/* Create a signal holding an int value. */
CA_API Ca_Signal *ca_signal_int   (Ca_Instance *inst, int      initial);

/* Create a signal holding a float value. */
CA_API Ca_Signal *ca_signal_float (Ca_Instance *inst, float    initial);

/* Create a signal holding a bool value. */
CA_API Ca_Signal *ca_signal_bool  (Ca_Instance *inst, bool     initial);

/* Create a signal holding a uint32_t value. */
CA_API Ca_Signal *ca_signal_u32   (Ca_Instance *inst, uint32_t initial);

/* Create a signal holding a void* pointer value. */
CA_API Ca_Signal *ca_signal_ptr   (Ca_Instance *inst, void    *initial);

/* Destroy a signal and unsubscribe all its dependents. */
CA_API void       ca_signal_destroy(Ca_Signal *sig);

/* ---- Reads (tracked when called inside an effect / computed / builder) ---- */

/* Return a const pointer to the signal's current value, subscribing the caller. */
CA_API const void *ca_signal_get      (Ca_Signal *sig);

/* Return the signal's int value, subscribing the caller. */
CA_API int         ca_signal_get_int  (Ca_Signal *sig);

/* Return the signal's float value, subscribing the caller. */
CA_API float       ca_signal_get_float(Ca_Signal *sig);

/* Return the signal's bool value, subscribing the caller. */
CA_API bool        ca_signal_get_bool (Ca_Signal *sig);

/* Return the signal's uint32_t value, subscribing the caller. */
CA_API uint32_t    ca_signal_get_u32  (Ca_Signal *sig);

/* Return the signal's void* value, subscribing the caller. */
CA_API void       *ca_signal_get_ptr  (Ca_Signal *sig);

/* Read the signal's current value without subscribing the current effect. */
CA_API const void *ca_signal_peek(Ca_Signal *sig);

/* ---- Writes (notify subscribers) ---- */

/*
 * Write a new raw value to the signal; no-op if the value is unchanged (memcmp).
 *
 * sig    Target signal.
 * value  Pointer to the new value (must match the signal's value_size).
 */
CA_API void ca_signal_set        (Ca_Signal *sig, const void *value);

/* Set the signal's int value; no-op if unchanged. */
CA_API void ca_signal_set_int    (Ca_Signal *sig, int      v);

/* Set the signal's float value; no-op if unchanged. */
CA_API void ca_signal_set_float  (Ca_Signal *sig, float    v);

/* Set the signal's bool value; no-op if unchanged. */
CA_API void ca_signal_set_bool   (Ca_Signal *sig, bool     v);

/* Set the signal's uint32_t value; no-op if unchanged. */
CA_API void ca_signal_set_u32    (Ca_Signal *sig, uint32_t v);

/* Set the signal's void* value; no-op if unchanged. */
CA_API void ca_signal_set_ptr    (Ca_Signal *sig, void    *v);

/*
 * Return a mutable pointer to the signal's storage for in-place mutation.
 * Call ca_signal_notify after the mutation to notify subscribers.
 *
 * sig     Target signal.
 * Returns Mutable pointer to the internal value buffer.
 */
CA_API void *ca_signal_mut(Ca_Signal *sig);

/* Notify all subscribers that the signal's value has changed. */
CA_API void  ca_signal_notify(Ca_Signal *sig);

/* ---- Effects ---- */

/*
 * Register an effect that tracks its signal dependencies automatically.
 *
 * inst       Owning Ca_Instance.
 * fn         Effect body; runs once immediately and again when deps change.
 * user_data  Passed to fn on each run.
 * Returns    Ca_Effect handle that can be passed to ca_effect_destroy.
 */
CA_API Ca_Effect *ca_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data);

/* Destroy an effect and remove all its signal subscriptions. */
CA_API void ca_effect_destroy(Ca_Effect *eff);

/* Force the effect to re-run on the next tick, re-tracking its dependencies. */
CA_API void ca_effect_invalidate(Ca_Effect *eff);

/*
 * Register an effect that re-runs unconditionally on every
 * ca_instance_tick, regardless of which signals (if any) it reads —
 * for genuinely external, continuously-changing state with no signal
 * source (live engine telemetry: frame timing, GPU/CPU stats, and
 * similar). The body may call ca_set_text / ca_set_* and other direct
 * widget mutators (safe outside a build context); it must NOT call
 * ca_reconcile_begin, ca_div_clear, ca_div_begin or anything else that
 * requires an active widget build context — use ca_div_set_builder
 * (optionally combined with a plain ca_signal write from a
 * ca_frame_effect) for that instead.
 *
 * Ordinary ca_effect is almost always the right choice — reach for this
 * only when the effect's own body has no signal to depend on because the
 * state it reads lives outside causality entirely.
 *
 * inst       Owning Ca_Instance.
 * fn         Effect body; runs once immediately and again every tick.
 * user_data  Passed to fn on each run.
 * Returns    Ca_Effect handle that can be passed to ca_effect_destroy.
 */
CA_API Ca_Effect *ca_frame_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data);

/* ---- Computed ---- */

/*
 * Create a derived signal whose value is recomputed lazily by fn.
 *
 * inst        Owning Ca_Instance.
 * value_size  Size in bytes of the value returned by fn; copied into storage.
 * fn          Compute function; re-runs when tracked dependencies change.
 * user_data   Passed to fn on each recomputation.
 * Returns     A Ca_Signal that reads like any other signal.
 */
CA_API Ca_Signal *ca_computed(Ca_Instance *inst,
                              size_t       value_size,
                              Ca_ComputeFn fn,
                              void        *user_data);

/* ---- Batching ---- */

/*
 * Begin a batch: defer all effect re-runs until the matching ca_batch_end.
 * Calls nest; effects only flush when the outermost batch ends.
 *
 * inst  Owning Ca_Instance.
 */
CA_API void ca_batch_begin(Ca_Instance *inst);

/*
 * End a batch and flush any pending effect re-runs.
 *
 * inst  Owning Ca_Instance.
 */
CA_API void ca_batch_end  (Ca_Instance *inst);

/*
 * Execute fn with dependency tracking suppressed for the current effect.
 *
 * inst       Owning Ca_Instance.
 * fn         Function to run; any signal reads inside will not create subscriptions.
 * user_data  Passed to fn.
 */
CA_API void ca_untrack(Ca_Instance *inst, void (*fn)(void *), void *user_data);

#ifdef __cplusplus
}
#endif
