// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* event.h — internal event system */
#pragma once

#include "ca_internal.h"

/*
 * Initialize the event subsystem for an instance.
 *
 * Called once at instance creation to set up the event mutex and the two
 * growable post-side/dispatch-side queues swapped by ca_event_dispatch.
 *
 * inst  Instance whose event system is being initialized.
 */
bool ca_event_init(Ca_Instance *inst);

/*
 * Tear down the event subsystem for an instance.
 *
 * Destroys the event mutex and both queues. Called at instance destruction.
 *
 * inst  Instance whose event system is being shut down.
 */
void ca_event_shutdown(Ca_Instance *inst);

/*
 * Push an event onto the instance's post-side queue (thread-safe).
 *
 * inst   Instance to post to.
 * event  Event to enqueue; copied by value.
 */
void ca_event_post(Ca_Instance *inst, const Ca_Event *event);

/*
 * Drain the posted events and invoke the registered handler for each.
 *
 * Swaps the post-side queue with the dispatch-side queue under the mutex,
 * then processes the dispatch-side queue without holding the lock, allowing
 * new events to be posted concurrently during dispatch.
 *
 * inst  Instance whose events are to be dispatched.
 */
void ca_event_dispatch(Ca_Instance *inst);
