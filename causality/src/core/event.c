// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "event.h"

/*
 * Initialise the event subsystem for an instance.
 *
 * Creates the event mutex and resets the ring-buffer head and tail to zero.
 *
 * inst  Instance whose event system is being initialised.
 */
bool ca_event_init(Ca_Instance *inst)
{
    if (!inst ||
        !ca_dyn_array_init(&inst->event_queue, sizeof(Ca_Event)) ||
        !ca_dyn_array_init(&inst->event_dispatch_queue, sizeof(Ca_Event)))
        return false;
    inst->event_mutex = ca_mutex_create();
    if (!inst->event_mutex) {
        ca_dyn_array_destroy(&inst->event_dispatch_queue);
        ca_dyn_array_destroy(&inst->event_queue);
        return false;
    }
    return true;
}

/*
 * Tear down the event subsystem for an instance.
 *
 * Destroys the event mutex and clears the pointer.
 *
 * inst  Instance whose event system is being shut down.
 */
void ca_event_shutdown(Ca_Instance *inst)
{
    ca_mutex_destroy(inst->event_mutex);
    inst->event_mutex = NULL;
    ca_dyn_array_destroy(&inst->event_dispatch_queue);
    ca_dyn_array_destroy(&inst->event_queue);
}

/*
 * Push an event onto the instance ring-buffer (thread-safe).
 *
 * Acquires the event mutex, writes the event into the next free slot, and
 * advances the tail.  Drops the event with a warning if the buffer is full.
 *
 * inst   Instance to post to.
 * event  Event to enqueue; copied by value.
 */
void ca_event_post(Ca_Instance *inst, const Ca_Event *event)
{
    if (!inst || !event || !inst->event_mutex) return;
    ca_mutex_lock(inst->event_mutex);
    bool queued = ca_dyn_array_push(&inst->event_queue, event);
    ca_mutex_unlock(inst->event_mutex);
    if (!queued)
        fprintf(stderr, "[causality] event queue allocation failed\n");
}

/*
 * Drain the event ring-buffer and invoke the registered handler for each event.
 *
 * Takes a snapshot of the current head/tail under the mutex, then processes
 * all queued events without holding the lock, allowing new events to be posted
 * concurrently during dispatch.
 *
 * inst  Instance whose events are to be dispatched.
 */
void ca_event_dispatch(Ca_Instance *inst)
{
    if (!inst || !inst->event_mutex) return;
    ca_mutex_lock(inst->event_mutex);
    ca_dyn_array_swap(&inst->event_queue, &inst->event_dispatch_queue);
    ca_mutex_unlock(inst->event_mutex);

    for (size_t i = 0; i < inst->event_dispatch_queue.count; ++i) {
        const Ca_Event *ev =
            ca_dyn_array_at_const(&inst->event_dispatch_queue, i);
        if (ev->type > CA_EVENT_NONE && ev->type < CA_EVENT_TYPE_COUNT) {
            const Ca_EventHandler *h = &inst->handlers[ev->type];
            if (h->fn) h->fn(ev, h->user_data);
        }
    }
    ca_dyn_array_clear(&inst->event_dispatch_queue);
}

/*
 * Register a handler callback for a specific event type.
 *
 * Replaces any previously registered handler for the given type.  Pass
 * NULL for fn to unregister.  Silently ignores invalid type values.
 *
 * inst       Instance to configure.
 * type       Event type to handle.
 * fn         Callback invoked on dispatch, or NULL to clear.
 * user_data  Opaque pointer forwarded to fn on each call.
 */
void ca_event_set_handler(Ca_Instance *inst, Ca_EventType type,
                          Ca_EventFn fn, void *user_data)
{
    if (type <= CA_EVENT_NONE || type >= CA_EVENT_TYPE_COUNT) return;
    inst->handlers[type].fn        = fn;
    inst->handlers[type].user_data = user_data;
}
