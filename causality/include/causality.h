#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "ca_api.h"

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
    /* Font — leave NULL to use the built-in Roboto Mono Nerd Font.
       Set font_path to override with a custom .ttf or .otf file. */
    const char *font_path;       /* path to regular .ttf or .otf, or NULL for embedded */
    const char *bold_font_path;  /* path to bold .ttf or .otf, or NULL for embedded bold */
    float       font_size_px;    /* desired size in logical pixels (default: 12) */
} Ca_InstanceDesc;

CA_API Ca_Instance *ca_instance_create(const Ca_InstanceDesc *desc);
CA_API void         ca_instance_destroy(Ca_Instance *instance);

/* Pumps the event loop: processes window events, updates UI, renders one frame.
   Returns false when all windows have been closed. */
CA_API bool         ca_instance_tick(Ca_Instance *instance);

/* Wake the event loop from another thread (e.g. after posting async data). */
CA_API void         ca_instance_wake(void);

/* Enable or disable continuous rendering mode.
   When true, ca_instance_tick uses glfwPollEvents() so the loop runs every
   frame regardless of input — required for smooth camera / game-loop behaviour.
   When false (default), the loop sleeps until an event arrives, saving CPU
   for idle editor / tool windows. */
CA_API void         ca_instance_set_continuous(Ca_Instance *instance, bool continuous);

/* ============================================================
   WINDOW
   ============================================================ */

typedef struct Ca_WindowDesc {
    const char *title;
    int         width;
    int         height;
} Ca_WindowDesc;

CA_API Ca_Window *ca_window_create(Ca_Instance *instance, const Ca_WindowDesc *desc);
CA_API void       ca_window_destroy(Ca_Window *window);

/// Returns the Ca_Instance that owns this window.
CA_API Ca_Instance *ca_window_instance(Ca_Window *window);

/* Request the window to close at the end of the current tick.
   Safe to call from button callbacks or any other context.
   The window is fully destroyed by the event loop on the next frame. */
CA_API void       ca_window_close(Ca_Window *window);

/* Returns true if the window handle is valid and still open. */
CA_API bool       ca_window_is_open(const Ca_Window *window);

/* UI scale factor — like browser zoom.
   1.0 = 100% (default), 1.5 = 150%, 2.0 = 200%, etc.
   Affects all widget sizes, paddings, gaps, and text rendering. */
CA_API void       ca_window_set_scale(Ca_Window *window, float scale);
CA_API float      ca_window_get_scale(Ca_Window *window);

/* Set the window title displayed in the custom title bar. */
CA_API void ca_window_set_title(Ca_Window *window, const char *title);

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

typedef void (*Ca_EventFn)(const Ca_Event *event, void *user_data);

CA_API void ca_event_set_handler(Ca_Instance *instance, Ca_EventType type,
                                 Ca_EventFn fn, void *user_data);

/* ============================================================
   THREADS
   ============================================================ */

typedef void *(*Ca_ThreadFn)(void *user_data);

CA_API Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data);
CA_API void       ca_thread_join(Ca_Thread *thread);   /* blocks, then frees handle */
CA_API void       ca_thread_detach(Ca_Thread *thread); /* fire-and-forget, frees handle */

CA_API Ca_Mutex  *ca_mutex_create(void);
CA_API void       ca_mutex_destroy(Ca_Mutex *mutex);
CA_API void       ca_mutex_lock(Ca_Mutex *mutex);
CA_API void       ca_mutex_unlock(Ca_Mutex *mutex);
CA_API bool       ca_mutex_trylock(Ca_Mutex *mutex);   /* returns true if lock acquired */

CA_API Ca_CondVar *ca_condvar_create(void);
CA_API void        ca_condvar_destroy(Ca_CondVar *cv);
CA_API void        ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex);
CA_API void        ca_condvar_signal(Ca_CondVar *cv);
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

typedef void (*Ca_ClickFn)(Ca_Button *button, void *user_data);
typedef void (*Ca_ChangeFn)(Ca_TextInput *input, void *user_data);

/* Drag interaction callback.
   'dx' and 'dy' are the delta from the drag start point. */
typedef struct Ca_DragEvent {
    Ca_Window *window;
    float      x, y;           /* current mouse position */
    float      start_x, start_y; /* where mouse was when drag began */
    float      dx, dy;         /* x - start_x, y - start_y */
} Ca_DragEvent;

typedef void (*Ca_DragFn)(const Ca_DragEvent *event, void *user_data);

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
    float    width, height;        /* 0 = fill available space              */
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
    /* Visibility / interactivity */
    bool     hidden;               /* display: none — removed from layout   */
    bool     disabled;             /* non-interactive, visually dimmed       */
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

/* <button> — clickable element, can also nest children.
   When used with ca_btn (self-closing), built-in text is rendered.
   When used with ca_btn_begin / ca_btn_end, nest children inside. */
typedef struct Ca_BtnDesc {
    const char *text;              /* built-in label (for self-closing ca_btn) */
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

/* <input> — single-line text input field. */
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
    const char *id;                /* CSS id  (without #)                    */
    const char *style;             /* space-separated CSS class names        */
    bool        hidden;            /* display: none — removed from layout    */
    bool        disabled;          /* non-interactive, visually dimmed        */
} Ca_InputDesc;

/* ---- Tree root ---- */

CA_API void ca_ui_begin(Ca_Window *window, const Ca_DivDesc *root_desc);
CA_API void ca_ui_end(void);

/// Removes all children from a div and enters it as the current parent.
/// New widgets created after this call become children of the cleared div.
/// Must be paired with ca_div_end().
CA_API void ca_div_clear(Ca_Div *div);

/// Enters a div in keyed reconciliation mode.
/// Children created while active are matched/reused by key (id) when possible,
/// and any old unmatched children are removed on the matching ca_div_end().
CA_API void ca_reconcile_begin(Ca_Div *div);

/// Sets a one-shot reconciliation key for the next created element.
/// If set, this key overrides descriptor id for matching in reconcile mode.
CA_API void ca_reconcile_key(const char *key);

/* ---- Container elements (push / pop the parent stack) ---- */

CA_API Ca_Div *ca_div_begin(const Ca_DivDesc *desc);
CA_API void    ca_div_end(void);

CA_API Ca_Button *ca_btn_begin(const Ca_BtnDesc *desc); /* nestable button      */
CA_API void       ca_btn_end(void);

CA_API void ca_list_begin(const Ca_DivDesc *desc);      /* list (vertical, gap 4) */
CA_API void ca_list_end(void);

CA_API void ca_li_begin(const Ca_DivDesc *desc);        /* list item (horiz, gap 8) */
CA_API void ca_li_end(void);

/* ---- Self-closing elements ---- */

CA_API Ca_Label     *ca_text(const Ca_TextDesc *desc);
CA_API Ca_Button    *ca_btn(const Ca_BtnDesc *desc);       /* self-closing button  */
CA_API Ca_TextInput *ca_input(const Ca_InputDesc *desc);    /* text input field     */

CA_API void ca_hr(const Ca_HrDesc *desc);               /* horizontal rule      */
CA_API void ca_spacer(const Ca_SpacerDesc *desc);       /* invisible space      */

/* ---- Headings (convenience — text with default heights) ---- */

CA_API Ca_Label *ca_h1(const Ca_TextDesc *desc);        /* 24px */
CA_API Ca_Label *ca_h2(const Ca_TextDesc *desc);        /* 20px */
CA_API Ca_Label *ca_h3(const Ca_TextDesc *desc);        /* 18px */
CA_API Ca_Label *ca_h4(const Ca_TextDesc *desc);        /* 16px */
CA_API Ca_Label *ca_h5(const Ca_TextDesc *desc);        /* 14px */
CA_API Ca_Label *ca_h6(const Ca_TextDesc *desc);        /* 12px */

/* ---- Scroll container queries (by CSS id) ---- */

/// Scrolls a scroll container to the top of its content.
CA_API void ca_scroll_to_top(Ca_Window *window, const char *id);

/// Scrolls a scroll container to the bottom of its content.
CA_API void ca_scroll_to_bottom(Ca_Window *window, const char *id);

/* ---- Window callbacks ---- */

/// Registers a per-frame callback invoked after input processing, before paint.
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
    const char *id, *style;
} Ca_SplitDesc;

CA_API Ca_Splitter *ca_split_begin(const Ca_SplitDesc *desc);
CA_API void         ca_split_end(void);
CA_API float        ca_split_get_ratio(const Ca_Splitter *s);
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

   Images are displayed as textured quads using the text pipeline.
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

   Load a CSS stylesheet from a string and attach it to an instance.
   All windows belonging to that instance will use the stylesheet
   for style resolution.

   Supported selectors:
     - Type:       div, button, text, h1-h6, hr, spacer, list, li
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
     ca_btn(&(Ca_BtnDesc){ .text = "OK", .style = "btn-primary" });
   ============================================================ */

typedef struct Ca_Stylesheet Ca_Stylesheet;

CA_API Ca_Stylesheet *ca_css_parse(const char *css_text);
CA_API void           ca_css_destroy(Ca_Stylesheet *ss);

/* Attach a parsed stylesheet to the instance.  Ownership is NOT transferred;
   the caller must keep the stylesheet alive and destroy it after the instance. */
CA_API void ca_instance_set_stylesheet(Ca_Instance *instance, Ca_Stylesheet *ss);

/* ============================================================
   GPU — Vulkan resource accessors
   ============================================================

   These expose the Vulkan objects owned by causality so that an
   external renderer (e.g. a game engine) can share the same GPU
   context.  The returned handles are owned by causality — do NOT
   destroy them.
   ============================================================ */

/// Returns the VkInstance created by causality.
CA_API VkInstance          ca_gpu_instance(Ca_Instance *instance);

/// Returns the VkPhysicalDevice selected during init.
CA_API VkPhysicalDevice    ca_gpu_physical_device(Ca_Instance *instance);

/// Returns the VkDevice (logical device).
CA_API VkDevice            ca_gpu_device(Ca_Instance *instance);

/// Returns the graphics queue and its family index.
CA_API VkQueue             ca_gpu_graphics_queue(Ca_Instance *instance, uint32_t *family_index);

/// Returns the presentation queue and its family index.
CA_API VkQueue             ca_gpu_present_queue(Ca_Instance *instance, uint32_t *family_index);

/// Returns the shared command pool (graphics family, resettable buffers).
CA_API VkCommandPool       ca_gpu_command_pool(Ca_Instance *instance);

/// Finds a memory type index matching the given type bits and property flags.
/// Returns UINT32_MAX on failure.
CA_API uint32_t            ca_gpu_find_memory_type(Ca_Instance *instance,
                                                   uint32_t type_bits,
                                                   VkMemoryPropertyFlags properties);

/// Allocates and begins a one-shot command buffer for immediate GPU work.
CA_API VkCommandBuffer     ca_gpu_begin_transfer(Ca_Instance *instance);

/// Ends, submits, waits, and frees a one-shot command buffer.
CA_API void                ca_gpu_end_transfer(Ca_Instance *instance, VkCommandBuffer cmd);

/// Compiles a GLSL source string to a VkShaderModule via shaderc.
/// Returns VK_NULL_HANDLE on failure.
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

/// Called each frame to let the engine render into the viewport.
typedef void (*Ca_ViewportRenderFn)(Ca_Viewport *viewport, void *user_data);

/// Called when the viewport widget is resized by the layout system.
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

/// Creates a viewport widget in the current UI tree.
CA_API Ca_Viewport *ca_viewport(const Ca_ViewportDesc *desc);

/// Returns the command buffer to record into during on_render.
CA_API VkCommandBuffer ca_viewport_cmd(Ca_Viewport *viewport);

/// Returns the current pixel width of the viewport image.
CA_API uint32_t ca_viewport_width(const Ca_Viewport *viewport);

/// Returns the current pixel height of the viewport image.
CA_API uint32_t ca_viewport_height(const Ca_Viewport *viewport);

/// Returns the VkImage backing the viewport (for barrier/transition use).
CA_API VkImage ca_viewport_image(const Ca_Viewport *viewport);

/// Returns the VkImageView for the viewport's colour attachment.
CA_API VkImageView ca_viewport_image_view(const Ca_Viewport *viewport);

/// Returns the VkFormat of the viewport's colour attachment.
CA_API VkFormat ca_viewport_format(const Ca_Viewport *viewport);

/// Returns the owning Ca_Instance.
CA_API Ca_Instance *ca_viewport_instance(Ca_Viewport *viewport);

/// Marks the viewport as needing a redraw on the next frame.
CA_API void ca_viewport_request_redraw(Ca_Viewport *viewport);

/// Replaces the render and resize callbacks on an existing viewport.
CA_API void ca_viewport_set_callbacks(Ca_Viewport *viewport,
                                      Ca_ViewportRenderFn on_render, void *render_data,
                                      Ca_ViewportResizeFn on_resize, void *resize_data);

/* Auto-include the component layer for backward compatibility. */
#include "ca_components.h"

/* ============================================================
   UNIFIED RUNTIME SETTERS — ca_set_style / ca_set_hidden / ca_set_disabled

   These use C11 _Generic to accept ANY widget handle (Ca_Div*, Ca_Button*,
   Ca_Label*, Ca_Checkbox*, etc.) as the first argument.

       Ca_Button *btn = ca_btn(&(Ca_BtnDesc){...});
       ca_set_style(btn, "primary active");
       ca_set_hidden(btn, false);
       ca_set_disabled(btn, true);

       Ca_Div *panel = ca_div_begin(&(Ca_DivDesc){...});
       ca_set_style(panel, "sidebar collapsed");

   ============================================================ */

/* Backing functions (do not call directly — use the macros below) */
CA_API void ca__set_style_node(Ca_Div *div, const char *style);
CA_API void ca__set_style_widget(void *widget, const char *style);
CA_API void ca__set_hidden_node(Ca_Div *div, bool hidden);
CA_API void ca__set_hidden_widget(void *widget, bool hidden);
CA_API void ca__set_disabled_node(Ca_Div *div, bool disabled);
CA_API void ca__set_disabled_widget(void *widget, bool disabled);
CA_API void ca__set_text(void *widget, const char *text);
CA_API const char *ca__get_text(const void *widget);
CA_API void ca__set_color(void *widget, uint32_t color);
CA_API void ca__set_background_node(Ca_Div *div, uint32_t color);
CA_API void ca__set_background_widget(void *widget, uint32_t color);

#define ca_set_style(widget, style) \
    _Generic((widget),              \
        Ca_Div *: ca__set_style_node,  \
        default:  ca__set_style_widget \
    )((widget), (style))

#define ca_set_hidden(widget, hidden) \
    _Generic((widget),                \
        Ca_Div *: ca__set_hidden_node,  \
        default:  ca__set_hidden_widget \
    )((widget), (hidden))

#define ca_set_disabled(widget, disabled) \
    _Generic((widget),                    \
        Ca_Div *: ca__set_disabled_node,  \
        default:  ca__set_disabled_widget \
    )((widget), (disabled))

#define ca_set_text(widget, text)   ca__set_text((widget), (text))
#define ca_get_text(widget)         ca__get_text((widget))
#define ca_set_color(widget, color) ca__set_color((widget), (color))

#define ca_set_background(widget, color) \
    _Generic((widget),                   \
        Ca_Div *: ca__set_background_node,  \
        default:  ca__set_background_widget \
    )((widget), (color))

/* Title bar menu API — declared here because it requires Ca_MenuDesc
   which is defined in ca_components.h above. */

/* Deep-copy 'count' Ca_MenuDesc items into the title bar menu strip.
   Pass NULL / 0 to remove all menus. */
CA_API void ca_window_set_title_bar_menus(Ca_Window        *window,
                                          const Ca_MenuDesc *menus, int count);

/* ============================================================
   REACTIVE STATE
   ============================================================

   Ca_State is a typed, observable value that lives in the Causality
   instance.  When its value changes (via ca_state_set), subscribers
   are notified before the next frame's UI rebuild:

     - Node subscriptions  (ca_node_subscribe) mark nodes dirty so the
       paint cache is invalidated and the node repaints.
     - Function observers  (ca_state_observe) fire a callback so code
       can push new text, toggle visibility, rebuild sections, etc.

   Typical usage:

       // create (once, at init):
       Ca_State *sel = ca_state_create(inst, sizeof(Qs_Entity), &invalid);

       // observe (once, at init):
       ca_state_observe(sel, on_selection_changed, my_ctx);

       // mutate (at event time, not every frame):
       ca_state_set(sel, &new_entity);

   The observer fires exactly once per mutation, before the UI rebuild
   loop runs.  On idle frames where no mutation occurred, zero work is
   done.
   ============================================================ */

typedef struct Ca_State Ca_State;

/// Creates a new state with the given value size and optional initial value.
CA_API Ca_State *ca_state_create(Ca_Instance *inst, size_t data_size,
                                  const void *initial);

/// Destroys a state and releases its resources.
CA_API void ca_state_destroy(Ca_State *state);

/// Sets the state value.  Performs a memcmp — if the value is unchanged
/// no subscribers or observers are notified.
CA_API void ca_state_set(Ca_State *state, const void *value);

/// Reads the current state value into out (must be data_size bytes).
CA_API void ca_state_get(const Ca_State *state, void *out);

/// Returns a monotonically increasing generation counter.  Increments on
/// every successful (value-changing) ca_state_set call.
CA_API uint64_t ca_state_generation(const Ca_State *state);

/// Registers a function observer.  The callback fires before each frame's
/// UI rebuild, exactly once per value-changing ca_state_set call.
/// Up to 8 observers per state.
CA_API void ca_state_observe(Ca_State *state,
                              void (*fn)(const void *value, void *user),
                              void *user);

#ifdef __cplusplus
}
#endif

