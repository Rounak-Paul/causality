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

/* User-supplied effect body. Any signal read inside this function is
   automatically subscribed; the effect will re-run when one changes. */
typedef void (*Ca_EffectFn)(void *user_data);

/* User-supplied computed body. Returns the new value. The pointer must
   stay valid for the lifetime of the computed (typically: pointer into
   a struct member owned by user_data). */
typedef const void *(*Ca_ComputeFn)(void *user_data);

/* ---- Lifecycle ---- */

/* Create a signal holding `value_size` bytes initialised from `initial`
   (NULL = zero-initialised). The signal is owned by the instance and
   freed by ca_instance_destroy. */
CA_API Ca_Signal *ca_signal_create(Ca_Instance *inst,
                                   size_t value_size,
                                   const void *initial);

/* Convenience constructors. */
CA_API Ca_Signal *ca_signal_int   (Ca_Instance *inst, int      initial);
CA_API Ca_Signal *ca_signal_float (Ca_Instance *inst, float    initial);
CA_API Ca_Signal *ca_signal_bool  (Ca_Instance *inst, bool     initial);
CA_API Ca_Signal *ca_signal_u32   (Ca_Instance *inst, uint32_t initial);
CA_API Ca_Signal *ca_signal_ptr   (Ca_Instance *inst, void    *initial);

CA_API void       ca_signal_destroy(Ca_Signal *sig);

/* ---- Reads (tracked when called inside an effect / computed / builder) ---- */

CA_API const void *ca_signal_get  (Ca_Signal *sig);
CA_API int         ca_signal_get_int  (Ca_Signal *sig);
CA_API float       ca_signal_get_float(Ca_Signal *sig);
CA_API bool        ca_signal_get_bool (Ca_Signal *sig);
CA_API uint32_t    ca_signal_get_u32  (Ca_Signal *sig);
CA_API void       *ca_signal_get_ptr  (Ca_Signal *sig);

/* Read without subscribing the current effect to this signal. */
CA_API const void *ca_signal_peek(Ca_Signal *sig);

/* ---- Writes (notify subscribers) ---- */

/* Compares old vs new with memcmp. No-op if equal. */
CA_API void ca_signal_set        (Ca_Signal *sig, const void *value);
CA_API void ca_signal_set_int    (Ca_Signal *sig, int      v);
CA_API void ca_signal_set_float  (Ca_Signal *sig, float    v);
CA_API void ca_signal_set_bool   (Ca_Signal *sig, bool     v);
CA_API void ca_signal_set_u32    (Ca_Signal *sig, uint32_t v);
CA_API void ca_signal_set_ptr    (Ca_Signal *sig, void    *v);

/* In-place mutate then notify (use when the value is large or non-trivial). */
CA_API void *ca_signal_mut(Ca_Signal *sig);          /* returns mutable pointer */
CA_API void  ca_signal_notify(Ca_Signal *sig);       /* tell subscribers */

/* ---- Effects ---- */

/* Register an effect. It runs once immediately to capture dependencies,
   then re-runs whenever any read signal changes. Returns a handle that
   can be passed to ca_effect_destroy. */
CA_API Ca_Effect *ca_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data);

CA_API void ca_effect_destroy(Ca_Effect *eff);

/* Manually re-run an effect (and re-track its deps). */
CA_API void ca_effect_invalidate(Ca_Effect *eff);

/* ---- Computed ---- */

/* A computed is a signal whose value is the return of `fn`, recomputed
   lazily on read after any of its tracked dependencies changed.
   `value_size` must match the size of the struct that `fn` returns a
   pointer to — the framework copies it into the computed's storage. */
CA_API Ca_Signal *ca_computed(Ca_Instance *inst,
                              size_t       value_size,
                              Ca_ComputeFn fn,
                              void        *user_data);

/* ---- Batching ---- */

/* Defer effect re-runs until ca_batch_end(). Calls nest. */
CA_API void ca_batch_begin(Ca_Instance *inst);
CA_API void ca_batch_end  (Ca_Instance *inst);

/* Run `fn` with tracking suppressed (any signal reads inside do not
   subscribe the current effect). */
CA_API void ca_untrack(Ca_Instance *inst, void (*fn)(void *), void *user_data);

#ifdef __cplusplus
}
#endif
