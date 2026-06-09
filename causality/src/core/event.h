// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* event.h — internal event system */
#pragma once

#include "ca_internal.h"

/* Initialise the event subsystem for inst; called once at instance creation. */
void ca_event_init(Ca_Instance *inst);

/* Tear down the event subsystem for inst; called at instance destruction. */
void ca_event_shutdown(Ca_Instance *inst);

/*
 * Push an event onto the ring buffer (thread-safe).
 *
 * inst   Instance to post to.
 * event  Event to enqueue; copied by value.
 */
void ca_event_post(Ca_Instance *inst, const Ca_Event *event);

/* Drain the ring buffer and invoke all registered handlers. */
void ca_event_dispatch(Ca_Instance *inst);
