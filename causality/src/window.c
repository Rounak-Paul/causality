#include "window.h"
#include "event.h"

/* ---- GLFW callbacks ---- */

static void glfw_key_cb(GLFWwindow *glfw, int key, int scancode, int action, int mods)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    Ca_Event ev;
    ev.type         = CA_EVENT_KEY;
    ev.window       = win;
    ev.key.key      = key;
    ev.key.scancode = scancode;
    ev.key.action   = action;
    ev.key.mods     = mods;
    ca_event_post(win->instance, &ev);
}

static void glfw_mouse_button_cb(GLFWwindow *glfw, int button, int action, int mods)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    Ca_Event ev;
    ev.type                = CA_EVENT_MOUSE_BUTTON;
    ev.window              = win;
    ev.mouse_button.button = button;
    ev.mouse_button.action = action;
    ev.mouse_button.mods   = mods;
    ca_event_post(win->instance, &ev);
}

static void glfw_cursor_pos_cb(GLFWwindow *glfw, double x, double y)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    Ca_Event ev;
    ev.type        = CA_EVENT_MOUSE_MOVE;
    ev.window      = win;
    ev.mouse_pos.x = x;
    ev.mouse_pos.y = y;
    ca_event_post(win->instance, &ev);
}

static void glfw_scroll_cb(GLFWwindow *glfw, double dx, double dy)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    Ca_Event ev;
    ev.type            = CA_EVENT_MOUSE_SCROLL;
    ev.window          = win;
    ev.mouse_scroll.dx = dx;
    ev.mouse_scroll.dy = dy;
    ca_event_post(win->instance, &ev);
}

static void glfw_window_size_cb(GLFWwindow *glfw, int width, int height)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    Ca_Event ev;
    ev.type          = CA_EVENT_WINDOW_RESIZE;
    ev.window        = win;
    ev.resize.width  = width;
    ev.resize.height = height;
    ca_event_post(win->instance, &ev);
}

/* ---- System ---- */

bool ca_window_system_init(void)
{
    if (!glfwInit()) {
        fprintf(stderr, "[causality] glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    return true;
}

void ca_window_system_shutdown(Ca_Instance *inst)
{
    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (inst->windows[i].in_use) {
            glfwDestroyWindow(inst->windows[i].glfw);
            inst->windows[i].glfw     = NULL;
            inst->windows[i].instance = NULL;
            inst->windows[i].in_use   = false;
        }
    }
    glfwTerminate();
}

bool ca_window_system_tick(Ca_Instance *inst)
{
    glfwPollEvents();

    /* Dispatch all queued input / resize events */
    ca_event_dispatch(inst);

    /* Fire WINDOW_CLOSE event then destroy — order matters */
    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (inst->windows[i].in_use && glfwWindowShouldClose(inst->windows[i].glfw)) {
            Ca_Event ev;
            ev.type   = CA_EVENT_WINDOW_CLOSE;
            ev.window = &inst->windows[i];
            const Ca_EventHandler *h = &inst->handlers[CA_EVENT_WINDOW_CLOSE];
            if (h->fn) h->fn(&ev, h->user_data);
            ca_window_destroy(&inst->windows[i]);
        }
    }

    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (inst->windows[i].in_use) return true;
    }
    return false;
}

/* ---- Per-window ---- */

Ca_Window *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc)
{
    assert(inst && desc);

    Ca_Window *slot = NULL;
    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (!inst->windows[i].in_use) {
            slot = &inst->windows[i];
            break;
        }
    }

    if (!slot) {
        fprintf(stderr, "[causality] window pool exhausted (max %d)\n", CA_MAX_WINDOWS);
        return NULL;
    }

    GLFWwindow *glfw = glfwCreateWindow(
        desc->width  > 0 ? desc->width  : 1280,
        desc->height > 0 ? desc->height : 720,
        desc->title  ? desc->title : "causality",
        NULL, NULL
    );

    if (!glfw) {
        fprintf(stderr, "[causality] glfwCreateWindow failed\n");
        return NULL;
    }

    slot->glfw     = glfw;
    slot->instance = inst;
    slot->in_use   = true;

    glfwSetWindowUserPointer(glfw, slot);
    glfwSetKeyCallback(glfw, glfw_key_cb);
    glfwSetMouseButtonCallback(glfw, glfw_mouse_button_cb);
    glfwSetCursorPosCallback(glfw, glfw_cursor_pos_cb);
    glfwSetScrollCallback(glfw, glfw_scroll_cb);
    glfwSetWindowSizeCallback(glfw, glfw_window_size_cb);

    return slot;
}

void ca_window_destroy(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    glfwDestroyWindow(window->glfw);
    window->glfw     = NULL;
    window->instance = NULL;
    window->in_use   = false;
}

GLFWwindow *ca_window_glfw(const Ca_Window *window)
{
    if (!window || !window->in_use) return NULL;
    return window->glfw;
}

