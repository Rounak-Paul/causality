#include "event.h"

void ca_event_init(Ca_Instance *inst)
{
    inst->event_mutex = ca_mutex_create();
    inst->event_head = 0;
    inst->event_tail = 0;
}

void ca_event_shutdown(Ca_Instance *inst)
{
    ca_mutex_destroy(inst->event_mutex);
    inst->event_mutex = NULL;
}

void ca_event_post(Ca_Instance *inst, const Ca_Event *event)
{
    ca_mutex_lock(inst->event_mutex);
    uint32_t next = (inst->event_tail + 1) % CA_EVENT_QUEUE_CAPACITY;
    if (next != inst->event_head) {
        inst->event_queue[inst->event_tail] = *event;
        inst->event_tail = next;
    } else {
        fprintf(stderr, "[causality] event queue full, dropping event\n");
    }
    ca_mutex_unlock(inst->event_mutex);
}

void ca_event_dispatch(Ca_Instance *inst)
{
    /* Snapshot under lock, then process without holding the lock */
    ca_mutex_lock(inst->event_mutex);
    uint32_t head = inst->event_head;
    uint32_t tail = inst->event_tail;
    inst->event_head = tail; /* consume all */
    ca_mutex_unlock(inst->event_mutex);

    while (head != tail) {
        const Ca_Event *ev = &inst->event_queue[head];
        if (ev->type > CA_EVENT_NONE && ev->type < CA_EVENT_TYPE_COUNT) {
            const Ca_EventHandler *h = &inst->handlers[ev->type];
            if (h->fn) h->fn(ev, h->user_data);
        }
        head = (head + 1) % CA_EVENT_QUEUE_CAPACITY;
    }
}

void ca_event_set_handler(Ca_Instance *inst, Ca_EventType type,
                          Ca_EventFn fn, void *user_data)
{
    if (type <= CA_EVENT_NONE || type >= CA_EVENT_TYPE_COUNT) return;
    inst->handlers[type].fn        = fn;
    inst->handlers[type].user_data = user_data;
}
