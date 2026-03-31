/* state.h — internal header for the reactive state system */
#pragma once

#include <stddef.h>
#include "ca_internal.h"

void ca_state_system_init(Ca_Instance *inst);
void ca_state_system_shutdown(Ca_Instance *inst);

/* Propagate dirty flags from states to their subscriber nodes. */
void ca_state_flush_dirty(Ca_Instance *inst);

Ca_State *ca_state_create(Ca_Instance *instance, size_t data_size, const void *initial);
void      ca_state_destroy(Ca_State *state);
void      ca_state_set(Ca_State *state, const void *value);
void      ca_state_get(const Ca_State *state, void *out_value);
uint64_t  ca_state_generation(const Ca_State *state);
void      ca_state_observe(Ca_State *state,
                            void (*fn)(const void *value, void *user),
                            void *user);
