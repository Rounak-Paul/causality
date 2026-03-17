/* ca_internal.h — internal struct definitions, never exposed publicly */
#pragma once

#include "causality.h"
#include <pthread.h>
#include <stdint.h>

#define CA_EVENT_QUEUE_CAPACITY 256

typedef struct {
    Ca_EventFn  fn;
    void       *user_data;
} Ca_EventHandler;

struct Ca_Window {
    GLFWwindow  *glfw;
    Ca_Instance *instance;  /* back-pointer for GLFW callbacks */
    bool         in_use;
};

struct Ca_Instance {
    Ca_Window        windows[CA_MAX_WINDOWS];

    /* Event ring-buffer */
    Ca_Event         event_queue[CA_EVENT_QUEUE_CAPACITY];
    uint32_t         event_head;
    uint32_t         event_tail;
    pthread_mutex_t  event_mutex;

    /* Per-type user handlers */
    Ca_EventHandler  handlers[CA_EVENT_TYPE_COUNT];
};

struct Ca_Thread {
    pthread_t handle;
};
