/* state.c — reactive state pool */
#include "state.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

void ca_state_system_init(Ca_Instance *inst)
{
    inst->state_pool = (Ca_State *)calloc(CA_MAX_STATES, sizeof(Ca_State));
}

void ca_state_system_shutdown(Ca_Instance *inst)
{
    if (inst->state_pool) {
        for (uint32_t i = 0; i < CA_MAX_STATES; ++i) {
            Ca_State *s = &inst->state_pool[i];
            if (s->in_use) free(s->data);
        }
    }
    free(inst->state_pool);
    inst->state_pool = NULL;
}

void ca_state_flush_dirty(Ca_Instance *inst)
{
    if (!inst->state_pool) return;

    uint32_t count = inst->dirty_state_count;
    for (uint32_t i = 0; i < count; ++i) {
        Ca_State *s = &inst->state_pool[inst->dirty_states[i]];
        if (!s->in_use || !s->dirty) continue;

        for (uint32_t j = 0; j < s->sub_count; ++j) {
            Ca_Node *node = s->subscribers[j];
            if (node && node->in_use)
                node->dirty |= s->sub_flags[j];
        }

        s->dirty = false;
    }
    inst->dirty_state_count = 0;
}

/* ---- Public API ---- */

Ca_State *ca_state_create(Ca_Instance *instance, const Ca_StateDesc *desc)
{
    assert(instance && desc);
    assert(desc->data_size > 0);

    for (uint32_t i = 0; i < CA_MAX_STATES; ++i) {
        Ca_State *s = &instance->state_pool[i];
        if (s->in_use) continue;

        /* Free any previous allocation (shouldn't happen, but safe) */
        free(s->data);
        memset(s, 0, sizeof(*s));

        s->instance  = instance;
        s->data_size = (uint16_t)desc->data_size;
        s->data      = (uint8_t *)calloc(1, desc->data_size);
        s->in_use    = true;
        if (desc->initial)
            memcpy(s->data, desc->initial, desc->data_size);
        return s;
    }

    fprintf(stderr, "[causality] ca_state_create: pool exhausted (max %d)\n", CA_MAX_STATES);
    return NULL;
}

void ca_state_destroy(Ca_State *state)
{
    if (!state || !state->in_use) return;
    free(state->data);
    memset(state, 0, sizeof(*state));
}

void ca_state_set(Ca_State *state, const void *value)
{
    assert(state && state->in_use && value);
    if (memcmp(state->data, value, state->data_size) == 0)
        return; /* value unchanged — skip */
    memcpy(state->data, value, state->data_size);
    state->generation++;
    if (!state->dirty) {
        state->dirty = true;
        Ca_Instance *inst = state->instance;
        uint32_t idx = (uint32_t)(state - inst->state_pool);
        if (inst->dirty_state_count < CA_MAX_STATES)
            inst->dirty_states[inst->dirty_state_count++] = (uint16_t)idx;
    }
}

void ca_state_get(const Ca_State *state, void *out_value)
{
    assert(state && state->in_use && out_value);
    memcpy(out_value, state->data, state->data_size);
}
