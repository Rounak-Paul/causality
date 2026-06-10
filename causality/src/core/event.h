// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* event.h — internal event system */
#pragma once

#include "ca_internal.h"

/*
 * Initialize the event subsystem for an instance.
 *
 * Called once at instance creation to set up the event mutex and ring buffer.
 *
 * inst  Instance whose event system is being initialized.
 */
void ca_event_init(Ca_Instance *inst);

/*
 * Tear down the event subsystem for an instance.
 *
 * Destroys the event mutex and clears the pointer. Called at instance destruction.
 *
 * inst  Instance whose event system is being shut down.
 */
void ca_event_shutdown(Ca_Instance *inst);

/*
 * Push an event onto the ring buffer (thread-safe).
 *
 * inst   Instance to post to.
 * event  Event to enqueue; copied by value.
 */
void ca_event_post(Ca_Instance *inst, const Ca_Event *event);

/*
 * Drain the event ring-buffer and invoke the registered handler for each event.
 *
 * Takes a snapshot of the current head/tail under the mutex, then processes
 * all queued events without holding the lock, allowing new events to be posted
 * concurrently during dispatch.
 *
 * inst  Instance whose events are to be dispatched.
 */
void ca_event_dispatch(Ca_Instance *inst);
