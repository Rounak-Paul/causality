#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

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
typedef struct Ca_Splitter  Ca_Splitter;
typedef struct Ca_Image     Ca_Image;

/* ============================================================
   INSTANCE
   ============================================================ */

typedef struct Ca_InstanceDesc {
    const char *app_name;
    bool        prefer_dedicated_gpu;
    /* Font — leave NULL to use the built-in Roboto Mono Nerd Font.
       Set font_path to override with a custom .ttf or .otf file. */
    const char *font_path;       /* path to a .ttf or .otf file, or NULL */
    float       font_size_px;    /* desired size in logical pixels (default: 14) */
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

/* UI scale factor — like browser zoom.
   1.0 = 100% (default), 1.5 = 150%, 2.0 = 200%, etc.
   Affects all widget sizes, paddings, gaps, and text rendering. */
void       ca_window_set_scale(Ca_Window *window, float scale);
float      ca_window_get_scale(Ca_Window *window);

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

void ca_event_set_handler(Ca_Instance *instance, Ca_EventType type,
                          Ca_EventFn fn, void *user_data);

/* ============================================================
   THREADS
   ============================================================ */

typedef void *(*Ca_ThreadFn)(void *user_data);

Ca_Thread *ca_thread_create(Ca_ThreadFn fn, void *user_data);
void       ca_thread_join(Ca_Thread *thread);   /* blocks, then frees handle */
void       ca_thread_detach(Ca_Thread *thread); /* fire-and-forget, frees handle */

Ca_Mutex  *ca_mutex_create(void);
void       ca_mutex_destroy(Ca_Mutex *mutex);
void       ca_mutex_lock(Ca_Mutex *mutex);
void       ca_mutex_unlock(Ca_Mutex *mutex);
bool       ca_mutex_trylock(Ca_Mutex *mutex);   /* returns true if lock acquired */

Ca_CondVar *ca_condvar_create(void);
void        ca_condvar_destroy(Ca_CondVar *cv);
void        ca_condvar_wait(Ca_CondVar *cv, Ca_Mutex *mutex);
void        ca_condvar_signal(Ca_CondVar *cv);
void        ca_condvar_broadcast(Ca_CondVar *cv);

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
typedef void (*Ca_CheckFn)(Ca_Checkbox *cb, void *user_data);
typedef void (*Ca_SliderFn)(Ca_Slider *slider, void *user_data);
typedef void (*Ca_ToggleFn)(Ca_Toggle *toggle, void *user_data);
typedef void (*Ca_SelectFn)(Ca_Select *sel, void *user_data);
typedef void (*Ca_TabFn)(Ca_TabBar *tabs, void *user_data);
typedef void (*Ca_TreeToggleFn)(Ca_TreeNode *tn, void *user_data);
typedef void (*Ca_MenuFn)(int item_index, void *user_data);

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
} Ca_DivDesc;

/* <p> / text — leaf text element. */
typedef struct Ca_TextDesc {
    const char *text;
    float       width, height;     /* 0 = auto                              */
    uint32_t    color;             /* text foreground colour                */
    bool        wrap;              /* true = multi-line text wrapping       */
    const char *id;                /* CSS id  (without #)                   */
    const char *style;             /* space-separated CSS class names       */
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
} Ca_InputDesc;

/* ---- Tree root ---- */

void ca_ui_begin(Ca_Window *window, const Ca_DivDesc *root_desc);
void ca_ui_end(void);

/* ---- Container elements (push / pop the parent stack) ---- */

void ca_div_begin(const Ca_DivDesc *desc);
void ca_div_end(void);

Ca_Button *ca_btn_begin(const Ca_BtnDesc *desc); /* nestable button      */
void       ca_btn_end(void);

void ca_list_begin(const Ca_DivDesc *desc);      /* list (vertical, gap 4) */
void ca_list_end(void);

void ca_li_begin(const Ca_DivDesc *desc);        /* list item (horiz, gap 8) */
void ca_li_end(void);

/* ---- Self-closing elements ---- */

Ca_Label     *ca_text(const Ca_TextDesc *desc);
Ca_Button    *ca_btn(const Ca_BtnDesc *desc);       /* self-closing button  */
Ca_TextInput *ca_input(const Ca_InputDesc *desc);    /* text input field     */

void ca_hr(const Ca_HrDesc *desc);               /* horizontal rule      */
void ca_spacer(const Ca_SpacerDesc *desc);       /* invisible space      */

/* ---- Headings (convenience — text with default heights) ---- */

Ca_Label *ca_h1(const Ca_TextDesc *desc);        /* 24px */
Ca_Label *ca_h2(const Ca_TextDesc *desc);        /* 20px */
Ca_Label *ca_h3(const Ca_TextDesc *desc);        /* 18px */
Ca_Label *ca_h4(const Ca_TextDesc *desc);        /* 16px */
Ca_Label *ca_h5(const Ca_TextDesc *desc);        /* 14px */
Ca_Label *ca_h6(const Ca_TextDesc *desc);        /* 12px */

/* ---- Runtime setters ---- */

void ca_label_set_text(Ca_Label *label, const char *text);
void ca_button_set_text(Ca_Button *button, const char *text);
void ca_button_set_background(Ca_Button *button, uint32_t color);
void ca_input_set_text(Ca_TextInput *input, const char *text);
const char *ca_input_get_text(const Ca_TextInput *input);

/* ============================================================
   UI — NEW WIDGET DESCRIPTORS
   ============================================================ */

typedef struct Ca_CheckboxDesc {
    const char *text;
    bool        checked;
    Ca_CheckFn  on_change;
    void       *change_data;
    const char *id, *style;
} Ca_CheckboxDesc;

typedef struct Ca_RadioDesc {
    const char *text;
    int         group;
    int         value;
    Ca_CheckFn  on_change;     /* reuses check callback signature */
    void       *change_data;
    const char *id, *style;
} Ca_RadioDesc;

typedef struct Ca_SliderDesc {
    float       min, max, value;
    float       width;
    Ca_SliderFn on_change;
    void       *change_data;
    const char *id, *style;
} Ca_SliderDesc;

typedef struct Ca_ToggleDesc {
    bool        on;
    Ca_ToggleFn on_change;
    void       *change_data;
    const char *id, *style;
} Ca_ToggleDesc;

typedef struct Ca_ProgressDesc {
    float       value;         /* 0.0 – 1.0 */
    float       width, height;
    uint32_t    bar_color;
    const char *id, *style;
} Ca_ProgressDesc;

typedef struct Ca_SelectDesc {
    const char **options;
    int          option_count;
    int          selected;
    float        width;
    Ca_SelectFn  on_change;
    void        *change_data;
    const char  *id, *style;
} Ca_SelectDesc;

typedef struct Ca_TabBarDesc {
    const char **labels;
    int          count;
    int          active;
    Ca_TabFn     on_change;
    void        *change_data;
    const char  *id, *style;
} Ca_TabBarDesc;

typedef struct Ca_TreeNodeDesc {
    const char      *text;
    bool             expanded;
    Ca_TreeToggleFn  on_toggle;
    void            *toggle_data;
    const char      *id, *style;
} Ca_TreeNodeDesc;

typedef struct Ca_TableDesc {
    int          column_count;
    const float *column_widths;
    const char  *id, *style;
} Ca_TableDesc;

typedef struct Ca_TooltipDesc {
    const char *text;
    const char *id, *style;
} Ca_TooltipDesc;

typedef struct Ca_CtxMenuDesc {
    const char **items;
    int          item_count;
    Ca_MenuFn    on_select;
    void        *select_data;
    const char  *id, *style;
} Ca_CtxMenuDesc;

typedef struct Ca_ModalDesc {
    bool        visible;
    uint32_t    overlay_color;
    const char *id, *style;
} Ca_ModalDesc;

/* ============================================================
   UI — NEW WIDGET API
   ============================================================ */

/* Checkbox */
Ca_Checkbox *ca_checkbox(const Ca_CheckboxDesc *desc);
void         ca_checkbox_set(Ca_Checkbox *cb, bool checked);
bool         ca_checkbox_get(const Ca_Checkbox *cb);

/* Radio button */
Ca_Radio    *ca_radio(const Ca_RadioDesc *desc);
int          ca_radio_group_get(Ca_Window *win, int group);

/* Slider */
Ca_Slider   *ca_slider(const Ca_SliderDesc *desc);
void         ca_slider_set(Ca_Slider *s, float value);
float        ca_slider_get(const Ca_Slider *s);

/* Toggle switch */
Ca_Toggle   *ca_toggle(const Ca_ToggleDesc *desc);
void         ca_toggle_set(Ca_Toggle *t, bool on);
bool         ca_toggle_get(const Ca_Toggle *t);

/* Progress bar */
Ca_Progress *ca_progress(const Ca_ProgressDesc *desc);
void         ca_progress_set(Ca_Progress *p, float value);

/* Select / dropdown */
Ca_Select   *ca_select(const Ca_SelectDesc *desc);
void         ca_select_set(Ca_Select *s, int index);
int          ca_select_get(const Ca_Select *s);

/* Tab bar */
Ca_TabBar   *ca_tabs(const Ca_TabBarDesc *desc);
int          ca_tabs_active(const Ca_TabBar *t);

/* Tree view */
void         ca_tree_begin(const Ca_DivDesc *desc);
void         ca_tree_end(void);
Ca_TreeNode *ca_tree_node_begin(const Ca_TreeNodeDesc *desc);
void         ca_tree_node_end(void);
bool         ca_tree_node_expanded(const Ca_TreeNode *n);

/* Table */
void ca_table_begin(const Ca_TableDesc *desc);
void ca_table_end(void);
void ca_table_row_begin(const Ca_DivDesc *desc);
void ca_table_row_end(void);
void ca_table_cell(const Ca_TextDesc *desc);

/* Tooltip — attach to the previously created element */
void ca_tooltip(const Ca_TooltipDesc *desc);

/* Context menu — attach to the previously created element */
void ca_context_menu(const Ca_CtxMenuDesc *desc);

/* Modal / dialog */
void ca_modal_begin(const Ca_ModalDesc *desc);
void ca_modal_end(void);

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

Ca_Splitter *ca_split_begin(const Ca_SplitDesc *desc);
void         ca_split_end(void);
float        ca_split_get_ratio(const Ca_Splitter *s);
void         ca_split_set_ratio(Ca_Splitter *s, float ratio);

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
Ca_Image *ca_image_create(Ca_Instance *instance,
                          const uint8_t *pixels, int width, int height);

/* Destroy an image and release its GPU resources. */
void ca_image_destroy(Ca_Instance *instance, Ca_Image *image);

/* Display an image as a UI element. */
void ca_image(const Ca_ImageDesc *desc);

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

Ca_Stylesheet *ca_css_parse(const char *css_text);
void           ca_css_destroy(Ca_Stylesheet *ss);

/* Attach a parsed stylesheet to the instance.  Ownership is NOT transferred;
   the caller must keep the stylesheet alive and destroy it after the instance. */
void ca_instance_set_stylesheet(Ca_Instance *instance, Ca_Stylesheet *ss);

/* ============================================================
   GPU — Vulkan resource accessors
   ============================================================

   These expose the Vulkan objects owned by causality so that an
   external renderer (e.g. a game engine) can share the same GPU
   context.  The returned handles are owned by causality — do NOT
   destroy them.
   ============================================================ */

/// Returns the VkInstance created by causality.
VkInstance          ca_gpu_instance(Ca_Instance *instance);

/// Returns the VkPhysicalDevice selected during init.
VkPhysicalDevice    ca_gpu_physical_device(Ca_Instance *instance);

/// Returns the VkDevice (logical device).
VkDevice            ca_gpu_device(Ca_Instance *instance);

/// Returns the graphics queue and its family index.
VkQueue             ca_gpu_graphics_queue(Ca_Instance *instance, uint32_t *family_index);

/// Returns the presentation queue and its family index.
VkQueue             ca_gpu_present_queue(Ca_Instance *instance, uint32_t *family_index);

/// Returns the shared command pool (graphics family, resettable buffers).
VkCommandPool       ca_gpu_command_pool(Ca_Instance *instance);

/// Finds a memory type index matching the given type bits and property flags.
/// Returns UINT32_MAX on failure.
uint32_t            ca_gpu_find_memory_type(Ca_Instance *instance,
                                            uint32_t type_bits,
                                            VkMemoryPropertyFlags properties);

/// Allocates and begins a one-shot command buffer for immediate GPU work.
VkCommandBuffer     ca_gpu_begin_transfer(Ca_Instance *instance);

/// Ends, submits, waits, and frees a one-shot command buffer.
void                ca_gpu_end_transfer(Ca_Instance *instance, VkCommandBuffer cmd);

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
Ca_Viewport *ca_viewport(const Ca_ViewportDesc *desc);

/// Returns the command buffer to record into during on_render.
VkCommandBuffer ca_viewport_cmd(Ca_Viewport *viewport);

/// Returns the current pixel width of the viewport image.
uint32_t ca_viewport_width(const Ca_Viewport *viewport);

/// Returns the current pixel height of the viewport image.
uint32_t ca_viewport_height(const Ca_Viewport *viewport);

/// Returns the VkImage backing the viewport (for barrier/transition use).
VkImage ca_viewport_image(const Ca_Viewport *viewport);

/// Returns the VkImageView for the viewport's colour attachment.
VkImageView ca_viewport_image_view(const Ca_Viewport *viewport);

/// Returns the VkFormat of the viewport's colour attachment.
VkFormat ca_viewport_format(const Ca_Viewport *viewport);

/// Returns the owning Ca_Instance.
Ca_Instance *ca_viewport_instance(Ca_Viewport *viewport);

#ifdef __cplusplus
}
#endif

