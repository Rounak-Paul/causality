// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.
//
// signal.c — fine-grained reactivity implementation (signals, effects,
// computed, batching). Main-thread-only.

#include "ca_reactive.h"
#include "ca_internal.h"
#include "reactive.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Owns stable signal storage and reciprocal subscriber links. */
struct Ca_Signal {
    Ca_Instance *inst;
    uint8_t *value;
    Ca_DynArray subscribers;
    Ca_ComputeFn compute_fn;
    void *compute_user;
    Ca_Effect *owner_effect;
    uint32_t value_size;
    bool in_use;
    bool dirty_computed;
    bool computing;
};

/** Keeps a callback's slot alive until its execution references unwind. */
struct Ca_Effect {
    Ca_Instance *inst;
    Ca_EffectFn fn;
    void *user_data;
    Ca_DynArray dependencies;
    Ca_Signal *computed_target;
    size_t execution_refs;
    bool in_use;
    bool scheduled;
    bool is_frame_effect;
    bool running;
    bool tracking_failed;
};

/** Owns an instance's stable reactive handles and retained work queues. */
typedef struct Ca_Reactive {
    Ca_Pool signals;
    Ca_Pool effects;
    Ca_DynArray track_stack;
    Ca_DynArray pending;
    Ca_DynArray frame_effects;
    Ca_DynArray invalidated_computeds;
    Ca_Effect *untracked_effect;
    size_t batch_depth;
    size_t untrack_depth;
    bool flushing;
    bool running_frame_effects;
    bool compact_frame_effects;
} Ca_Reactive;

/** Returns inst's runtime, lazily publishing it only after initialization. */
static Ca_Reactive *get_reactive(Ca_Instance *inst)
{
    if (!inst) return NULL;
    if (inst->reactive) return inst->reactive;
    Ca_Reactive *runtime = CA_CALLOC(1, sizeof(*runtime));
    if (!runtime) return NULL;
    bool initialized =
        ca_pool_init(&runtime->signals, sizeof(Ca_Signal),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Signal))) &&
        ca_pool_init(&runtime->effects, sizeof(Ca_Effect),
                     ca_pool_recommended_chunk_capacity(sizeof(Ca_Effect))) &&
        ca_dyn_array_init(&runtime->track_stack, sizeof(Ca_Effect *)) &&
        ca_dyn_array_init(&runtime->pending, sizeof(Ca_Effect *)) &&
        ca_dyn_array_init(&runtime->frame_effects, sizeof(Ca_Effect *)) &&
        ca_dyn_array_init(&runtime->invalidated_computeds, sizeof(Ca_Signal *));
    if (!initialized) {
        ca_dyn_array_destroy(&runtime->invalidated_computeds);
        ca_dyn_array_destroy(&runtime->frame_effects);
        ca_dyn_array_destroy(&runtime->pending);
        ca_dyn_array_destroy(&runtime->track_stack);
        ca_pool_destroy(&runtime->effects, NULL, NULL);
        ca_pool_destroy(&runtime->signals, NULL, NULL);
        CA_FREE(runtime);
        return NULL;
    }
    inst->reactive = runtime;
    return runtime;
}

void ca_reactive_shutdown(Ca_Instance *inst)
{
    if (!inst || !inst->reactive) return;
    Ca_Reactive *r = inst->reactive;
    assert(!r->track_stack.count && !r->flushing && !r->running_frame_effects);
    inst->reactive = NULL;
    for (size_t i = 0; i < ca_pool_slot_count(&r->signals); ++i) {
        Ca_Signal *signal = CA_POOL_AT(r->signals, Ca_Signal, i);
        if (!signal->in_use) continue;
        CA_FREE(signal->value);
        ca_dyn_array_destroy(&signal->subscribers);
    }
    for (size_t i = 0; i < ca_pool_slot_count(&r->effects); ++i) {
        Ca_Effect *effect = CA_POOL_AT(r->effects, Ca_Effect, i);
        if (!effect->in_use) continue;
        ca_dyn_array_destroy(&effect->dependencies);
    }
    ca_dyn_array_destroy(&r->invalidated_computeds);
    ca_dyn_array_destroy(&r->frame_effects);
    ca_dyn_array_destroy(&r->pending);
    ca_dyn_array_destroy(&r->track_stack);
    ca_pool_destroy(&r->effects, NULL, NULL);
    ca_pool_destroy(&r->signals, NULL, NULL);
    CA_FREE(r);
}

/** Queues e once in r's preallocated pending storage. */
static void schedule_effect(Ca_Reactive *r, Ca_Effect *e)
{
    if (!e || !e->in_use || e->scheduled) return;
    if (!ca_dyn_array_push(&r->pending, &e)) {
        fprintf(stderr, "[causality] reactive: pending queue allocation failed\n");
        return;
    }
    e->scheduled = true;
}

/** Detaches e from its reciprocal signal subscriber lists. */
static void clear_effect_deps(Ca_Effect *e)
{
    for (size_t i = 0; i < e->dependencies.count; ++i) {
        Ca_Signal *s = *(Ca_Signal **)ca_dyn_array_at(&e->dependencies, i);
        if (!s || !s->in_use) continue;
        for (size_t j = 0; j < s->subscribers.count; ++j) {
            Ca_Effect **subscriber = ca_dyn_array_at(&s->subscribers, j);
            if (*subscriber == e) {
                ca_dyn_array_erase_unordered(&s->subscribers, j);
                break;
            }
        }
    }
    ca_dyn_array_clear(&e->dependencies);
}

/** Records s in r's current effect, publishing both links together. */
static void track_dep(Ca_Reactive *r, Ca_Signal *s)
{
    if (r->track_stack.count == 0) return;
    Ca_Effect *e = *(Ca_Effect **)ca_dyn_array_back(&r->track_stack);
    if (!e || !e->in_use ||
        (r->untrack_depth && r->untracked_effect == e)) return;

    for (size_t i = 0; i < e->dependencies.count; ++i)
        if (*(Ca_Signal **)ca_dyn_array_at(&e->dependencies, i) == s) return;

    if (!ca_dyn_array_reserve(&e->dependencies,
                              e->dependencies.count + 1u) ||
        !ca_dyn_array_reserve(&s->subscribers,
                              s->subscribers.count + 1u)) {
        e->tracking_failed = true;
        fprintf(stderr, "[causality] reactive: dependency allocation failed\n");
        return;
    }
    ca_dyn_array_push(&e->dependencies, &s);
    ca_dyn_array_push(&s->subscribers, &e);
}

/** Cancels e's unique pending entry in r without invoking its callback. */
static void unschedule_effect(Ca_Reactive *r, Ca_Effect *e)
{
    if (e->scheduled) {
        for (size_t i = 0; i < r->pending.count; ++i) {
            if (((Ca_Effect **)r->pending.data)[i] != e) continue;
            ca_dyn_array_erase_unordered(&r->pending, i);
            break;
        }
    }
    e->scheduled = false;
}

/** Runs e in r, retaining its slot through callbacks and pending work; returns live tracking success. */
static bool run_effect(Ca_Reactive *r, Ca_Effect *e)
{
    if (!e || !e->in_use || !e->fn || e->running) return false;
    if (!ca_dyn_array_push(&r->track_stack, &e)) {
        fprintf(stderr, "[causality] reactive: tracking stack allocation failed\n");
        return false;
    }
    ++e->execution_refs;
    e->running = true;
    e->tracking_failed = false;
    clear_effect_deps(e);
    unschedule_effect(r, e);
    e->fn(e->user_data);
    ca_dyn_array_pop(&r->track_stack, NULL);
    e->running = false;
    bool tracked = !e->tracking_failed;
    ca_reactive_flush(e->inst);
    bool live = e->in_use;
    if (--e->execution_refs == 0 && !live)
        ca_pool_release(&r->effects, e);
    return live && tracked;
}

void ca_reactive_flush(Ca_Instance *inst)
{
    Ca_Reactive *r = inst ? inst->reactive : NULL;
    if (!r || r->flushing || r->track_stack.count || r->batch_depth || r->untrack_depth) return;
    r->flushing = true;
    while (r->pending.count > 0) {
        Ca_Effect *e = NULL;
        ca_dyn_array_pop(&r->pending, &e);
        e->scheduled = false;
        if (!run_effect(r, e) && e->in_use) {
            schedule_effect(r, e);
            break;
        }
    }
    r->flushing = false;
}

void ca_reactive_run_frame_effects(Ca_Instance *inst)
{
    Ca_Reactive *r = inst ? inst->reactive : NULL;
    if (!r || r->running_frame_effects) return;
    r->running_frame_effects = true;
    size_t count = r->frame_effects.count;
    for (size_t i = 0; i < count; ++i) {
        Ca_Effect *e = ((Ca_Effect **)r->frame_effects.data)[i];
        if (e && e->in_use && e->is_frame_effect &&
            !run_effect(r, e) && e->in_use)
            schedule_effect(r, e);
    }
    r->running_frame_effects = false;
    if (r->compact_frame_effects) {
        Ca_Effect **effects = r->frame_effects.data;
        size_t live_count = 0;
        for (size_t i = 0; i < r->frame_effects.count; ++i)
            if (effects[i]) effects[live_count++] = effects[i];
        ca_dyn_array_resize(&r->frame_effects, live_count);
        r->compact_frame_effects = false;
    }
}

/** Creates inst's value_size-byte signal from initial, or returns NULL on failure. */
Ca_Signal *ca_signal_create(Ca_Instance *inst, size_t value_size, const void *initial)
{
    if (!inst || value_size == 0 || value_size > UINT32_MAX) return NULL;
    Ca_Reactive *r = get_reactive(inst);
    if (!r) return NULL;
    Ca_Signal *signal = ca_pool_acquire(&r->signals);
    if (!signal) return NULL;
    signal->inst = inst;
    signal->value_size = (uint32_t)value_size;
    signal->value = CA_CALLOC(1, value_size);
    if (!signal->value ||
        !ca_dyn_array_init(&signal->subscribers, sizeof(Ca_Effect *))) {
        CA_FREE(signal->value);
        ca_pool_release(&r->signals, signal);
        return NULL;
    }
    signal->in_use = true;
    if (initial) memcpy(signal->value, initial, value_size);
    return signal;
}

/* Typed convenience wrappers around ca_signal_create(). */
Ca_Signal *ca_signal_int  (Ca_Instance *i, int      v) { return ca_signal_create(i, sizeof(int),      &v); }
Ca_Signal *ca_signal_float(Ca_Instance *i, float    v) { return ca_signal_create(i, sizeof(float),    &v); }
Ca_Signal *ca_signal_bool (Ca_Instance *i, bool     v) { return ca_signal_create(i, sizeof(bool),     &v); }
Ca_Signal *ca_signal_u32  (Ca_Instance *i, uint32_t v) { return ca_signal_create(i, sizeof(uint32_t), &v); }
Ca_Signal *ca_signal_ptr  (Ca_Instance *i, void    *v) { return ca_signal_create(i, sizeof(void *),   &v); }

/** Destroys sig's value and reciprocal links before recycling its slot. */
void ca_signal_destroy(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return;
    Ca_Reactive *r = sig->inst->reactive;
    if (r) {
        while (sig->subscribers.count > 0) {
            Ca_Effect *effect = NULL;
            ca_dyn_array_pop(&sig->subscribers, &effect);
            if (!effect || !effect->in_use) continue;
            for (size_t i = 0; i < effect->dependencies.count; ++i) {
                Ca_Signal **dependency =
                    ca_dyn_array_at(&effect->dependencies, i);
                if (*dependency == sig) {
                    ca_dyn_array_erase_unordered(&effect->dependencies, i);
                    break;
                }
            }
        }
        Ca_Effect *owner = sig->owner_effect;
        sig->owner_effect = NULL;
        if (owner) ca_effect_destroy(owner);
    }
    CA_FREE(sig->value);
    ca_dyn_array_destroy(&sig->subscribers);
    if (r) ca_pool_release(&r->signals, sig);
}

/** Evaluates dirty s through its owning effect; returns false on cycles or tracking failure. */
static bool maybe_recompute(Ca_Signal *s)
{
    if (!s->compute_fn || !s->dirty_computed) return true;
    if (s->computing) {
        fprintf(stderr, "[causality] reactive: cyclic computed dependency\n");
        return false;
    }
    return s->owner_effect && run_effect(s->inst->reactive, s->owner_effect) &&
           !s->dirty_computed;
}

/** Reads sig's current value and tracks it in its instance's active effect. */
const void *ca_signal_get(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return NULL;
    Ca_Reactive *r = sig->inst->reactive;
    if (!r) return NULL;
    if (!maybe_recompute(sig)) {
        Ca_Effect **current = ca_dyn_array_back(&r->track_stack);
        if (current) (*current)->tracking_failed = true;
        return NULL;
    }
    track_dep(r, sig);
    return sig->value;
}

/* Typed accessors — call ca_signal_get() and dereference the result. */
int      ca_signal_get_int   (Ca_Signal *s) { const int      *p = (const int      *)ca_signal_get(s); return p ? *p : 0; }
float    ca_signal_get_float (Ca_Signal *s) { const float    *p = (const float    *)ca_signal_get(s); return p ? *p : 0.0f; }
bool     ca_signal_get_bool  (Ca_Signal *s) { const bool     *p = (const bool     *)ca_signal_get(s); return p ? *p : false; }
uint32_t ca_signal_get_u32   (Ca_Signal *s) { const uint32_t *p = (const uint32_t *)ca_signal_get(s); return p ? *p : 0u; }
void    *ca_signal_get_ptr   (Ca_Signal *s) { void *const *p = (void *const *)ca_signal_get(s); return p ? *p : NULL; }

/** Reads sig's current value without subscribing the current effect. */
const void *ca_signal_peek(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return NULL;
    return maybe_recompute(sig) ? sig->value : NULL;
}

/** Marks derived subscribers dirty and schedules ordinary effects in r. */
static void invalidate_subscribers(Ca_Reactive *r, Ca_Signal *signal)
{
    Ca_Effect **subscribers = signal->subscribers.data;
    for (size_t i = 0; i < signal->subscribers.count; ++i) {
        Ca_Effect *effect = subscribers[i];
        if (!effect->in_use) continue;
        Ca_Signal *computed = effect->computed_target;
        if (!computed) {
            schedule_effect(r, effect);
        } else if (!computed->dirty_computed) {
            computed->dirty_computed = true;
            bool pushed = ca_dyn_array_push(&r->invalidated_computeds, &computed);
            assert(pushed);
            (void)pushed;
        }
    }
}

/** Invalidates plain sig's derived graph before executing any subscriber. */
void ca_signal_notify(Ca_Signal *sig)
{
    if (!sig || !sig->in_use || sig->compute_fn) return;
    Ca_Reactive *r = sig->inst->reactive;
    invalidate_subscribers(r, sig);
    for (size_t i = 0; i < r->invalidated_computeds.count; ++i) {
        Ca_Signal *computed = ((Ca_Signal **)r->invalidated_computeds.data)[i];
        invalidate_subscribers(r, computed);
    }
    ca_dyn_array_clear(&r->invalidated_computeds);
    ca_reactive_flush(sig->inst);
}

/** Copies value into plain sig and notifies only when its bytes change. */
void ca_signal_set(Ca_Signal *sig, const void *value)
{
    if (!sig || !sig->in_use || sig->compute_fn || !value) return;
    if (memcmp(sig->value, value, sig->value_size) == 0) return;
    memcpy(sig->value, value, sig->value_size);
    ca_signal_notify(sig);
}

/* Typed setters — call ca_signal_set() with the address of a local value. */
void ca_signal_set_int   (Ca_Signal *s, int      v) { ca_signal_set(s, &v); }
void ca_signal_set_float (Ca_Signal *s, float    v) { ca_signal_set(s, &v); }
void ca_signal_set_bool  (Ca_Signal *s, bool     v) { ca_signal_set(s, &v); }
void ca_signal_set_u32   (Ca_Signal *s, uint32_t v) { ca_signal_set(s, &v); }
void ca_signal_set_ptr   (Ca_Signal *s, void    *v) { ca_signal_set(s, &v); }

/** Returns plain sig's writable storage, or NULL when unavailable. */
void *ca_signal_mut(Ca_Signal *sig)
{
    if (!sig || !sig->in_use || sig->compute_fn) return NULL;
    return sig->value;
}

/** Creates inst's callback fn with borrowed user_data and performs its initial run. */
Ca_Effect *ca_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data)
{
    if (!inst || !fn) return NULL;
    Ca_Reactive *r = get_reactive(inst);
    if (!r) return NULL;
    Ca_Effect *effect = ca_pool_acquire(&r->effects);
    if (!effect) return NULL;
    if (!ca_dyn_array_reserve(&r->pending, ca_pool_live_count(&r->effects)) ||
        !ca_dyn_array_reserve(&r->invalidated_computeds, ca_pool_live_count(&r->effects)) ||
        !ca_dyn_array_init(&effect->dependencies, sizeof(Ca_Signal *))) {
        ca_pool_release(&r->effects, effect);
        return NULL;
    }
    effect->inst = inst;
    effect->fn = fn;
    effect->user_data = user_data;
    effect->in_use = true;
    if (!run_effect(r, effect)) {
        if (effect->in_use) ca_effect_destroy(effect);
        return NULL;
    }
    return effect;
}

/** Registers inst's callback fn for immediate and recurring execution. */
Ca_Effect *ca_frame_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data)
{
    Ca_Effect *e = ca_effect(inst, fn, user_data);
    if (!e) return NULL;
    e->is_frame_effect = true;
    Ca_Reactive *r = get_reactive(inst);
    if (!r || !ca_dyn_array_push(&r->frame_effects, &e)) {
        ca_effect_destroy(e);
        return NULL;
    }
    return e;
}

/** Cancels eff immediately and recycles its slot after active executions unwind. */
void ca_effect_destroy(Ca_Effect *eff)
{
    if (!eff || !eff->in_use) return;
    eff->in_use = false;
    Ca_Reactive *r = eff->inst->reactive;
    if (r) {
        clear_effect_deps(eff);
        unschedule_effect(r, eff);
        for (size_t i = 0; i < r->frame_effects.count; ++i) {
            Ca_Effect **frame_effect =
                ca_dyn_array_at(&r->frame_effects, i);
            if (*frame_effect == eff) {
                if (r->running_frame_effects) {
                    *frame_effect = NULL;
                    r->compact_frame_effects = true;
                } else {
                    ca_dyn_array_erase(&r->frame_effects, i, 1);
                }
                break;
            }
        }
    }
    if (eff->computed_target && eff->computed_target->owner_effect == eff)
        eff->computed_target->owner_effect = NULL;
    ca_dyn_array_destroy(&eff->dependencies);
    if (r && !eff->execution_refs) ca_pool_release(&r->effects, eff);
}

/** Schedules eff and flushes when no callback or batching scope is active. */
void ca_effect_invalidate(Ca_Effect *eff)
{
    if (!eff || !eff->in_use) return;
    Ca_Reactive *r = eff->inst->reactive;
    if (!r) return;
    schedule_effect(r, eff);
    if (r->batch_depth == 0) ca_reactive_flush(eff->inst);
}

/** Computes and copies s's value once while its owning effect tracks reads. */
static void computed_effect_fn(void *user_data)
{
    Ca_Signal *s = user_data;
    Ca_Reactive *r = s->inst->reactive;
    Ca_Effect *effect = *(Ca_Effect **)ca_dyn_array_back(&r->track_stack);
    s->computing = true;
    const void *value = s->compute_fn(s->compute_user);
    s->computing = false;
    if (!value) effect->tracking_failed = true;
    if (effect->tracking_failed) return;
    memcpy(s->value, value, s->value_size);
    s->dirty_computed = false;
}

/** Creates inst's derived value with one tracked fn(user_data) computation. */
Ca_Signal *ca_computed(Ca_Instance *inst, size_t value_size,
                       Ca_ComputeFn fn, void *user_data)
{
    if (!fn) return NULL;
    Ca_Signal *signal = ca_signal_create(inst, value_size, NULL);
    if (!signal) return NULL;
    signal->compute_fn = fn;
    signal->compute_user = user_data;
    signal->dirty_computed = true;
    Ca_Effect *effect = ca_effect(inst, computed_effect_fn, signal);
    if (!effect) {
        ca_signal_destroy(signal);
        return NULL;
    }
    signal->owner_effect = effect;
    effect->computed_target = signal;
    return signal;
}

/** Opens inst's batch, returning false when state cannot be acquired. */
bool ca_batch_begin(Ca_Instance *inst)
{
    Ca_Reactive *r = get_reactive(inst);
    if (!r || r->batch_depth == SIZE_MAX) return false;
    ++r->batch_depth;
    return true;
}

/** Closes one successful batch on inst and flushes the outermost scope. */
void ca_batch_end(Ca_Instance *inst)
{
    Ca_Reactive *r = inst ? inst->reactive : NULL;
    if (!r || !r->batch_depth) return;
    if (--r->batch_depth == 0) ca_reactive_flush(inst);
}

/** Invokes fn(user_data) with inst's tracking suppressed, restoring it before notification. */
bool ca_untrack(Ca_Instance *inst, Ca_EffectFn fn, void *user_data)
{
    if (!inst || !fn) return false;
    Ca_Reactive *r = get_reactive(inst);
    if (!r || r->untrack_depth == SIZE_MAX) return false;
    Ca_Effect *previous = r->untracked_effect;
    Ca_Effect **current = ca_dyn_array_back(&r->track_stack);
    r->untracked_effect = current ? *current : NULL;
    r->untrack_depth++;
    fn(user_data);
    r->untrack_depth--;
    r->untracked_effect = previous;
    ca_reactive_flush(inst);
    return true;
}
