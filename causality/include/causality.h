#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAUSALITY_VERSION_MAJOR 0
#define CAUSALITY_VERSION_MINOR 1
#define CAUSALITY_VERSION_PATCH 0

/* Maximum number of simultaneously open windows per instance. */
#define CA_MAX_WINDOWS 8

/* ---- Opaque handles ---- */

typedef struct Ca_Instance Ca_Instance;
typedef struct Ca_Window   Ca_Window;
typedef struct Ca_Thread   Ca_Thread;

/* ---- Descriptors ---- */

typedef struct Ca_InstanceDesc {
    const char *app_name;
} Ca_InstanceDesc;

typedef struct Ca_WindowDesc {
    const char *title;
    int         width;
    int         height;
} Ca_WindowDesc;

/* ---- Events ---- */

/* Key / button action values (mirror GLFW). */
#define CA_RELEASE 0
#define CA_PRESS   1
#define CA_REPEAT  2

typedef enum Ca_EventType {
    CA_EVENT_NONE = 0,
    CA_EVENT_WINDOW_CLOSE,
    CA_EVENT_WINDOW_RESIZE,
    CA_EVENT_KEY,
    CA_EVENT_MOUSE_BUTTON,
    CA_EVENT_MOUSE_MOVE,
    CA_EVENT_MOUSE_SCROLL,
    CA_EVENT_TYPE_COUNT
} Ca_EventType;

typedef struct Ca_Event {
    Ca_EventType  type;
    Ca_Window    *window;
    union {
        struct { int width, height; }               resize;
        struct { int key, scancode, action, mods; } key;
        struct { int button, action, mods; }        mouse_button;
        struct { double x, y; }                     mouse_pos;
        struct { double dx, dy; }                   mouse_scroll;
    };
} Ca_Event;

typedef void (*Ca_EventFn)(const Ca_Event *event, void *user_data);

/* Register a handler for an event type. Pass NULL fn to remove. */
void ca_event_set_handler(Ca_Instance *instance, Ca_EventType type,
                          Ca_EventFn fn, void *user_data);

/* ---- Instance ---- */

Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc);
void         ca_instance_destroy(Ca_Instance *instance);
int          ca_instance_exec(Ca_Instance *instance);

/* ---- Window ---- */

Ca_Window *ca_window_create(Ca_Instance *instance, const Ca_WindowDesc *desc);
void       ca_window_destroy(Ca_Window *window);

/* ---- Threads ---- */

typedef void *(*Ca_ThreadFn)(void *user_data);

Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data);
void       ca_thread_join(Ca_Thread *thread);    /* blocks until done, frees handle */
void       ca_thread_detach(Ca_Thread *thread);  /* fire-and-forget, frees handle */

#ifdef __cplusplus
}
#endif

