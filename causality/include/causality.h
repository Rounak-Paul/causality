// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "causality_config.h"
#include "ca_api.h"
#include "ca_icons.h"
#include "ca_reactive.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAUSALITY_VERSION_MAJOR 0
#define CAUSALITY_VERSION_MINOR 1
#define CAUSALITY_VERSION_PATCH 0

/* Maximum number of simultaneously open windows per instance. */
#define CA_MAX_WINDOWS 8
/* Reserved windows are owned by Causality internals (e.g. popup manager).
   They do not count against CA_MAX_WINDOWS. */
#define CA_RESERVED_POPUP_WINDOWS 1
#define CA_MAX_WINDOWS_TOTAL (CA_MAX_WINDOWS + CA_RESERVED_POPUP_WINDOWS)

/* ---- Opaque handles ---- */

typedef struct Ca_Instance Ca_Instance;
typedef struct Ca_Window   Ca_Window;
typedef struct Ca_Thread   Ca_Thread;
typedef struct Ca_Mutex    Ca_Mutex;
typedef struct Ca_CondVar  Ca_CondVar;

/* ---- Widget handles ---- */

typedef struct Ca_Label     Ca_Label;
typedef struct Ca_Button    Ca_Button;
typedef struct Ca_TextInput Ca_TextInput;
typedef struct Ca_Splitter  Ca_Splitter;
typedef struct Ca_Image     Ca_Image;
typedef struct Ca_Node      Ca_Div;

/* Component widget handles — defined in ca_components.h */
typedef struct Ca_Checkbox  Ca_Checkbox;
typedef struct Ca_Radio     Ca_Radio;
typedef struct Ca_Slider    Ca_Slider;
typedef struct Ca_Toggle    Ca_Toggle;
typedef struct Ca_Progress  Ca_Progress;
typedef struct Ca_Select    Ca_Select;
typedef struct Ca_TabBar    Ca_TabBar;
typedef struct Ca_TreeNode  Ca_TreeNode;
typedef struct Ca_Table     Ca_Table;
typedef struct Ca_Tooltip   Ca_Tooltip;
typedef struct Ca_CtxMenu   Ca_CtxMenu;
typedef struct Ca_Modal     Ca_Modal;
typedef struct Ca_MenuBar   Ca_MenuBar;

/* ============================================================
   INSTANCE
   ============================================================ */

typedef struct Ca_InstanceDesc {
    const char *app_name;
    bool        prefer_dedicated_gpu;
    /* Font — leave NULL to use the built-in Roboto Mono Nerd Font text layer
       plus Symbols Nerd Font icon layer.
       Set font_path to override text with a custom .ttf or .otf file.
       The bundled Symbols icon layer remains active for CA_ICON_* glyphs
       even when the regular/bold text faces are overridden. */
    const char *font_path;       /* path to regular .ttf or .otf, or NULL for embedded */
    const char *bold_font_path;  /* path to bold .ttf or .otf, or NULL for embedded bold */
    /* Default UI scale inherited by every window created on this instance.
       0.0 / 1.0 both mean no scaling.  Clamped to [0.25, 4.0] at init. */
    float       default_ui_scale;
} Ca_InstanceDesc;

/*
 * Create a new Causality instance, initialising Vulkan and the font system.
 *
 * desc      Description of the instance to create.
 * Returns   A heap-allocated Ca_Instance, or NULL on failure.
 */
CA_API Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc);

/* Destroy the instance and release all associated GPU and CPU resources. */
CA_API void         ca_instance_destroy(Ca_Instance *instance);

/* Sets a custom allocator for all Causality internal heap allocations.
   Must be called before ca_instance_create().  Pass NULL for any function
   to keep the current binding (defaults to the standard-library equivalents). */
CA_API void ca_set_allocator(Ca_MallocFn mal, Ca_CallocFn cal,
                              Ca_ReallocFn ral, Ca_FreeFn fre);

/* Pumps the event loop: processes window events, updates UI, renders one frame.
   Returns false when all windows have been closed. */
CA_API bool         ca_instance_tick(Ca_Instance *instance);

/* Wake the event loop from another thread (e.g. after posting async data). */
CA_API void         ca_instance_wake(void);

/**
 * Request an event-loop frame after a minimum delay.
 *
 * Repeated requests keep the earliest deadline. This function must be called
 * from the UI thread.
 *
 * @param instance Instance whose event loop should wake.
 * @param delay_seconds Non-negative delay in seconds.
 */
CA_API void ca_instance_request_frame_after(Ca_Instance *instance,
                                             double delay_seconds);

/* Enable or disable continuous rendering mode.
   When true, ca_instance_tick uses glfwPollEvents() so the loop runs every
   frame regardless of input — required for smooth camera / game-loop behaviour.
   When false (default), the loop sleeps until an event or explicit wake. */
CA_API void         ca_instance_set_continuous(Ca_Instance *instance, bool continuous);

/* ============================================================
   WINDOW
   ============================================================ */

typedef struct Ca_WindowDesc {
    const char *title;
    int         width;
    int         height;
} Ca_WindowDesc;

/*
 * Create a new platform window belonging to the given instance.
 *
 * instance  Owning Ca_Instance.
 * desc      Window title, width, and height.
 * Returns   Newly created Ca_Window, or NULL on failure.
 */
CA_API Ca_Window *ca_window_create(Ca_Instance *instance, const Ca_WindowDesc *desc);

/* Immediately destroy a window and release its swap-chain resources. */
CA_API void       ca_window_destroy(Ca_Window *window);

/* Returns the Ca_Instance that owns this window. */
CA_API Ca_Instance *ca_window_instance(Ca_Window *window);

/* Request the window to close at the end of the current tick.
   Safe to call from button callbacks or any other context.
   The window is fully destroyed by the event loop on the next frame. */
CA_API void       ca_window_close(Ca_Window *window);
/* Maximize the window to fill the screen. */
CA_API void       ca_window_maximize(Ca_Window *window);
/* Restore a maximized window to its previous geometry. */
CA_API void       ca_window_restore(Ca_Window *window);

/* Returns true if the window handle is valid and still open. */
CA_API bool       ca_window_is_open(const Ca_Window *window);

/*
 * Callback invoked each frame to render a background directly into the
 * swapchain image, BEFORE any UI elements are composited.  The swapchain is
 * presented with LOAD_OP_LOAD so this content shows through transparent UI.
 *
 * cmd              Command buffer to record into (already begun).
 * window           Window whose swapchain is being rendered.
 * swapchain_image  VkImage of the current swapchain image (needed for image barriers).
 * swapchain_view   VkImageView of the current swapchain image (COLOR_ATTACHMENT_OPTIMAL).
 * format           VkFormat of the swapchain image.
 * image_usage      Usage flags enabled for the swapchain image.
 * frame_slot       In-flight frame slot whose fence has already completed.
 * width, height    Pixel dimensions of the swapchain image.
 * user_data        Pointer passed to ca_window_set_bg_render.
 */
typedef bool (*Ca_BgRenderFn)(VkCommandBuffer cmd,
                              Ca_Window       *window,
                              VkImage         swapchain_image,
                              VkImageView     swapchain_view,
                              VkFormat        format,
                              VkImageUsageFlags image_usage,
                              uint32_t        frame_slot,
                              uint32_t        width,
                              uint32_t        height,
                              void           *user_data);

/*
 * Register or replace the per-window background render callback.
 * Pass NULL for fn to disable background rendering.
 *
 * window     Target window.
 * fn         Callback to invoke before each frame's UI render pass (NULL = none).
 * user_data  Passed unchanged to fn.
 */
CA_API void ca_window_set_bg_render(Ca_Window *window, Ca_BgRenderFn fn, void *user_data);

/*
 * Set the background callback inherited by every window that has no explicit
 * per-window callback. Existing and subsequently created windows both use it.
 * Pass NULL for fn to clear the fallback.
 */
CA_API void ca_instance_set_bg_render(Ca_Instance *instance,
                                      Ca_BgRenderFn fn,
                                      void *user_data);

/* Clipboard helpers for text content. Clipboard ownership remains with the
   platform backend; returned text is valid until the next clipboard update. */
/*
 * Write a UTF-8 string to the system clipboard.
 *
 * window  Window whose platform context owns the clipboard.
 * text    Null-terminated UTF-8 string to set.
 */
CA_API void        ca_clipboard_set_text(Ca_Window *window, const char *text);

/*
 * Read the current UTF-8 text from the system clipboard.
 *
 * window  Window whose platform context owns the clipboard.
 * Returns Pointer to the clipboard text, valid until the next clipboard update.
 */
CA_API const char *ca_clipboard_get_text(Ca_Window *window);

/* Compatibility aliases for instance-wide UI scale.  Causality intentionally
   has one global UI scale per instance; calling this on any window updates
   every open window and the scale inherited by future windows. */
/*
 * Set the instance-wide UI scale via a window handle (compatibility alias).
 *
 * window  Any open window belonging to the instance.
 * scale   New scale factor; clamped to [0.25, 4.0].
 */
CA_API void       ca_window_set_scale(Ca_Window *window, float scale);

/* Returns the current instance-wide UI scale via a window handle. */
CA_API float      ca_window_get_scale(Ca_Window *window);

/* Return framebuffer pixels per logical window unit for the current display. */
CA_API float      ca_window_get_pixel_ratio(Ca_Window *window);

/* Return the resolved logical height reserved for the system title bar. */
CA_API float      ca_window_get_title_bar_height(Ca_Window *window);

/* Instance-wide UI scale — like browser zoom.
   1.0 = 100% (default), 1.5 = 150%, 2.0 = 200%, etc.
   Affects all widget sizes, paddings, gaps, and text rendering.
   Every existing open window is rescaled, and every future window inherits
   the same value.  Clamped to [0.25, 4.0]. */
/*
 * Set the instance-wide UI scale factor.
 *
 * instance  Owning Ca_Instance.
 * scale     New scale factor; clamped to [0.25, 4.0].
 */
CA_API void  ca_instance_set_scale(Ca_Instance *instance, float scale);

/* Returns the current instance-wide UI scale factor. */
CA_API float ca_instance_get_scale(const Ca_Instance *instance);

/* Set the window title displayed in the custom title bar. */
CA_API void ca_window_set_title(Ca_Window *window, const char *title);

/* ============================================================
   POPUP CONTROL (instance-managed message/confirm windows)
   ============================================================ */

typedef enum Ca_PopupButtons {
   CA_POPUP_BUTTONS_OK = 0,
   CA_POPUP_BUTTONS_OK_CANCEL,
   CA_POPUP_BUTTONS_YES_NO,
} Ca_PopupButtons;

typedef enum Ca_PopupResult {
   CA_POPUP_RESULT_NONE = 0,
   CA_POPUP_RESULT_OK,
   CA_POPUP_RESULT_CANCEL,
   CA_POPUP_RESULT_YES,
   CA_POPUP_RESULT_NO,
   CA_POPUP_RESULT_CLOSED,
   CA_POPUP_RESULT_REPLACED,
} Ca_PopupResult;

/*
 * Callback invoked when a popup is dismissed.
 *
 * result     Which button the user pressed (or CLOSED/REPLACED).
 * user_data  Value passed via Ca_PopupDesc.result_data.
 */
typedef void (*Ca_PopupResultFn)(Ca_PopupResult result, void *user_data);

typedef struct Ca_PopupDesc {
   const char       *title;          /* NULL -> "Message" */
   const char       *message;        /* NULL -> "" */
   Ca_PopupButtons   buttons;        /* default: OK */
   bool              replace_active; /* true -> replace current popup */
   bool              queue_if_busy;  /* true -> enqueue when busy */
   Ca_PopupResultFn  on_result;      /* optional callback */
   void             *result_data;
} Ca_PopupDesc;

/*
 * Show a popup using Causality's reserved popup window control.
 *
 * instance  Owning Ca_Instance.
 * desc      Popup title, message, button set, and optional result callback.
 * Returns   false if the request is rejected (busy with no queue), true otherwise.
 */
CA_API bool ca_popup_show(Ca_Instance *instance, const Ca_PopupDesc *desc);

/* Returns true when a popup is currently visible or being displayed. */
CA_API bool ca_popup_is_active(const Ca_Instance *instance);

/* Drop all queued popup requests without closing the currently active popup. */
CA_API void ca_popup_clear_queue(Ca_Instance *instance);

/* ============================================================
   STATUS BAR (system-managed bottom strip)
   ============================================================

   Causality reserves a horizontal strip at the bottom of the window for
   a status bar. It is laid out automatically as a sibling of the user's
   content_root, so user code never has to subtract the bar's height
   from manual layout calculations — just fill content_root and the
   status bar lives below it.

   The builder runs every time the bar is invalidated. Inside the builder
   the widget context is already inside the system-managed status bar
   node, so the user just emits children (no outer ca_div_begin/_end
   needed).

   Pass fn = NULL to hide the bar. Setting height = 0 also hides it.
   Height is specified in logical UI units and is scaled by window UI scale. */
/*
 * Builder callback invoked each time the status bar needs to be rebuilt.
 *
 * window     Window that owns the status bar.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_StatusBarFn)(Ca_Window *window, void *user_data);

/*
 * Attach a status-bar builder to a window.
 *
 * window     Target window.
 * fn         Builder function; pass NULL to hide the bar.
 * user_data  Passed to fn on each rebuild.
 * height     Bar height in logical UI units (0 also hides the bar).
 */
CA_API void ca_window_set_status_bar(Ca_Window      *window,
                                     Ca_StatusBarFn  fn,
                                     void           *user_data,
                                     float           height);

/* Force a status-bar rebuild on the next frame (e.g. when the data the
   builder reads has changed). Equivalent to invalidating the bar's
   internal effect. */
CA_API void ca_window_invalidate_status_bar(Ca_Window *window);

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
    CA_EVENT_CHAR,
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
        struct { uint32_t codepoint; }              character;
        struct { int button, action, mods; }        mouse_button;
        struct { double x, y; }                     mouse_pos;
        struct { double dx, dy; }                   mouse_scroll;
    };
} Ca_Event;

/*
 * Callback invoked when an event of the registered type is dispatched.
 *
 * event      The incoming event (valid only for the duration of the call).
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_EventFn)(const Ca_Event *event, void *user_data);

/*
 * Register a handler for a specific event type on an instance.
 *
 * instance   Owning Ca_Instance.
 * type       The event type to listen for.
 * fn         Handler function (NULL to clear).
 * user_data  Passed to fn on each event.
 */
CA_API void ca_event_set_handler(Ca_Instance *instance, Ca_EventType type,
                                 Ca_EventFn fn, void *user_data);

/* ============================================================
   THREADS
   ============================================================ */

/*
 * Thread entry-point function.
 *
 * user_data  Caller-supplied context pointer passed to ca_thread_create.
 * Returns    Arbitrary pointer value (retrievable after ca_thread_join).
 */
typedef void *(*Ca_ThreadFn)(void *user_data);

/*
 * Spawn a new platform thread running fn.
 *
 * fn         Thread entry point.
 * user_data  Passed to fn.
 * Returns    Newly created Ca_Thread handle.
 */
CA_API Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data);

/* Block until the thread exits, then free the handle. */
CA_API void       ca_thread_join(Ca_Thread *thread);

/* Detach the thread so it runs independently; the handle is freed immediately. */
CA_API void       ca_thread_detach(Ca_Thread *thread);

/* Allocate and return a new, unlocked mutex. */
CA_API Ca_Mutex  *ca_mutex_create(void);

/* Destroy a mutex and release its resources. */
CA_API void       ca_mutex_destroy(Ca_Mutex *mutex);

/* Acquire the mutex, blocking until it is available. */
CA_API void       ca_mutex_lock(Ca_Mutex *mutex);

/* Release a previously acquired mutex. */
CA_API void       ca_mutex_unlock(Ca_Mutex *mutex);

/* Attempt to acquire the mutex without blocking; returns true if acquired. */
CA_API bool       ca_mutex_trylock(Ca_Mutex *mutex);

/* Allocate and return a new condition variable. */
CA_API Ca_CondVar *ca_condvar_create(void);

/* Destroy a condition variable and release its resources. */
CA_API void        ca_condvar_destroy(Ca_CondVar *cv);

/*
 * Atomically release the mutex and wait on the condition variable.
 *
 * cv     Condition variable to wait on.
 * mutex  Mutex to release; re-acquired before returning.
 */
CA_API void        ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex);

/* Wake one thread waiting on the condition variable. */
CA_API void        ca_condvar_signal(Ca_CondVar *cv);

/* Wake all threads waiting on the condition variable. */
CA_API void        ca_condvar_broadcast(Ca_CondVar *cv);

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

/*
 * Callback fired when a button is clicked.
 *
 * button     The button that was clicked.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_ClickFn)(Ca_Button *button, void *user_data);

/*
 * Callback fired whenever a text input's content changes.
 *
 * input      The text input that changed.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_ChangeFn)(Ca_TextInput *input, void *user_data);

/* Drag interaction callback.
   'dx' and 'dy' are the delta from the drag start point. */
typedef struct Ca_DragEvent {
    Ca_Window *window;
    float      x, y;           /* current mouse position */
    float      start_x, start_y; /* where mouse was when drag began */
    float      dx, dy;         /* x - start_x, y - start_y */
    /* Mouse position in the receiving node's local coordinate space
       (origin = node's top-left, before content padding).  Useful for
       widgets like a text editor that need to convert a click to a
       (row, column) without looking up the node geometry. */
    float      local_x, local_y;
    float      node_w, node_h;
} Ca_DragEvent;

/*
 * Callback fired on drag-start, drag-move, or drag-end of a div.
 *
 * event      Drag state including position, delta, and node-local coordinates.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_DragFn)(const Ca_DragEvent *event, void *user_data);

/*
 * Callback fired when the scroll wheel or trackpad scrolls over a div.
 *
 * dx         Horizontal scroll delta for this frame (raw GLFW value).
 * dy         Vertical scroll delta for this frame (raw GLFW value).
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_ScrollFn)(double dx, double dy, void *user_data);

/* Layout direction constants */
#define CA_HORIZONTAL 0
#define CA_VERTICAL   1

/* ============================================================
   UI — DECLARATIVE BUILDER (HTML-like)
   ============================================================

   Every element can nest children, just like HTML.
   An implicit parent stack tracks hierarchy automatically.

       ca_ui_begin(window, &(Ca_DivDesc){ .direction = CA_VERTICAL });

         ca_h1(&(Ca_TextDesc){ .text = "My App", .color = WHITE });

         ca_btn_begin(&(Ca_BtnDesc){ .on_click = handler, .background = BLUE });
           ca_text(&(Ca_TextDesc){ .text = "Click me" });
         ca_btn_end();

         ca_list_begin(NULL);
           ca_li_begin(NULL);
             ca_text(&(Ca_TextDesc){ .text = "Item 1" });
           ca_li_end();
         ca_list_end();

         ca_hr(NULL);
         ca_spacer(&(Ca_SpacerDesc){ .height = 16 });

       ca_ui_end();

   Components are plain functions:

       void sidebar(void) {
           ca_div_begin(&(Ca_DivDesc){ .width = 240 });
             ca_text(&(Ca_TextDesc){ .text = "Nav" });
           ca_div_end();
       }
   ============================================================ */

/* ---- Descriptors ---- */

/* Positioning mode constants */
#define CA_POSITION_RELATIVE 0
#define CA_POSITION_ABSOLUTE 1
#define CA_POSITION_FIXED    2

/* <div> — generic layout container. */
typedef struct Ca_DivDesc {
   float    width, height;        /* 0 = auto; use CSS flex-grow to fill   */
    float    padding[4];           /* top, right, bottom, left              */
    float    gap;                  /* space between children                */
    int      direction;            /* CA_HORIZONTAL (0) or CA_VERTICAL (1)  */
    uint32_t background;           /* ca_color(r,g,b,a)                     */
    float    corner_radius;
    const char *id;                /* CSS id  (without #)                   */
    const char *style;             /* space-separated CSS class names       */
    /* Positioning — default 0 (relative, participates in flex flow). */
    int      position;             /* CA_POSITION_RELATIVE / ABSOLUTE / FIXED */
    float    pos_x, pos_y;         /* coordinates when position != RELATIVE  */
    /* Drag interaction callbacks */
    Ca_DragFn on_drag_start;       /* called when drag begins               */
    Ca_DragFn on_drag;             /* called every frame during drag         */
    Ca_DragFn on_drag_end;         /* called when mouse released             */
    void     *drag_data;           /* user_data passed to drag callbacks     */
    /* Border */
    float    border_width;         /* border thickness in px (0 = none)     */
    uint32_t border_color;         /* ca_color(r,g,b,a)                     */
    /* Box shadow */
    float    shadow_offset_x;      /* shadow X offset in px                 */
    float    shadow_offset_y;      /* shadow Y offset in px                 */
    float    shadow_blur;          /* shadow blur radius in px              */
    uint32_t shadow_color;         /* ca_color(r,g,b,a)                     */
    /* Z-index */
    int      z_index;              /* draw order (higher = on top)          */
    /* Scroll interaction callback */
    Ca_ScrollFn on_scroll;         /* called when scroll wheel moves over div */
    void       *scroll_data;       /* user_data passed to on_scroll          */
    /* Visibility / interactivity */
    bool     hidden;               /* display: none — removed from layout   */
    bool     disabled;             /* non-interactive, visually dimmed       */
    bool     no_hover;             /* invisible to hover hit-testing; children
                                      still participate normally               */
} Ca_DivDesc;

/* <p> / text — leaf text element. */
typedef struct Ca_TextDesc {
    const char *text;
    float       width, height;     /* 0 = auto                              */
    uint32_t    color;             /* text foreground colour                */
    bool        wrap;              /* true = multi-line text wrapping       */
    const char *id;                /* CSS id  (without #)                   */
    const char *style;             /* space-separated CSS class names       */
    bool        hidden;            /* display: none — removed from layout   */
} Ca_TextDesc;

/* <button> — clickable nestable element. Use ca_btn_begin / ca_btn_end and
   nest children (text, icons, etc.) inside. */
typedef struct Ca_BtnDesc {
    const char *text;              /* optional inline label                  */
    float       width, height;     /* 0 = auto (72x24)                     */
    float       padding[4];        /* inner padding (for nested content)    */
    float       gap;               /* gap between nested children           */
    int         direction;         /* layout direction for nested children  */
    uint32_t    background;
    uint32_t    text_color;
    float       corner_radius;
    Ca_ClickFn  on_click;          /* NULL = no callback                    */
    void       *click_data;
    const char *id;                /* CSS id  (without #)                   */
    const char *style;             /* space-separated CSS class names       */
    bool        hidden;            /* display: none — removed from layout   */
    bool        disabled;          /* non-interactive, visually dimmed       */
} Ca_BtnDesc;

/* <hr> — horizontal rule / separator. */
typedef struct Ca_HrDesc {
    float    thickness;            /* default 1                             */
    uint32_t color;                /* default grey                          */
    const char *id;                /* CSS id  (without #)                   */
    const char *style;             /* space-separated CSS class names       */
} Ca_HrDesc;

/* spacer — invisible spacing element. */
typedef struct Ca_SpacerDesc {
    float width, height;
    const char *id;                /* CSS id  (without #)                   */
    const char *style;             /* space-separated CSS class names       */
} Ca_SpacerDesc;

typedef enum Ca_InputMode {
    CA_INPUT_TEXT = 0,
    CA_INPUT_FLOAT,
    CA_INPUT_INT,
    CA_INPUT_UINT,
} Ca_InputMode;

/* <input> — single-line text or draggable numeric input field. */
typedef struct Ca_InputDesc {
    const char *text;              /* initial text (NULL = empty)            */
    const char *placeholder;       /* placeholder text (greyed out)          */
    float       width, height;     /* 0 = auto                              */
    uint32_t    text_color;        /* foreground colour                      */
    uint32_t    background;
    float       corner_radius;
    float       padding[4];
    Ca_ChangeFn on_change;         /* called on every edit                   */
    void       *change_data;
    Ca_InputMode input_mode;       /* text or validated numeric editing      */
    float       drag_speed;        /* numeric units per horizontal pixel     */
    const char *id;                /* CSS id  (without #)                    */
    const char *style;             /* space-separated CSS class names        */
    bool        hidden;            /* display: none — removed from layout    */
    bool        disabled;          /* non-interactive, visually dimmed        */
} Ca_InputDesc;

/* ---- Tree root ---- */

/*
 * Begin the per-frame UI tree for a window.
 *
 * window     Target window.
 * root_desc  Descriptor for the root div (layout direction, padding, etc.).
 */
CA_API void ca_ui_begin(Ca_Window *window, const Ca_DivDesc *root_desc);

/* Close the UI tree opened by ca_ui_begin. */
CA_API void ca_ui_end(void);

/*
 * Remove all children from a div and enter it as the current parent.
 *
 * div  Target div to clear and enter; must be paired with ca_div_end().
 */
CA_API void ca_div_clear(Ca_Div *div);

/*
 * Register a reactive builder callback on a div.
 *
 * div        Target div.
 * fn         Builder function; called with the div cleared and entered.
 * user_data  Passed to fn on each rebuild.
 * Returns    The underlying Ca_Effect (owned by the instance).
 */
CA_API Ca_Effect *ca_div_set_builder(Ca_Div *div,
                                     void (*fn)(Ca_Div *div, void *user_data),
                                     void *user_data);

/*
 * Manually trigger a rebuild of the builder registered on this div.
 *
 * div  Div whose builder should be re-run on the next tick.
 */
CA_API void ca_div_invalidate(Ca_Div *div);

/*
 * Enter a div in keyed reconciliation mode.
 *
 * div  Target div; children will be matched/reused by key until ca_div_end().
 */
CA_API void ca_reconcile_begin(Ca_Div *div);

/*
 * Set a one-shot reconciliation key for the next created element.
 *
 * key  Key string that overrides the descriptor id for matching.
 */
CA_API void ca_reconcile_key(const char *key);

/* ---- Container elements (push / pop the parent stack) ---- */

/*
 * Open a new div container and push it onto the parent stack.
 *
 * desc     Descriptor for layout, appearance, and interaction; NULL for defaults.
 * Returns  Handle to the created div.
 */
CA_API Ca_Div *ca_div_begin(const Ca_DivDesc *desc);

/* Pop the current div from the parent stack. */
CA_API void    ca_div_end(void);

/*
 * Open a new button container and push it onto the parent stack.
 *
 * desc     Descriptor for label, appearance, and click callback; NULL for defaults.
 * Returns  Handle to the created button.
 */
CA_API Ca_Button *ca_btn_begin(const Ca_BtnDesc *desc);

/* Pop the current button from the parent stack. */
CA_API void       ca_btn_end(void);

/* Read the most recent click position (in pixels) relative to the
   button's content top-left. Valid only inside an Ca_ClickFn body
   for the same button. Returns true on success, false if the button
   has not been clicked yet or is NULL. */
CA_API bool ca_button_get_click_pos(const Ca_Button *button,
                                    float *out_x, float *out_y);

/* Measure the rendered pixel width of `text` in the window's default
   UI font at `font_size`. Pass `font_size <= 0` to use the font's
   default size. Returns 0 if no font is loaded or the text is empty. */
CA_API float ca_measure_text_px(Ca_Window *window,
                                const char *text,
                                float font_size);

/* Retrieve vertical font metrics in logical pixels for the given size.
   out_ascent  — distance from baseline to the top of the tallest glyph (>0).
   out_descent — distance from baseline to the bottom of deepest descender (<0).
   Both are in the same logical-pixel coordinate space as node positions.
   Returns false if no font is loaded on this window. */
CA_API bool ca_font_line_metrics(Ca_Window *window, float font_size,
                                 float *out_ascent, float *out_descent);

/* Open a vertical list container (gap 4); must be paired with ca_list_end(). */
CA_API void ca_list_begin(const Ca_DivDesc *desc);
CA_API void ca_list_end(void);

/* Open a horizontal list-item container (gap 8); must be paired with ca_li_end(). */
CA_API void ca_li_begin(const Ca_DivDesc *desc);
CA_API void ca_li_end(void);

/* ---- Self-closing elements ---- */

/* Emit a text label; returns the created Ca_Label handle. */
CA_API Ca_Label     *ca_text(const Ca_TextDesc *desc);

/* Emit a single-line text or draggable numeric input field. */
CA_API Ca_TextInput *ca_input(const Ca_InputDesc *desc);

/* Emit a horizontal rule separator. */
CA_API void ca_hr(const Ca_HrDesc *desc);

/* Emit an invisible spacing element. */
CA_API void ca_spacer(const Ca_SpacerDesc *desc);

/* ---- Headings (convenience — text with default heights) ---- */

CA_API Ca_Label *ca_h1(const Ca_TextDesc *desc);  /* heading text at 24 px */
CA_API Ca_Label *ca_h2(const Ca_TextDesc *desc);  /* heading text at 20 px */
CA_API Ca_Label *ca_h3(const Ca_TextDesc *desc);  /* heading text at 18 px */
CA_API Ca_Label *ca_h4(const Ca_TextDesc *desc);  /* heading text at 16 px */
CA_API Ca_Label *ca_h5(const Ca_TextDesc *desc);  /* heading text at 14 px */
CA_API Ca_Label *ca_h6(const Ca_TextDesc *desc);  /* heading text at 12 px */

/* ---- Scroll container queries (by CSS id) ---- */

/*
 * Scroll a scroll container to the very top of its content.
 *
 * window  Window owning the container.
 * id      CSS id of the scroll container.
 */
CA_API void ca_scroll_to_top(Ca_Window *window, const char *id);

/*
 * Scroll a scroll container to the very bottom of its content.
 *
 * window  Window owning the container.
 * id      CSS id of the scroll container.
 */
CA_API void ca_scroll_to_bottom(Ca_Window *window, const char *id);

/*
 * Return the current vertical scroll offset of a scroll container in pixels.
 *
 * window  Window owning the container.
 * id      CSS id of the scroll container.
 * Returns Current scroll-Y offset, or 0 if no container with that id exists.
 */
CA_API float ca_get_scroll_y(Ca_Window *window, const char *id);

/*
 * Set the vertical scroll offset of a scroll container, clamped to its viewport.
 *
 * window  Window owning the container.
 * id      CSS id of the scroll container.
 * y       Desired scroll offset in pixels.
 */
CA_API void  ca_set_scroll_y(Ca_Window *window, const char *id, float y);

/* ---- Window callbacks ---- */

/*
 * Register a callback invoked after input processing on each scheduled frame.
 *
 * window     Target window.
 * fn         Function called once per tick; NULL to clear.
 * user_data  Passed to fn each frame.
 */
CA_API void ca_window_set_on_frame(Ca_Window *window, void (*fn)(void *), void *user_data);

/* Component widgets (checkbox, slider, tabs, tree, table, menu bar, etc.)
   are defined in ca_components.h — included automatically below. */

/* ============================================================
   UI — SPLITTER (resizable split container)
   ============================================================

   A splitter divides its area into two panes with a draggable divider.
   Nest exactly two children inside ca_split_begin / ca_split_end.

       ca_split_begin(&(Ca_SplitDesc){ .direction = CA_HORIZONTAL, .ratio = 0.3f });
         ca_div_begin(NULL);  // left pane  (30%)
           ...
         ca_div_end();
         ca_div_begin(NULL);  // right pane (70%)
           ...
         ca_div_end();
       ca_split_end();

   ============================================================ */

typedef struct Ca_SplitDesc {
    int      direction;        /* CA_HORIZONTAL or CA_VERTICAL          */
    float    ratio;            /* 0.0–1.0: fraction for first pane (default 0.5) */
    float    min_ratio;        /* minimum ratio (default 0.1)           */
    float    max_ratio;        /* maximum ratio (default 0.9)           */
    float    bar_size;         /* divider thickness in px (default 4)   */
    uint32_t bar_color;        /* divider colour (default dark grey)    */
    uint32_t bar_hover_color;  /* divider colour when hovered           */
    /* Optional: invoked when the user drags the divider and the
       ratio changes. Use this to persist the new ratio in your own
       data model so subsequent rebuilds pass it back via .ratio. */
    void   (*on_resize)(float ratio, void *user_data);
    void    *user_data;
    const char *id, *style;
} Ca_SplitDesc;

/*
 * Open a resizable split container and push it onto the parent stack.
 *
 * desc     Split direction, initial ratio, limits, and resize callback.
 * Returns  Handle to the created Ca_Splitter.
 */
CA_API Ca_Splitter *ca_split_begin(const Ca_SplitDesc *desc);

/* Pop the current split container from the parent stack. */
CA_API void         ca_split_end(void);

/* Returns the current divider position as a fraction of the total size [0, 1]. */
CA_API float        ca_split_get_ratio(const Ca_Splitter *s);

/*
 * Programmatically move the divider to a new position.
 *
 * s      Target splitter.
 * ratio  New fraction [0, 1] clamped to [min_ratio, max_ratio].
 */
CA_API void         ca_split_set_ratio(Ca_Splitter *s, float ratio);

/* ============================================================
   UI — ABSOLUTE / FIXED POSITIONING
   ============================================================

   Use the position, pos_x, and pos_y fields of Ca_DivDesc to
   place a container outside the normal flex flow:

     ca_div_begin(&(Ca_DivDesc){
         .position = CA_POSITION_ABSOLUTE,
         .pos_x = 100, .pos_y = 50,
         .width = 200, .height = 300,
         .background = ca_color(0.1, 0.1, 0.15, 1),
     });
       ...  // children inside the floating panel
     ca_div_end();

   - ABSOLUTE: positioned relative to nearest positioned ancestor
   - FIXED: positioned relative to the window
   ============================================================ */

/* ============================================================
   IMAGE / TEXTURE RENDERING
   ============================================================

   Load an image from RGBA pixel data or a file path and display
   it as a UI element with ca_image().

   Example:
     Ca_Image *img = ca_image_create(instance, pixels, 64, 64);
     ca_image(&(Ca_ImageDesc){ .image = img, .width = 64, .height = 64 });

   Images are displayed as textured quads using the image pipeline.
   ============================================================ */

typedef struct Ca_ImageDesc {
    Ca_Image   *image;
    float       width, height;     /* display size (0 = use image natural size) */
    float       corner_radius;
    const char *id, *style;
} Ca_ImageDesc;

/* Create an image from raw RGBA pixel data (4 bytes per pixel).
   The pixel data is uploaded to the GPU immediately; the pointer
   is not retained after this call returns. */
CA_API Ca_Image *ca_image_create(Ca_Instance *instance,
                                 const uint8_t *pixels, int width, int height);

/* Destroy an image and release its GPU resources. */
CA_API void ca_image_destroy(Ca_Instance *instance, Ca_Image *image);

/* Display an image as a UI element. */
CA_API void ca_image(const Ca_ImageDesc *desc);

/* ============================================================
   CSS STYLESHEET
   ============================================================

   Parse a CSS stylesheet and attach it to an instance. All windows belonging
   to that instance use it as the author style layer. Causality's system chrome
   retains lower-priority defaults, so applications may override only the
   declarations they need.

   Supported selectors:
     - Type:       div, button, text, image, h1-h6, hr, spacer, list, li
     - Class:      .classname
     - Compound:   div.foo, .a.b
     - Descendant: .parent .child
     - Child:      .parent > .child
     - Comma:      .a, .b
     - Universal:  *

   Supported properties:
     width, height, min-width, max-width, min-height, max-height,
     padding (shorthand + longhands), margin (shorthand + longhands),
     gap, display (flex/block/none), flex-direction, flex-wrap,
     align-items, justify-content, flex-grow, flex-shrink,
     background-color / background, color, border-radius, opacity,
     font-size, overflow (shorthand + overflow-x/y)

   Values: px, %, auto, #hex, rgb(), rgba(), named colours, keywords

   Example:
     ca_instance_load_css(inst,
         ".container { padding: 16px; gap: 8px; flex-direction: column; }\n"
         ".btn-primary { background: #3366ee; color: white; border-radius: 4px; }\n"
     );

   Elements reference classes via the 'style' field of their descriptor:
     ca_div_begin(&(Ca_DivDesc){ .style = "container" });
     ca_btn_begin(&(Ca_BtnDesc){ .text = "OK", .style = "btn-primary" });
   ============================================================ */

typedef struct Ca_Stylesheet Ca_Stylesheet;

/*
 * Parse a CSS string into a Ca_Stylesheet.
 *
 * css_text  Null-terminated CSS source string.
 * Returns   Newly allocated Ca_Stylesheet, or NULL on parse error.
 */
CA_API Ca_Stylesheet *ca_css_parse(const char *css_text);

/* Destroy a Ca_Stylesheet and release its memory. */
CA_API void           ca_css_destroy(Ca_Stylesheet *ss);

/*
 * Attach a parsed author stylesheet to the instance.
 *
 * instance  Owning Ca_Instance.
 * ss        Stylesheet to attach; ownership is NOT transferred — the caller
 *           must keep it alive and destroy it after the instance. NULL clears
 *           author styles while retaining Causality system defaults.
 */
CA_API void ca_instance_set_stylesheet(Ca_Instance *instance, Ca_Stylesheet *ss);

/* Re-resolve CSS for every live node after replacing a stylesheet. */
CA_API void ca_instance_refresh_styles(Ca_Instance *instance);

/* ============================================================
   GPU — Vulkan resource accessors
   ============================================================

   These expose the Vulkan objects owned by causality so that an
   external renderer (e.g. a game engine) can share the same GPU
   context.  The returned handles are owned by causality — do NOT
   destroy them.
   ============================================================ */

/* Returns the VkInstance created by Causality. */
CA_API VkInstance          ca_gpu_instance(Ca_Instance *instance);

/* Returns the VkPhysicalDevice selected during initialisation. */
CA_API VkPhysicalDevice    ca_gpu_physical_device(Ca_Instance *instance);

/* Returns the VkDevice (logical device). */
CA_API VkDevice            ca_gpu_device(Ca_Instance *instance);

/*
 * Return the graphics VkQueue and optionally its queue family index.
 *
 * instance      Owning Ca_Instance.
 * family_index  Written with the queue family index; may be NULL.
 * Returns       The graphics VkQueue.
 */
CA_API VkQueue             ca_gpu_graphics_queue(Ca_Instance *instance, uint32_t *family_index);

/*
 * Return the presentation VkQueue and optionally its queue family index.
 *
 * instance      Owning Ca_Instance.
 * family_index  Written with the queue family index; may be NULL.
 * Returns       The presentation VkQueue.
 */
CA_API VkQueue             ca_gpu_present_queue(Ca_Instance *instance, uint32_t *family_index);

/* Returns the shared graphics-family command pool (buffers are individually resettable). */
CA_API VkCommandPool       ca_gpu_command_pool(Ca_Instance *instance);

/*
 * Find a Vulkan memory type index satisfying the given type bits and property flags.
 *
 * instance    Owning Ca_Instance.
 * type_bits   Bitmask of acceptable memory type indices from VkMemoryRequirements.
 * properties  Required memory property flags.
 * Returns     Matching type index, or UINT32_MAX on failure.
 */
CA_API uint32_t            ca_gpu_find_memory_type(Ca_Instance *instance,
                                                   uint32_t type_bits,
                                                   VkMemoryPropertyFlags properties);

/*
 * Allocate and begin a one-shot command buffer for immediate GPU work.
 *
 * instance  Owning Ca_Instance.
 * Returns   A VkCommandBuffer already in the recording state.
 */
CA_API VkCommandBuffer     ca_gpu_begin_transfer(Ca_Instance *instance);

/*
 * End, submit, wait for, and free a one-shot command buffer.
 *
 * instance  Owning Ca_Instance.
 * cmd       Command buffer returned by ca_gpu_begin_transfer.
 */
CA_API void                ca_gpu_end_transfer(Ca_Instance *instance, VkCommandBuffer cmd);

/*
 * Compile a GLSL source string to a VkShaderModule via shaderc.
 *
 * device      Logical device to create the module on.
 * glsl_source Null-terminated GLSL source code.
 * stage       Shader stage (e.g. VK_SHADER_STAGE_VERTEX_BIT).
 * Returns     The compiled VkShaderModule, or VK_NULL_HANDLE on failure.
 */
CA_API VkShaderModule      ca_shader_compile(VkDevice device,
                                             const char *glsl_source,
                                             VkShaderStageFlagBits stage);

/* ============================================================
   VIEWPORT — offscreen render target widget
   ============================================================

   A viewport is a widget that displays an offscreen-rendered image.
   The engine renders into the viewport's VkImage each frame via a
   callback, and causality composites the result into the UI layout.

   Usage:

     void my_render(Ca_Viewport *vp, void *user_data) {
         VkCommandBuffer cmd = ca_viewport_cmd(vp);
         uint32_t w = ca_viewport_width(vp);
         uint32_t h = ca_viewport_height(vp);
         // record rendering commands...
     }

     ca_viewport(&(Ca_ViewportDesc){
         .width  = 800,
         .height = 600,
         .on_render = my_render,
         .render_data = &my_engine,
     });

   The on_render callback is invoked once per frame before causality
   composites the UI.  Inside the callback the viewport's VkImage is
   already transitioned to COLOR_ATTACHMENT_OPTIMAL.  After the
   callback returns, causality transitions it to SHADER_READ_ONLY
   for compositing.
   ============================================================ */

typedef struct Ca_Viewport Ca_Viewport;

/*
 * Callback invoked each frame to record rendering commands into the viewport.
 *
 * viewport   The viewport to render into.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_ViewportRenderFn)(Ca_Viewport *viewport, void *user_data);

/*
 * Callback invoked when the viewport widget is resized by the layout system.
 *
 * viewport   The resized viewport.
 * width      New pixel width.
 * height     New pixel height.
 * user_data  Caller-supplied context pointer.
 */
typedef void (*Ca_ViewportResizeFn)(Ca_Viewport *viewport,
                                    uint32_t width, uint32_t height,
                                    void *user_data);

typedef struct Ca_ViewportDesc {
    float                width, height;     /* display size (0 = fill parent)    */
    Ca_ViewportRenderFn  on_render;         /* required — called each frame      */
    void                *render_data;
    Ca_ViewportResizeFn  on_resize;         /* optional — called on size change  */
    void                *resize_data;
    VkFormat             format;            /* 0 = VK_FORMAT_R8G8B8A8_UNORM      */
    VkClearColorValue    clear_color;       /* background clear colour           */
    const char          *id, *style;
} Ca_ViewportDesc;

/*
 * Create a viewport widget in the current UI tree.
 *
 * desc     Viewport size, render/resize callbacks, format, and clear color.
 * Returns  Handle to the created Ca_Viewport.
 */
CA_API Ca_Viewport *ca_viewport(const Ca_ViewportDesc *desc);

/* Returns the command buffer to record into during the on_render callback. */
CA_API VkCommandBuffer ca_viewport_cmd(Ca_Viewport *viewport);

/* Returns the current pixel width of the viewport image. */
CA_API uint32_t ca_viewport_width(const Ca_Viewport *viewport);

/* Returns the current pixel height of the viewport image. */
CA_API uint32_t ca_viewport_height(const Ca_Viewport *viewport);

/* Returns the VkImage backing the viewport (useful for explicit barrier/transition). */
CA_API VkImage ca_viewport_image(const Ca_Viewport *viewport);

/* Returns the VkImageView for the viewport's colour attachment. */
CA_API VkImageView ca_viewport_image_view(const Ca_Viewport *viewport);

/* Returns the VkFormat of the viewport's colour attachment. */
CA_API VkFormat ca_viewport_format(const Ca_Viewport *viewport);

/* Returns the Ca_Instance that owns this viewport. */
CA_API Ca_Instance *ca_viewport_instance(Ca_Viewport *viewport);

/* Mark the viewport as needing a redraw on the next frame. */
CA_API void ca_viewport_request_redraw(Ca_Viewport *viewport);

/*
 * Retrieve the viewport's layout-computed screen-space rectangle.
 *
 * viewport  Target viewport.
 * x         Written with the left edge in window coordinates.
 * y         Written with the top edge in window coordinates.
 * w         Written with the layout width in pixels.
 * h         Written with the layout height in pixels.
 */
CA_API void ca_viewport_screen_rect(const Ca_Viewport *viewport,
                                     float *x, float *y, float *w, float *h);

/*
 * Replace the render and resize callbacks on an existing viewport.
 *
 * viewport     Target viewport.
 * on_render    New render callback (NULL keeps the existing one).
 * render_data  User data for on_render.
 * on_resize    New resize callback (NULL keeps the existing one).
 * resize_data  User data for on_resize.
 */
CA_API void ca_viewport_set_callbacks(Ca_Viewport *viewport,
                                      Ca_ViewportRenderFn on_render, void *render_data,
                                      Ca_ViewportResizeFn on_resize, void *resize_data);

/* Component widgets and the reactivity primitives are part of the canonical
   public surface; pull them in here so consumers only need <causality.h>. */
#include "ca_components.h"
#include "ca_reactive.h"

/* ============================================================
   UNIFIED RUNTIME SETTERS — ca_set_style / ca_set_hidden / ca_set_disabled

   These use C11 _Generic to accept ANY widget handle (Ca_Div*, Ca_Button*,
   Ca_Label*, Ca_Checkbox*, etc.) as the first argument.

       Ca_Button *btn = ca_btn_begin(&(Ca_BtnDesc){...});
       ca_set_style(btn, "primary active");
       ca_set_hidden(btn, false);
       ca_set_disabled(btn, true);

       Ca_Div *panel = ca_div_begin(&(Ca_DivDesc){...});
       ca_set_style(panel, "sidebar collapsed");

   ============================================================ */

/* Backing functions (do not call directly — use the macros below) */
CA_API void        ca__set_style_node(Ca_Div *div, const char *style);
CA_API void        ca__set_style_widget(void *widget, const char *style);
CA_API void        ca__set_hidden_node(Ca_Div *div, bool hidden);
CA_API void        ca__set_hidden_widget(void *widget, bool hidden);
CA_API void        ca__set_disabled_node(Ca_Div *div, bool disabled);
CA_API void        ca__set_disabled_widget(void *widget, bool disabled);
CA_API void        ca__set_text(void *widget, const char *text);
CA_API const char *ca__get_text(const void *widget);
CA_API void        ca__set_color(void *widget, uint32_t color);
CA_API void        ca__set_background_node(Ca_Div *div, uint32_t color);
CA_API void        ca__set_background_widget(void *widget, uint32_t color);

/*
 * Return the current text content of a text input.
 *
 * input   Target text input, or NULL.
 * Returns Pointer to the null-terminated UTF-8 text; "" when input is NULL.
 */
CA_API const char *ca_input_text(const Ca_TextInput *input);

/* Returns true when the text input currently holds keyboard focus. */
CA_API bool ca_input_is_focused(const Ca_TextInput *input);

/* Programmatically focus the text input and select all of its content. */
CA_API void ca_input_focus(Ca_TextInput *input);

/*
 * Report whether a key was pressed this frame while the input was active.
 *
 * input     Target text input.
 * glfw_key  GLFW key constant (e.g. GLFW_KEY_ENTER).
 * Returns   true if the key was pressed this frame.
 */
CA_API bool ca_input_key_pressed(const Ca_TextInput *input, int glfw_key);

/*
 * Directly set a div's layout width in pixels, triggering a layout pass.
 *
 * div    Target div.
 * width  New width in logical pixels.
 */
CA_API void  ca_div_set_width(Ca_Div *div, float width);

/* Returns the div's computed pixel width from the last layout pass. */
CA_API float ca_div_get_layout_width(const Ca_Div *div);

/* Returns the div's computed pixel height from the last layout pass. */
CA_API float ca_div_get_layout_height(const Ca_Div *div);

/*
 * Returns the button's inner content dimensions from the last layout pass —
 * i.e., the button's laid-out w/h minus its CSS-applied padding on each axis.
 * Results are clamped to 0.  Both pointers may be NULL.
 *
 * btn      Target button (must be non-NULL).
 * out_w    Receives inner width  (layout_w - pad_left - pad_right).
 * out_h    Receives inner height (layout_h - pad_top  - pad_bottom).
 */
CA_API void ca_btn_get_layout_inner_size(const Ca_Button *btn,
                                         float *out_w, float *out_h);

/*
 * Apply CSS class names to any widget handle.  Accepts Ca_Div* or any other
 * widget pointer and dispatches to the correct backing function automatically.
 *
 * widget  Widget or div handle.
 * style   Space-separated CSS class names.
 */
#define ca_set_style(widget, style) \
    _Generic((widget),              \
        Ca_Div *: ca__set_style_node,  \
        default:  ca__set_style_widget \
    )((widget), (style))

/*
 * Show or hide any widget handle (display:none when hidden is true).
 *
 * widget  Widget or div handle.
 * hidden  true to hide, false to show.
 */
#define ca_set_hidden(widget, hidden) \
    _Generic((widget),                \
        Ca_Div *: ca__set_hidden_node,  \
        default:  ca__set_hidden_widget \
    )((widget), (hidden))

/*
 * Enable or disable any widget handle (non-interactive and dimmed when true).
 *
 * widget    Widget or div handle.
 * disabled  true to disable, false to enable.
 */
#define ca_set_disabled(widget, disabled) \
    _Generic((widget),                    \
        Ca_Div *: ca__set_disabled_node,  \
        default:  ca__set_disabled_widget \
    )((widget), (disabled))

/* Set the text content of a label, button, or input widget. */
#define ca_set_text(widget, text)   ca__set_text((widget), (text))

/* Return the current text content of a label, button, or input widget. */
#define ca_get_text(widget)         ca__get_text((widget))

/* Set the foreground text color of a label or text widget. */
#define ca_set_color(widget, color) ca__set_color((widget), (color))

/* Set the background color of a div or widget. */
#define ca_set_background(widget, color) \
    _Generic((widget),                   \
        Ca_Div *: ca__set_background_node,  \
        default:  ca__set_background_widget \
    )((widget), (color))

/* Title bar menu API — declared here because it requires Ca_MenuDesc
   which is defined in ca_components.h above. */

/*
 * Install a menu strip in the window's custom title bar.
 *
 * window  Target window.
 * menus   Array of Ca_MenuDesc items to deep-copy into the title bar.
 * count   Number of menus; pass 0 (and NULL for menus) to remove all.
 */
CA_API void ca_window_set_title_bar_menus(Ca_Window        *window,
                                          const Ca_MenuDesc *menus, int count);

/* Reactivity is provided by ca_reactive.h (signals / effects / computed). */

#ifdef __cplusplus
}
#endif
