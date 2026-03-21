/* event.h — internal event system */
#pragma once

#include "ca_internal.h"

/* Called once at instance creation. */
void ca_event_init(Ca_Instance *inst);

/* Called at instance destruction. */
void ca_event_shutdown(Ca_Instance *inst);

/* Thread-safe: push an event onto the ring buffer. */
void ca_event_post(Ca_Instance *inst, const Ca_Event *event);

/* Drain the ring buffer and invoke registered handlers. */
void ca_event_dispatch(Ca_Instance *inst);
