/* state.h — internal header for the reactive state system */
#pragma once

#include "ca_internal.h"

void ca_state_system_init(Ca_Instance *inst);
void ca_state_system_shutdown(Ca_Instance *inst);

/* Propagate dirty flags from states to their subscriber nodes. */
void ca_state_flush_dirty(Ca_Instance *inst);
