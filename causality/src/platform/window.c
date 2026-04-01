#include "window.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"

/* ---- GLFW callbacks ---- */

static void glfw_key_cb(GLFWwindow *glfw, int key, int scancode, int action, int mods)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    /* F9 toggles debug overlay */
    if (key == GLFW_KEY_F9 && action == GLFW_PRESS) {
        win->debug_overlay = !win->debug_overlay;
        win->dbg_force_repaint = true;
        return;
    }
    /* Buffer key presses for focus/input handling */
    if ((action == GLFW_PRESS || action == GLFW_REPEAT) &&
        win->key_count < CA_CHAR_BUF_MAX) {
        win->key_buf[win->key_count]        = key;
        win->key_action_buf[win->key_count] = action;
        win->key_mods_buf[win->key_count]   = mods;
        win->key_count++;
    }
    Ca_Event ev;
    ev.type         = CA_EVENT_KEY;
    ev.window       = win;
    ev.key.key      = key;
    ev.key.scancode = scancode;
    ev.key.action   = action;
    ev.key.mods     = mods;
    ca_event_post(win->instance, &ev);
}

static void glfw_char_cb(GLFWwindow *glfw, unsigned int codepoint)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (win->char_count < CA_CHAR_BUF_MAX)
        win->char_buf[win->char_count++] = codepoint;
    Ca_Event ev;
    ev.type              = CA_EVENT_CHAR;
    ev.window            = win;
    ev.character.codepoint = codepoint;
    ca_event_post(win->instance, &ev);
}

static void glfw_mouse_button_cb(GLFWwindow *glfw, int button, int action, int mods)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (button >= 0 && button < 3)
        win->mouse_buttons[button] = (action == GLFW_PRESS);
    if (button == 0 && action == GLFW_PRESS)
        win->mouse_click_this_frame = true;
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
    win->mouse_x = x;
    win->mouse_y = y;
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
    win->scroll_dx += dx;
    win->scroll_dy += dy;
    win->scroll_this_frame = true;
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
    /* Immediately rebuild swapchain so it's ready for the next frame */
    ca_renderer_window_resize(win->instance, win, width, height);
    /* Mark the root layout-dirty so ui_update re-flows and repaints */
    if (win->root)
        win->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

/* ---- System ---- */

static int g_glfw_refcount = 0;

bool ca_window_system_init(void)
{
    if (g_glfw_refcount > 0) {
        g_glfw_refcount++;
        return true;
    }
    if (!glfwInit()) {
        fprintf(stderr, "[causality] glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g_glfw_refcount = 1;
    return true;
}

void ca_window_system_shutdown(Ca_Instance *inst)
{
    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (inst->windows[i].in_use)
            ca_window_destroy(&inst->windows[i]);
    }
    if (g_glfw_refcount > 0)
        --g_glfw_refcount;
    /* glfwTerminate() is intentionally omitted.  Calling
       glfwTerminate → glfwInit in rapid succession races with
       MoltenVK / Vulkan-loader background threads on macOS,
       producing "mutex lock failed: Invalid argument" crashes.
       The OS reclaims all GLFW resources at process exit. */
}

bool ca_window_system_tick(Ca_Instance *inst)
{
    /* Clear per-frame click flags before GLFW fires callbacks */
    for (int i = 0; i < CA_MAX_WINDOWS; ++i)
        if (inst->windows[i].in_use) {
            inst->windows[i].mouse_click_this_frame = false;
            inst->windows[i].scroll_dx = 0;
            inst->windows[i].scroll_dy = 0;
            inst->windows[i].scroll_this_frame = false;
            inst->windows[i].char_count = 0;
            inst->windows[i].key_count  = 0;
        }

    if (inst->continuous)
        glfwPollEvents();
    else
        glfwWaitEvents();

    /* Dispatch all queued input / resize events */
    ca_event_dispatch(inst);

    /* Fire WINDOW_CLOSE event then destroy — order matters.
       Track whether we destroyed anything so we can re-focus. */
    bool destroyed_any = false;
    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (inst->windows[i].in_use && glfwWindowShouldClose(inst->windows[i].glfw)) {
            Ca_Event ev;
            ev.type   = CA_EVENT_WINDOW_CLOSE;
            ev.window = &inst->windows[i];
            const Ca_EventHandler *h = &inst->handlers[CA_EVENT_WINDOW_CLOSE];
            if (h->fn) h->fn(&ev, h->user_data);
            ca_window_destroy(&inst->windows[i]);
            destroyed_any = true;
        }
    }

    /* After any window closes, explicitly focus the first remaining window.
       Without this, macOS moves focus to the desktop and the next click on
       the window content is consumed by the OS to re-focus the window rather
       than being delivered to the app as a button event. */
    if (destroyed_any) {
        for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
            if (inst->windows[i].in_use && inst->windows[i].glfw) {
                glfwFocusWindow(inst->windows[i].glfw);
                break;
            }
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

    /* Zero the entire slot so no stale state survives from a previous window
       that occupied this slot (dangling pointers, input flags, etc.). */
    memset(slot, 0, sizeof(*slot));

    /* Copy the window title before creating the GLFW window */
    snprintf(slot->title, sizeof(slot->title), "%s",
             desc->title ? desc->title : "");

    /* Custom title bar: always create undecorated GLFW windows */
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

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
    slot->ui_scale = 1.0f;

    glfwSetWindowUserPointer(glfw, slot);
    glfwSetKeyCallback(glfw, glfw_key_cb);
    glfwSetCharCallback(glfw, glfw_char_cb);
    glfwSetMouseButtonCallback(glfw, glfw_mouse_button_cb);
    glfwSetCursorPosCallback(glfw, glfw_cursor_pos_cb);
    glfwSetScrollCallback(glfw, glfw_scroll_cb);
    glfwSetWindowSizeCallback(glfw, glfw_window_size_cb);

    /* Boot surface + swapchain (renderer must already be initialised) */
    if (inst->vk_device != VK_NULL_HANDLE) {
        if (!ca_renderer_window_init(inst, slot)) {
            glfwDestroyWindow(glfw);
            slot->glfw   = NULL;
            slot->in_use = false;
            return NULL;
        }
    }

    ca_ui_window_init(slot);

    /* Explicitly focus the new window.
       On macOS, glfwCreateWindow shows the window but does not guarantee
       keyboard/mouse focus.  Without this the first click on the window
       is consumed by the OS to bring it into focus rather than being
       delivered to the app as a button event. */
    glfwFocusWindow(glfw);

    return slot;
}

void ca_window_destroy(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    ca_ui_window_shutdown(window);
    if (window->instance && window->instance->vk_device != VK_NULL_HANDLE)
        ca_renderer_window_shutdown(window->instance, window);
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

void ca_window_close(Ca_Window *window)
{
    if (!window || !window->in_use || !window->glfw) return;
    glfwSetWindowShouldClose(window->glfw, GLFW_TRUE);
}

Ca_Instance *ca_window_instance(Ca_Window *window)
{
    return (window && window->in_use) ? window->instance : NULL;
}

bool ca_window_is_open(const Ca_Window *window)
{
    return window && window->in_use;
}

void ca_window_set_scale(Ca_Window *window, float scale)
{
    if (!window || !window->in_use) return;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 4.0f)  scale = 4.0f;
    window->ui_scale = scale;
    if (window->root)
        window->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

float ca_window_get_scale(Ca_Window *window)
{
    if (!window || !window->in_use) return 1.0f;
    return window->ui_scale;
}

