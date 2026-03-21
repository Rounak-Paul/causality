#include "event.h"

/* Platform mutex helpers */
#ifdef _WIN32
  #define MUTEX_INIT(m)    InitializeCriticalSection(m)
  #define MUTEX_DESTROY(m) DeleteCriticalSection(m)
  #define MUTEX_LOCK(m)    EnterCriticalSection(m)
  #define MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#else
  #define MUTEX_INIT(m)    pthread_mutex_init(m, NULL)
  #define MUTEX_DESTROY(m) pthread_mutex_destroy(m)
  #define MUTEX_LOCK(m)    pthread_mutex_lock(m)
  #define MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#endif

void ca_event_init(Ca_Instance *inst)
{
    MUTEX_INIT(&inst->event_mutex);
    inst->event_head = 0;
    inst->event_tail = 0;
}

void ca_event_shutdown(Ca_Instance *inst)
{
    MUTEX_DESTROY(&inst->event_mutex);
}

void ca_event_post(Ca_Instance *inst, const Ca_Event *event)
{
    MUTEX_LOCK(&inst->event_mutex);
    uint32_t next = (inst->event_tail + 1) % CA_EVENT_QUEUE_CAPACITY;
    if (next != inst->event_head) {
        inst->event_queue[inst->event_tail] = *event;
        inst->event_tail = next;
    } else {
        fprintf(stderr, "[causality] event queue full, dropping event\n");
    }
    MUTEX_UNLOCK(&inst->event_mutex);
}

void ca_event_dispatch(Ca_Instance *inst)
{
    /* Snapshot under lock, then process without holding the lock */
    MUTEX_LOCK(&inst->event_mutex);
    uint32_t head = inst->event_head;
    uint32_t tail = inst->event_tail;
    inst->event_head = tail; /* consume all */
    MUTEX_UNLOCK(&inst->event_mutex);

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
