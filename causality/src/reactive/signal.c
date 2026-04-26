// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.
//
// signal.c — fine-grained reactivity implementation (signals, effects,
// computed, batching). Main-thread-only.

#include "ca_reactive.h"
#include "ca_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
   Data layout
   ============================================================ */

struct Ca_Signal {
    Ca_Instance *inst;
    uint32_t     value_size;
    uint8_t     *value;          /* heap-allocated, value_size bytes */
    uint64_t     generation;     /* bumped on every change */
    bool         in_use;

    /* Subscribers (effects that read this signal). Indices into inst's
       effect pool; -1 = empty slot. */
    int32_t      subs[CA_MAX_SIGNAL_SUBSCRIBERS];
    uint32_t     sub_count;

    /* Optional computed body. NULL = plain signal. */
    Ca_ComputeFn compute_fn;
    void        *compute_user;
    bool         dirty_computed; /* true: value out of date, recompute on read */

    /* Effect that owns this computed (for tracking when it's a computed). */
    Ca_Effect   *owner_effect;
};

struct Ca_Effect {
    Ca_Instance *inst;
    Ca_EffectFn  fn;
    void        *user_data;
    bool         in_use;
    bool         scheduled;       /* queued for next flush */

    /* Signals this effect currently subscribes to. */
    Ca_Signal   *deps[CA_MAX_SIGNAL_DEPS];
    uint32_t     dep_count;

    /* For computed: the signal we should write into when re-running. */
    Ca_Signal   *computed_target;
};

/* ============================================================
   Per-instance reactive runtime.
   We attach this as a side-table indexed by Ca_Instance pointer so we
   don't have to modify the public Ca_Instance struct in ca_internal.h
   (which would force ABI breakage downstream).
   ============================================================ */

#define CA_REACTIVE_MAX_INSTANCES 8

typedef struct {
    Ca_Instance *inst;

    Ca_Signal *signals;
    Ca_Effect *effects;

    /* Tracking stack — current top is the effect being executed.
       NULL means "no current effect" (untracked read). */
    Ca_Effect *track_stack[64];
    int        track_top;        /* -1 = empty */

    int        batch_depth;

    /* Pending re-run queue (effects that need to be flushed). */
    int32_t    pending[CA_MAX_EFFECTS_PER_INSTANCE];
    uint32_t   pending_count;

    /* Untrack guard: when > 0, reads do not subscribe. */
    int        untrack_depth;
} Ca_Reactive;

static Ca_Reactive g_reactive[CA_REACTIVE_MAX_INSTANCES];

static Ca_Reactive *get_reactive(Ca_Instance *inst)
{
    if (!inst) return NULL;
    /* Find existing. */
    for (int i = 0; i < CA_REACTIVE_MAX_INSTANCES; ++i)
        if (g_reactive[i].inst == inst) return &g_reactive[i];
    /* Lazy-init. */
    for (int i = 0; i < CA_REACTIVE_MAX_INSTANCES; ++i) {
        if (g_reactive[i].inst == NULL) {
            Ca_Reactive *r = &g_reactive[i];
            memset(r, 0, sizeof(*r));
            r->inst = inst;
            r->signals = (Ca_Signal *)calloc(CA_MAX_SIGNALS_PER_INSTANCE, sizeof(Ca_Signal));
            r->effects = (Ca_Effect *)calloc(CA_MAX_EFFECTS_PER_INSTANCE, sizeof(Ca_Effect));
            r->track_top = -1;
            r->batch_depth = 0;
            r->untrack_depth = 0;
            return r;
        }
    }
    fprintf(stderr, "[causality] reactive: exceeded %d instances\n",
            CA_REACTIVE_MAX_INSTANCES);
    return NULL;
}

static void release_reactive(Ca_Instance *inst)
{
    for (int i = 0; i < CA_REACTIVE_MAX_INSTANCES; ++i) {
        Ca_Reactive *r = &g_reactive[i];
        if (r->inst != inst) continue;
        if (r->signals) {
            for (uint32_t s = 0; s < CA_MAX_SIGNALS_PER_INSTANCE; ++s)
                free(r->signals[s].value);
            free(r->signals);
        }
        free(r->effects);
        memset(r, 0, sizeof(*r));
        r->track_top = -1;
        return;
    }
}

/* Public hook so causality.c can release on instance destroy. */
void ca_reactive_release_instance(Ca_Instance *inst) { release_reactive(inst); }

/* Public hook so causality.c can flush pending effects each tick. */
void ca_reactive_flush(Ca_Instance *inst);

/* ============================================================
   Internal helpers
   ============================================================ */

static int32_t signal_index(Ca_Reactive *r, const Ca_Signal *s)
{
    return (int32_t)(s - r->signals);
}

static int32_t effect_index(Ca_Reactive *r, const Ca_Effect *e)
{
    return (int32_t)(e - r->effects);
}

static void schedule_effect(Ca_Reactive *r, Ca_Effect *e)
{
    if (!e || !e->in_use || e->scheduled) return;
    if (r->pending_count >= CA_MAX_EFFECTS_PER_INSTANCE) {
        fprintf(stderr, "[causality] reactive: pending queue full\n");
        return;
    }
    r->pending[r->pending_count++] = effect_index(r, e);
    e->scheduled = true;
}

static void clear_effect_deps(Ca_Reactive *r, Ca_Effect *e)
{
    /* Remove this effect from each dep's subscriber list. */
    for (uint32_t i = 0; i < e->dep_count; ++i) {
        Ca_Signal *s = e->deps[i];
        if (!s || !s->in_use) continue;
        int32_t target = effect_index(r, e);
        for (uint32_t j = 0; j < s->sub_count; ++j) {
            if (s->subs[j] == target) {
                s->subs[j] = s->subs[s->sub_count - 1];
                s->sub_count--;
                break;
            }
        }
    }
    e->dep_count = 0;
}

static void track_dep(Ca_Reactive *r, Ca_Signal *s)
{
    if (r->untrack_depth > 0) return;
    if (r->track_top < 0) return;
    Ca_Effect *e = r->track_stack[r->track_top];
    if (!e || !e->in_use) return;

    /* Already a dep? */
    for (uint32_t i = 0; i < e->dep_count; ++i)
        if (e->deps[i] == s) return;

    if (e->dep_count >= CA_MAX_SIGNAL_DEPS) {
        fprintf(stderr, "[causality] reactive: effect dep cap reached\n");
        return;
    }
    if (s->sub_count >= CA_MAX_SIGNAL_SUBSCRIBERS) {
        fprintf(stderr, "[causality] reactive: signal sub cap reached\n");
        return;
    }
    e->deps[e->dep_count++] = s;
    s->subs[s->sub_count++] = effect_index(r, e);
}

static void run_effect(Ca_Reactive *r, Ca_Effect *e)
{
    if (!e || !e->in_use || !e->fn) return;
    /* Re-track: clear old deps, push, run, pop. */
    clear_effect_deps(r, e);
    if (r->track_top + 1 >= (int)(sizeof(r->track_stack) / sizeof(r->track_stack[0]))) {
        fprintf(stderr, "[causality] reactive: effect nesting too deep\n");
        return;
    }
    r->track_top++;
    r->track_stack[r->track_top] = e;
    e->scheduled = false;
    e->fn(e->user_data);
    r->track_top--;
}

void ca_reactive_flush(Ca_Instance *inst)
{
    Ca_Reactive *r = get_reactive(inst);
    if (!r) return;
    /* Process queue, picking up newly scheduled effects too. */
    while (r->pending_count > 0) {
        int32_t idx = r->pending[--r->pending_count];
        Ca_Effect *e = &r->effects[idx];
        run_effect(r, e);
    }
}

/* ============================================================
   Signal API
   ============================================================ */

Ca_Signal *ca_signal_create(Ca_Instance *inst, size_t value_size, const void *initial)
{
    Ca_Reactive *r = get_reactive(inst);
    if (!r) return NULL;
    assert(value_size > 0);
    for (uint32_t i = 0; i < CA_MAX_SIGNALS_PER_INSTANCE; ++i) {
        Ca_Signal *s = &r->signals[i];
        if (s->in_use) continue;
        memset(s, 0, sizeof(*s));
        s->inst       = inst;
        s->value_size = (uint32_t)value_size;
        s->value      = (uint8_t *)calloc(1, value_size);
        s->in_use     = true;
        if (initial) memcpy(s->value, initial, value_size);
        return s;
    }
    fprintf(stderr, "[causality] ca_signal_create: pool exhausted (%d)\n",
            CA_MAX_SIGNALS_PER_INSTANCE);
    return NULL;
}

Ca_Signal *ca_signal_int  (Ca_Instance *i, int      v) { return ca_signal_create(i, sizeof(int),      &v); }
Ca_Signal *ca_signal_float(Ca_Instance *i, float    v) { return ca_signal_create(i, sizeof(float),    &v); }
Ca_Signal *ca_signal_bool (Ca_Instance *i, bool     v) { return ca_signal_create(i, sizeof(bool),     &v); }
Ca_Signal *ca_signal_u32  (Ca_Instance *i, uint32_t v) { return ca_signal_create(i, sizeof(uint32_t), &v); }
Ca_Signal *ca_signal_ptr  (Ca_Instance *i, void    *v) { return ca_signal_create(i, sizeof(void *),   &v); }

void ca_signal_destroy(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return;
    Ca_Reactive *r = get_reactive(sig->inst);
    if (r) {
        /* Detach all subscribers (their deps array may still point here;
           clear lazily on next run). */
        sig->sub_count = 0;
    }
    free(sig->value);
    memset(sig, 0, sizeof(*sig));
}

/* Recompute a computed-signal's value if its body is registered and dirty. */
static void maybe_recompute(Ca_Signal *s)
{
    if (!s->compute_fn || !s->dirty_computed) return;
    Ca_Reactive *r = get_reactive(s->inst);
    if (!r) return;
    /* Reuse the owning effect to retrack deps. */
    s->dirty_computed = false;
    if (s->owner_effect) {
        clear_effect_deps(r, s->owner_effect);
        if (r->track_top + 1 < (int)(sizeof(r->track_stack)/sizeof(r->track_stack[0]))) {
            r->track_top++;
            r->track_stack[r->track_top] = s->owner_effect;
            const void *new_val = s->compute_fn(s->compute_user);
            r->track_top--;
            if (new_val && memcmp(s->value, new_val, s->value_size) != 0) {
                memcpy(s->value, new_val, s->value_size);
                s->generation++;
            }
        }
    }
}

const void *ca_signal_get(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return NULL;
    Ca_Reactive *r = get_reactive(sig->inst);
    if (!r) return NULL;
    maybe_recompute(sig);
    track_dep(r, sig);
    return sig->value;
}

int      ca_signal_get_int   (Ca_Signal *s) { const int      *p = (const int      *)ca_signal_get(s); return p ? *p : 0; }
float    ca_signal_get_float (Ca_Signal *s) { const float    *p = (const float    *)ca_signal_get(s); return p ? *p : 0.0f; }
bool     ca_signal_get_bool  (Ca_Signal *s) { const bool     *p = (const bool     *)ca_signal_get(s); return p ? *p : false; }
uint32_t ca_signal_get_u32   (Ca_Signal *s) { const uint32_t *p = (const uint32_t *)ca_signal_get(s); return p ? *p : 0u; }
void    *ca_signal_get_ptr   (Ca_Signal *s) { void *const *p = (void *const *)ca_signal_get(s); return p ? *p : NULL; }

const void *ca_signal_peek(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return NULL;
    maybe_recompute(sig);
    return sig->value;
}

void ca_signal_notify(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return;
    Ca_Reactive *r = get_reactive(sig->inst);
    if (!r) return;

    sig->generation++;

    /* Mark dependent computeds dirty. */
    /* (Computeds subscribe via their owner effects; same path as effects.) */

    /* Schedule subscriber effects. */
    for (uint32_t j = 0; j < sig->sub_count; ++j) {
        int32_t idx = sig->subs[j];
        if (idx < 0 || idx >= (int32_t)CA_MAX_EFFECTS_PER_INSTANCE) continue;
        Ca_Effect *e = &r->effects[idx];
        if (e->computed_target) {
            /* This effect drives a computed signal — mark it dirty so the
               next read recomputes. */
            e->computed_target->dirty_computed = true;
        }
        schedule_effect(r, e);
    }

    /* If we are inside a batch, defer flushing. Otherwise flush immediately. */
    if (r->batch_depth == 0)
        ca_reactive_flush(sig->inst);
}

void ca_signal_set(Ca_Signal *sig, const void *value)
{
    if (!sig || !sig->in_use || !value) return;
    if (memcmp(sig->value, value, sig->value_size) == 0) return;
    memcpy(sig->value, value, sig->value_size);
    ca_signal_notify(sig);
}

void ca_signal_set_int   (Ca_Signal *s, int      v) { ca_signal_set(s, &v); }
void ca_signal_set_float (Ca_Signal *s, float    v) { ca_signal_set(s, &v); }
void ca_signal_set_bool  (Ca_Signal *s, bool     v) { ca_signal_set(s, &v); }
void ca_signal_set_u32   (Ca_Signal *s, uint32_t v) { ca_signal_set(s, &v); }
void ca_signal_set_ptr   (Ca_Signal *s, void    *v) { ca_signal_set(s, &v); }

void *ca_signal_mut(Ca_Signal *sig)
{
    if (!sig || !sig->in_use) return NULL;
    return sig->value;
}

/* ============================================================
   Effect API
   ============================================================ */

Ca_Effect *ca_effect(Ca_Instance *inst, Ca_EffectFn fn, void *user_data)
{
    Ca_Reactive *r = get_reactive(inst);
    if (!r || !fn) return NULL;
    for (uint32_t i = 0; i < CA_MAX_EFFECTS_PER_INSTANCE; ++i) {
        Ca_Effect *e = &r->effects[i];
        if (e->in_use) continue;
        memset(e, 0, sizeof(*e));
        e->inst      = inst;
        e->fn        = fn;
        e->user_data = user_data;
        e->in_use    = true;
        /* Run once to capture deps. */
        run_effect(r, e);
        return e;
    }
    fprintf(stderr, "[causality] ca_effect: pool exhausted (%d)\n",
            CA_MAX_EFFECTS_PER_INSTANCE);
    return NULL;
}

void ca_effect_destroy(Ca_Effect *eff)
{
    if (!eff || !eff->in_use) return;
    Ca_Reactive *r = get_reactive(eff->inst);
    if (r) clear_effect_deps(r, eff);
    memset(eff, 0, sizeof(*eff));
}

void ca_effect_invalidate(Ca_Effect *eff)
{
    if (!eff || !eff->in_use) return;
    Ca_Reactive *r = get_reactive(eff->inst);
    if (!r) return;
    schedule_effect(r, eff);
    if (r->batch_depth == 0) ca_reactive_flush(eff->inst);
}

/* ============================================================
   Computed
   ============================================================ */

typedef struct {
    Ca_Signal   *target;
    Ca_ComputeFn fn;
    void        *user;
} Ca_ComputedCtx;

static void computed_effect_fn(void *user)
{
    Ca_ComputedCtx *c = (Ca_ComputedCtx *)user;
    if (!c || !c->target || !c->target->in_use) return;
    const void *new_val = c->fn(c->user);
    if (!new_val) return;
    if (memcmp(c->target->value, new_val, c->target->value_size) != 0) {
        memcpy(c->target->value, new_val, c->target->value_size);
        ca_signal_notify(c->target);
    }
}

Ca_Signal *ca_computed(Ca_Instance *inst,
                       size_t        value_size,
                       Ca_ComputeFn  fn,
                       void         *user_data)
{
    Ca_Signal *s = ca_signal_create(inst, value_size, NULL);
    if (!s || !fn) return s;
    Ca_ComputedCtx *c = (Ca_ComputedCtx *)calloc(1, sizeof(*c));
    c->target = s;
    c->fn     = fn;
    c->user   = user_data;
    s->compute_fn   = fn;
    s->compute_user = user_data;
    /* Drive recomputation through an effect; the effect ties into the
       same dep-tracking machinery as a normal user effect. */
    Ca_Effect *e = ca_effect(inst, computed_effect_fn, c);
    s->owner_effect = e;
    if (e) e->computed_target = s;
    return s;
}

/* ============================================================
   Batching / untrack
   ============================================================ */

void ca_batch_begin(Ca_Instance *inst)
{
    Ca_Reactive *r = get_reactive(inst);
    if (r) r->batch_depth++;
}

void ca_batch_end(Ca_Instance *inst)
{
    Ca_Reactive *r = get_reactive(inst);
    if (!r) return;
    if (r->batch_depth > 0) r->batch_depth--;
    if (r->batch_depth == 0) ca_reactive_flush(inst);
}

void ca_untrack(Ca_Instance *inst, void (*fn)(void *), void *user_data)
{
    Ca_Reactive *r = get_reactive(inst);
    if (!r || !fn) return;
    r->untrack_depth++;
    fn(user_data);
    r->untrack_depth--;
}
