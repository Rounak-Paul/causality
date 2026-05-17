// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

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
    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i) {
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
    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i)
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
    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i) {
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
        for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i) {
            if (inst->windows[i].in_use && inst->windows[i].glfw) {
                glfwFocusWindow(inst->windows[i].glfw);
                break;
            }
        }
    }

    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i) {
        if (inst->windows[i].in_use) return true;
    }
    return false;
}

/* ---- Per-window ---- */

static Ca_Window *window_create_in_slot(Ca_Instance *inst, const Ca_WindowDesc *desc,
                                        int slot_index)
{
    if (!inst || !desc) return NULL;
    if (slot_index < 0 || slot_index >= CA_MAX_WINDOWS_TOTAL) return NULL;

    Ca_Window *slot = &inst->windows[slot_index];
    if (slot->in_use) return slot;

    assert(inst && desc);

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
    /* Apply the instance-wide default scale if one has been set,
       otherwise fall back to 1.0 (no scaling). */
    slot->ui_scale = (inst->default_ui_scale > 0.0f)
                     ? inst->default_ui_scale : 1.0f;

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

Ca_Window *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc)
{
    assert(inst && desc);

    for (int i = 0; i < CA_MAX_WINDOWS; ++i) {
        if (!inst->windows[i].in_use)
            return window_create_in_slot(inst, desc, i);
    }

    fprintf(stderr, "[causality] window pool exhausted (max %d app windows)\n", CA_MAX_WINDOWS);
    return NULL;
}

Ca_Window *ca_window_create_reserved(Ca_Instance *inst, const Ca_WindowDesc *desc,
                                     int reserved_index)
{
    if (!inst || !desc) return NULL;
    if (reserved_index < 0 || reserved_index >= CA_RESERVED_POPUP_WINDOWS)
        return NULL;
    return window_create_in_slot(inst, desc, CA_MAX_WINDOWS + reserved_index);
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

void ca_window_maximize(Ca_Window *window)
{
    if (!window || !window->in_use || !window->glfw) return;
    if (window->titlebar_maximized) return;

    /* Mirror the title bar's maximize branch exactly so restore works. */
    glfwGetWindowPos(window->glfw,
                     &window->titlebar_pre_max_x, &window->titlebar_pre_max_y);
    glfwGetWindowSize(window->glfw,
                      &window->titlebar_pre_max_w, &window->titlebar_pre_max_h);

    int cx = window->titlebar_pre_max_x + window->titlebar_pre_max_w / 2;
    int cy = window->titlebar_pre_max_y + window->titlebar_pre_max_h / 2;
    int mon_count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&mon_count);
    GLFWmonitor *target = glfwGetPrimaryMonitor();
    for (int i = 0; i < mon_count; i++) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) {
            target = monitors[i];
            break;
        }
    }

    int wx, wy, ww, wh;
    glfwGetMonitorWorkarea(target, &wx, &wy, &ww, &wh);
    glfwSetWindowPos(window->glfw, wx, wy);
    glfwSetWindowSize(window->glfw, ww, wh);
    window->titlebar_maximized     = true;
    window->titlebar_needs_rebuild = true;
}

Ca_Instance *ca_window_instance(Ca_Window *window)
{
    return (window && window->in_use) ? window->instance : NULL;
}

bool ca_window_is_open(const Ca_Window *window)
{
    return window && window->in_use;
}

/* ---- Edge / corner resize for undecorated windows ---- */

/* Bitmask for the 8 resize zones */
#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

#define RESIZE_BORDER 6  /* px hit zone on each edge */
#define RESIZE_MIN_W 200
#define RESIZE_MIN_H 120

static int resize_edge_for_pos(int win_w, int win_h, double cx, double cy)
{
    int edge = 0;
    if (cx < RESIZE_BORDER)           edge |= RESIZE_LEFT;
    if (cx > win_w - RESIZE_BORDER)   edge |= RESIZE_RIGHT;
    if (cy < RESIZE_BORDER)           edge |= RESIZE_TOP;
    if (cy > win_h - RESIZE_BORDER)   edge |= RESIZE_BOTTOM;
    return edge;
}

static GLFWcursor *s_cursors[3]; /* hresize, vresize, crossresize */
static bool        s_cursors_init = false;

static void ensure_cursors(void)
{
    if (s_cursors_init) return;
    s_cursors[0] = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
    s_cursors[1] = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
    s_cursors[2] = glfwCreateStandardCursor(GLFW_RESIZE_ALL_CURSOR);
    s_cursors_init = true;
}

void ca_window_resize_pass(Ca_Window *win)
{
    if (!win || !win->in_use || win->titlebar_maximized) return;

    ensure_cursors();

    bool left_down = win->mouse_buttons[0];
    double cx, cy;
    glfwGetCursorPos(win->glfw, &cx, &cy);
    int win_w, win_h;
    glfwGetWindowSize(win->glfw, &win_w, &win_h);

    /* --- Continue active resize --- */
    if (win->resize_active) {
        if (!left_down) {
            /* Button released — end resize */
            win->resize_active = false;
            glfwSetCursor(win->glfw, NULL);
            return;
        }

        int wx, wy;
        glfwGetWindowPos(win->glfw, &wx, &wy);
        double sx = (double)wx + cx;
        double sy = (double)wy + cy;
        double ddx = sx - win->resize_start_cursor_sx;
        double ddy = sy - win->resize_start_cursor_sy;

        int new_x = win->resize_start_win_x;
        int new_y = win->resize_start_win_y;
        int new_w = win->resize_start_win_w;
        int new_h = win->resize_start_win_h;

        if (win->resize_edge & RESIZE_RIGHT)  new_w = (int)(win->resize_start_win_w + ddx);
        if (win->resize_edge & RESIZE_BOTTOM) new_h = (int)(win->resize_start_win_h + ddy);
        if (win->resize_edge & RESIZE_LEFT) {
            new_w = (int)(win->resize_start_win_w - ddx);
            new_x = (int)(win->resize_start_win_x + ddx);
        }
        if (win->resize_edge & RESIZE_TOP) {
            new_h = (int)(win->resize_start_win_h - ddy);
            new_y = (int)(win->resize_start_win_y + ddy);
        }

        if (new_w < RESIZE_MIN_W) {
            if (win->resize_edge & RESIZE_LEFT)
                new_x = win->resize_start_win_x + win->resize_start_win_w - RESIZE_MIN_W;
            new_w = RESIZE_MIN_W;
        }
        if (new_h < RESIZE_MIN_H) {
            if (win->resize_edge & RESIZE_TOP)
                new_y = win->resize_start_win_y + win->resize_start_win_h - RESIZE_MIN_H;
            new_h = RESIZE_MIN_H;
        }

        glfwSetWindowPos(win->glfw, new_x, new_y);
        glfwSetWindowSize(win->glfw, new_w, new_h);
        return;
    }

    /* --- Not resizing — hit-test edges to show cursor feedback --- */
    int edge = resize_edge_for_pos(win_w, win_h, cx, cy);

    if (edge == 0) {
        glfwSetCursor(win->glfw, NULL);
        return;
    }

    /* Choose cursor shape */
    bool horiz = (edge == RESIZE_LEFT  || edge == RESIZE_RIGHT);
    bool vert  = (edge == RESIZE_TOP   || edge == RESIZE_BOTTOM);
    if (horiz && !vert)       glfwSetCursor(win->glfw, s_cursors[0]);
    else if (vert && !horiz)  glfwSetCursor(win->glfw, s_cursors[1]);
    else                      glfwSetCursor(win->glfw, s_cursors[2]);

    /* Start resize on mouse press */
    if (left_down && win->mouse_click_this_frame) {
        int wx, wy;
        glfwGetWindowPos(win->glfw, &wx, &wy);
        win->resize_active          = true;
        win->resize_edge            = edge;
        win->resize_start_win_x     = wx;
        win->resize_start_win_y     = wy;
        win->resize_start_win_w     = win_w;
        win->resize_start_win_h     = win_h;
        win->resize_start_cursor_sx = (double)wx + cx;
        win->resize_start_cursor_sy = (double)wy + cy;
    }
}

void ca_window_set_scale(Ca_Window *window, float scale)
{
    if (!window || !window->in_use) return;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 4.0f)  scale = 4.0f;
    window->ui_scale = scale;
    window->titlebar_needs_rebuild = true;
    if (window->root)
        window->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

float ca_window_get_scale(Ca_Window *window)
{
    if (!window || !window->in_use) return 1.0f;
    return window->ui_scale;
}

void ca_clipboard_set_text(Ca_Window *window, const char *text)
{
    if (!window || !window->glfw) return;
    glfwSetClipboardString(window->glfw, text ? text : "");
}

const char *ca_clipboard_get_text(Ca_Window *window)
{
    if (!window || !window->glfw) return NULL;
    return glfwGetClipboardString(window->glfw);
}

