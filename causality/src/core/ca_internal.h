/* ca_internal.h — internal struct definitions, never exposed publicly */
#pragma once

#include "causality.h"
#include <pthread.h>
#include <stdint.h>

/* ======================================================
   RENDERER
   ====================================================== */

#define CA_FRAMES_IN_FLIGHT     2
#define CA_MAX_SWAPCHAIN_IMAGES 8

typedef struct {
    VkCommandBuffer cmd;
    VkSemaphore     image_available;
    VkSemaphore     render_finished;
    VkFence         in_flight;
} Ca_Frame;

typedef struct {
    VkSwapchainKHR  swapchain;
    VkFormat        format;
    VkExtent2D      extent;
    uint32_t        image_count;
    VkImage         images[CA_MAX_SWAPCHAIN_IMAGES];
    VkImageView     image_views[CA_MAX_SWAPCHAIN_IMAGES];
    Ca_Frame        frames[CA_FRAMES_IN_FLIGHT];
    uint32_t        current_frame;
} Ca_Swapchain;

/* ======================================================
   RENDERER — rect pipeline types
   ====================================================== */

typedef struct {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
} Ca_RectPipeline;

/* ======================================================
   RENDERER — text pipeline types
   ====================================================== */

typedef struct {
    VkPipeline            pipeline;
    VkPipelineLayout      layout;
    VkDescriptorSetLayout desc_layout;
    VkDescriptorPool      desc_pool;
    VkDescriptorSet       desc_set;
} Ca_TextPipeline;

/* Must exactly match the push_constant block in the text vertex shader:
     vec2 pos      (offset  0)
     vec2 size     (offset  8)
     vec4 uv       (offset 16)  -- (s0, t0, s1, t1)
     vec4 color    (offset 32)
     vec2 viewport (offset 48)
   Total: 56 bytes.                                               */
typedef struct {
    float pos[2];
    float size[2];
    float uv[4];
    float color[4];
    float viewport[2];
} Ca_TextPushConst;

/* Forward-declare Ca_Font (full definition lives in renderer/font.h) */
typedef struct Ca_Font Ca_Font;

/* Must exactly match the push_constant block in the vertex shader:
     vec2 pos      (offset  0)
     vec2 size     (offset  8)
     vec4 color    (offset 16)
     vec2 viewport (offset 32)
   Total: 40 bytes.                                               */
typedef struct {
    float pos[2];
    float size[2];
    float color[4];
    float viewport[2];
} Ca_RectPushConst;

/* ======================================================
   EVENTS
   ====================================================== */

#define CA_EVENT_QUEUE_CAPACITY 256

typedef struct {
    Ca_EventFn  fn;
    void       *user_data;
} Ca_EventHandler;

/* ======================================================
   UI — forward declarations (State and Node reference each other)
   ====================================================== */

typedef struct Ca_State Ca_State;
typedef struct Ca_Node  Ca_Node;

/* ======================================================
   UI — dirty flags, layout enums, node descriptor, state descriptor
   (These are internal; users work with widgets instead.)
   ====================================================== */

typedef enum {
    CA_DIRTY_NONE     = 0,
    CA_DIRTY_CONTENT  = 1 << 0,
    CA_DIRTY_LAYOUT   = 1 << 1,
    CA_DIRTY_CHILDREN = 1 << 2,
} Ca_DirtyFlags;

typedef enum {
    CA_DIR_ROW    = 0,
    CA_DIR_COLUMN = 1,
} Ca_Direction;

typedef enum {
    CA_ALIGN_START   = 0,
    CA_ALIGN_CENTER  = 1,
    CA_ALIGN_END     = 2,
    CA_ALIGN_STRETCH = 3,
} Ca_Align;

typedef struct {
    float        width,  height;
    float        min_w,  min_h;
    float        max_w,  max_h;
    float        padding_top,    padding_right,
                 padding_bottom, padding_left;
    float        gap;
    Ca_Direction direction;
    Ca_Align     align_items;
    Ca_Align     justify_content;
    uint32_t     background;
    float        corner_radius;
} Ca_NodeDesc;

/* Internal state descriptor — used by ca_state_create inside the library. */
typedef struct {
    uint32_t    data_size;
    const void *initial;
} Ca_StateDesc;

/* ======================================================
   UI — constants
   ====================================================== */

#define CA_MAX_STATES               512
#define CA_STATE_DATA_MAX            64
#define CA_MAX_NODES_PER_WINDOW     512
#define CA_MAX_NODE_CHILDREN         32
#define CA_MAX_NODE_SUBS              8
#define CA_MAX_STATE_SUBSCRIBERS     64
#define CA_MAX_DRAW_CMDS_PER_WINDOW 512

#define CA_MAX_LABELS_PER_WINDOW    256
#define CA_MAX_BUTTONS_PER_WINDOW   128
#define CA_LABEL_TEXT_MAX           256
#define CA_BUTTON_TEXT_MAX          128

/* ======================================================
   UI — draw command (CPU-side, one per visible node or glyph)
   ====================================================== */

typedef enum {
    CA_DRAW_RECT  = 0,  /* solid colour rectangle               */
    CA_DRAW_GLYPH = 1,  /* font glyph textured quad             */
} Ca_DrawType;

typedef struct {
    Ca_DrawType type;
    float       x, y, w, h;
    float       r, g, b, a;
    float       corner_radius;
    /* CA_DRAW_GLYPH: normalised UV coords in the font atlas */
    float       u0, v0, u1, v1;
    bool        in_use;
} Ca_DrawCmd;

/* ======================================================
   UI — node type
   ====================================================== */

typedef enum {
    CA_NODE_BOX = 0,
} Ca_NodeType;

/* ======================================================
   UI — State (full definition)
   ====================================================== */

struct Ca_State {
    Ca_Instance  *instance;
    uint64_t      generation;
    uint16_t      data_size;
    bool          dirty;
    bool          in_use;
    uint8_t       data[CA_STATE_DATA_MAX];
    Ca_Node      *subscribers[CA_MAX_STATE_SUBSCRIBERS];
    uint8_t       sub_flags[CA_MAX_STATE_SUBSCRIBERS];
    uint32_t      sub_count;
};

/* ======================================================
   UI — Node (full definition)
   ====================================================== */

struct Ca_Node {
    Ca_NodeType   type;
    uint8_t       dirty;           /* Ca_DirtyFlags bits              */
    bool          in_use;
    Ca_NodeDesc   desc;
    float         x, y, w, h;     /* computed by layout pass         */
    Ca_Window    *window;
    Ca_Node      *parent;
    Ca_Node      *children[CA_MAX_NODE_CHILDREN];
    uint32_t      child_count;
    Ca_State     *subs[CA_MAX_NODE_SUBS];
    uint8_t       sub_flags[CA_MAX_NODE_SUBS];
    uint32_t      sub_count;
    int32_t       draw_cmd_idx;    /* -1 = no slot assigned           */
};

/* ======================================================
   UI — Widget structs (full definitions; opaque in public header)
   ====================================================== */

struct Ca_Label {
    Ca_Node  *node;
    char      text[CA_LABEL_TEXT_MAX];
    uint32_t  color;   /* packed RGBA foreground colour */
    bool      in_use;
};

struct Ca_Button {
    Ca_Node    *node;
    char        text[CA_BUTTON_TEXT_MAX];
    uint32_t    text_color;  /* packed RGBA; 0 → white default */
    Ca_ClickFn  on_click;
    void       *click_data;
    bool        in_use;
};

/* ======================================================
   WINDOW
   ====================================================== */

struct Ca_Window {
    GLFWwindow   *glfw;
    Ca_Instance  *instance;
    VkSurfaceKHR  surface;
    Ca_Swapchain  sc;
    bool          in_use;

    /* UI node tree */
    Ca_Node      *node_pool;
    Ca_Node      *root;
    Ca_DrawCmd   *draw_cmds;
    uint32_t      draw_cmd_count;

    /* Widget pools */
    Ca_Label     *label_pool;
    Ca_Button    *button_pool;

    /* UI scale factor (1.0 = default, 2.0 = 200%, like browser zoom) */
    float         ui_scale;

    /* Input state (updated by GLFW callbacks each tick) */
    double        mouse_x, mouse_y;
    bool          mouse_buttons[3];       /* [0]=left [1]=right [2]=middle */
    bool          mouse_click_this_frame; /* cleared at top of each tick   */

    /* Render gating: set by ui.c when draw list changes, cleared after submit */
    bool          needs_render;
};

/* ======================================================
   INSTANCE
   ====================================================== */

struct Ca_Instance {
    Ca_Window windows[CA_MAX_WINDOWS];

    /* Vulkan */
    VkInstance               vk_instance;
    VkPhysicalDevice         vk_gpu;
    VkDevice                 vk_device;
    VkQueue                  gfx_queue;
    VkQueue                  present_queue;
    uint32_t                 gfx_family;
    uint32_t                 present_family;
    VkCommandPool            cmd_pool;

    /* Event ring-buffer */
    Ca_Event         event_queue[CA_EVENT_QUEUE_CAPACITY];
    uint32_t         event_head;
    uint32_t         event_tail;
    pthread_mutex_t  event_mutex;
    Ca_EventHandler  handlers[CA_EVENT_TYPE_COUNT];

    /* Font config (copied from Ca_InstanceDesc, used on first window init) */
    char  font_path[512];
    float font_size_px;

    /* UI state pool */
    Ca_State        *state_pool;

    /* Rect drawing pipeline — created on first window init */
    Ca_RectPipeline  rect_pipeline;

    /* Text pipeline + font atlas — created on first window init */
    Ca_TextPipeline  text_pipeline;
    Ca_Font         *font;   /* NULL until successfully loaded */
};

/* ======================================================
   THREAD
   ====================================================== */

struct Ca_Thread {
    pthread_t handle;
};

