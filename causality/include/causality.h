#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/* ---- Widget handles ---- */

typedef struct Ca_Panel  Ca_Panel;
typedef struct Ca_Label  Ca_Label;
typedef struct Ca_Button Ca_Button;

/* ============================================================
   INSTANCE
   ============================================================ */

typedef struct Ca_InstanceDesc {
    const char *app_name;
    bool        prefer_dedicated_gpu;
    /* Font — leave NULL / 0 to skip text rendering */
    const char *font_path;       /* path to a .ttf or .otf file */
    float       font_size_px;    /* desired size in logical pixels (e.g. 18.0) */
} Ca_InstanceDesc;

Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc);
void         ca_instance_destroy(Ca_Instance *instance);

/* Block until all windows are closed, then destroy the instance. */
int          ca_instance_exec(Ca_Instance *instance);

/* ============================================================
   WINDOW
   ============================================================ */

typedef struct Ca_WindowDesc {
    const char *title;
    int         width;
    int         height;
} Ca_WindowDesc;

Ca_Window *ca_window_create(Ca_Instance *instance, const Ca_WindowDesc *desc);
void       ca_window_destroy(Ca_Window *window);

/* Request the window to close at the end of the current tick.
   Safe to call from button callbacks or any other context.
   The window is fully destroyed by the event loop on the next frame. */
void       ca_window_close(Ca_Window *window);

/* ============================================================
   EVENTS
   ============================================================ */

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

void ca_event_set_handler(Ca_Instance *instance, Ca_EventType type,
                          Ca_EventFn fn, void *user_data);

/* ============================================================
   THREADS
   ============================================================ */

typedef void *(*Ca_ThreadFn)(void *user_data);

Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data);
void       ca_thread_join(Ca_Thread *thread);   /* blocks, then frees handle */
void       ca_thread_detach(Ca_Thread *thread); /* fire-and-forget, frees handle */

/* ============================================================
   UI — COLOUR HELPER
   ============================================================ */

/* Pack normalised RGBA [0,1] floats into a uint32 (R in high byte). */
#define ca_color(r, g, b, a)                              \
    (  ((uint32_t)((float)(r) * 255.0f) << 24)            \
     | ((uint32_t)((float)(g) * 255.0f) << 16)            \
     | ((uint32_t)((float)(b) * 255.0f) <<  8)            \
     |  (uint32_t)((float)(a) * 255.0f)          )

/* ============================================================
   UI — WIDGETS
   ============================================================ */

typedef enum Ca_Orientation {
    CA_HORIZONTAL = 0,
    CA_VERTICAL   = 1,
} Ca_Orientation;

/* Panel — layout container with optional background.
   Analogous to a QWidget with a QBoxLayout.            */
typedef struct Ca_PanelDesc {
    float          width, height;       /* 0 = fill available space  */
    float          padding_top;
    float          padding_right;
    float          padding_bottom;
    float          padding_left;
    float          gap;                 /* space between children    */
    Ca_Orientation orientation;
    uint32_t       background;          /* ca_color(r,g,b,a)         */
    float          corner_radius;
} Ca_PanelDesc;

/* Label — static text (GPU text rendering is a future TODO). */
typedef struct Ca_LabelDesc {
    const char *text;
    float       width, height;          /* 0 = auto                  */
    uint32_t    color;                  /* text foreground colour    */
} Ca_LabelDesc;

/* Button — clickable box with a text label. */
typedef struct Ca_ButtonDesc {
    const char *text;
    float       width, height;          /* 0 = auto (120x36)         */
    uint32_t    background;
    uint32_t    text_color;
    float       corner_radius;
} Ca_ButtonDesc;

typedef void (*Ca_ClickFn)(Ca_Button *button, void *user_data);

/* Panels */
Ca_Panel *ca_panel_create(Ca_Window *window, const Ca_PanelDesc *desc);
Ca_Panel *ca_panel_add(Ca_Panel *parent, const Ca_PanelDesc *desc);
void      ca_panel_destroy(Ca_Panel *panel);
void      ca_panel_set_background(Ca_Panel *panel, uint32_t color);

/* Labels */
Ca_Label *ca_label_add(Ca_Panel *parent, const Ca_LabelDesc *desc);
void      ca_label_destroy(Ca_Label *label);
void      ca_label_set_text(Ca_Label *label, const char *text);

/* Buttons */
Ca_Button *ca_button_add(Ca_Panel *parent, const Ca_ButtonDesc *desc);
void       ca_button_destroy(Ca_Button *button);
void       ca_button_set_text(Ca_Button *button, const char *text);
void       ca_button_set_background(Ca_Button *button, uint32_t color);
void       ca_button_on_click(Ca_Button *button, Ca_ClickFn fn, void *user_data);

#ifdef __cplusplus
}
#endif

