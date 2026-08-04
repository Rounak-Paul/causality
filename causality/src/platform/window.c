// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#endif

#include "window.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"
#include "app_menu.h"

#include <limits.h>
#include <stdlib.h>

#if defined(__linux__)
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef __APPLE__
/* Defined in mouse_state_mac.m — query live OS-level mouse state and drive the
   resize-preview outline overlay. */
extern bool ca_mac_left_button_held(void);
extern void ca_mac_cursor_screen_pos(double *out_x, double *out_y);
extern void ca_mac_resize_preview_show(int x, int y, int w, int h);
extern void ca_mac_resize_preview_hide(void);
#endif

#if !defined(__APPLE__)
#if defined(_WIN32)
static HWND s_resize_preview_edges[4] = {0};
static HBRUSH s_resize_preview_brush = NULL;

static LRESULT CALLBACK resize_preview_wndproc(HWND hwnd, UINT msg,
                                               WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool resize_preview_ensure(void)
{
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = resize_preview_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"CausalityResizePreview";
        s_resize_preview_brush = CreateSolidBrush(RGB(78, 140, 255));
        wc.hbrBackground = s_resize_preview_brush;
        if (!RegisterClassW(&wc)) return false;
        class_registered = true;
    }

    for (int i = 0; i < 4; ++i) {
        if (s_resize_preview_edges[i]) continue;
        s_resize_preview_edges[i] =
            CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            L"CausalityResizePreview", L"",
                            WS_POPUP,
                            0, 0, 1, 1,
                            NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!s_resize_preview_edges[i]) return false;
    }
    return true;
}

static void resize_preview_show(int x, int y, int w, int h)
{
    if (!resize_preview_ensure()) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    const int thickness = 3;

    int edge_rects[4][4] = {
        {x, y, w, thickness},
        {x, y + h - thickness, w, thickness},
        {x, y, thickness, h},
        {x + w - thickness, y, thickness, h},
    };

    for (int i = 0; i < 4; ++i) {
        SetWindowPos(s_resize_preview_edges[i], HWND_TOPMOST,
                     edge_rects[i][0], edge_rects[i][1],
                     edge_rects[i][2], edge_rects[i][3],
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

static void resize_preview_hide(void)
{
    for (int i = 0; i < 4; ++i) {
        if (s_resize_preview_edges[i])
            ShowWindow(s_resize_preview_edges[i], SW_HIDE);
    }
}

static void resize_preview_destroy(void)
{
    for (int i = 0; i < 4; ++i) {
        if (!s_resize_preview_edges[i]) continue;
        DestroyWindow(s_resize_preview_edges[i]);
        s_resize_preview_edges[i] = NULL;
    }
    if (s_resize_preview_brush) {
        DeleteObject(s_resize_preview_brush);
        s_resize_preview_brush = NULL;
    }
}
#elif defined(__linux__)
static Window s_resize_preview_edges[4] = {0};
static unsigned long s_resize_preview_pixel = 0;

static Display *resize_preview_x11_display(void)
{
    if (glfwGetPlatform() != GLFW_PLATFORM_X11) return NULL;
    return glfwGetX11Display();
}

static bool resize_preview_ensure(void)
{
    Display *display = resize_preview_x11_display();
    if (!display) return false;

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    if (s_resize_preview_pixel == 0) {
        XColor color;
        XColor exact;
        Colormap cmap = DefaultColormap(display, screen);
        if (XAllocNamedColor(display, cmap, "#4E8CFF", &color, &exact))
            s_resize_preview_pixel = color.pixel;
        else
            s_resize_preview_pixel = WhitePixel(display, screen);
    }

    for (int i = 0; i < 4; ++i) {
        if (s_resize_preview_edges[i]) continue;
        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        attrs.save_under = True;
        attrs.background_pixel = s_resize_preview_pixel;
        attrs.event_mask = NoEventMask;
        s_resize_preview_edges[i] =
            XCreateWindow(display, root, 0, 0, 1, 1, 0,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWSaveUnder |
                              CWBackPixel | CWEventMask,
                          &attrs);
        if (!s_resize_preview_edges[i]) return false;
    }
    return true;
}

static void resize_preview_show(int x, int y, int w, int h)
{
    Display *display = resize_preview_x11_display();
    if (!display || !resize_preview_ensure()) return;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    const int thickness = 3;

    int edge_rects[4][4] = {
        {x, y, w, thickness},
        {x, y + h - thickness, w, thickness},
        {x, y, thickness, h},
        {x + w - thickness, y, thickness, h},
    };

    for (int i = 0; i < 4; ++i) {
        XMoveResizeWindow(display, s_resize_preview_edges[i],
                          edge_rects[i][0], edge_rects[i][1],
                          (unsigned int)edge_rects[i][2],
                          (unsigned int)edge_rects[i][3]);
        XMapRaised(display, s_resize_preview_edges[i]);
    }
    XFlush(display);
}

static void resize_preview_hide(void)
{
    Display *display = resize_preview_x11_display();
    if (!display) return;
    for (int i = 0; i < 4; ++i) {
        if (s_resize_preview_edges[i])
            XUnmapWindow(display, s_resize_preview_edges[i]);
    }
    XFlush(display);
}

static void resize_preview_destroy(void)
{
    Display *display = resize_preview_x11_display();
    if (!display) return;
    for (int i = 0; i < 4; ++i) {
        if (!s_resize_preview_edges[i]) continue;
        XDestroyWindow(display, s_resize_preview_edges[i]);
        s_resize_preview_edges[i] = 0;
    }
    XFlush(display);
}
#else
static void resize_preview_show(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;
}

static void resize_preview_hide(void) {}
static void resize_preview_destroy(void) {}
#endif
#else
static void resize_preview_show(int x, int y, int w, int h)
{
    ca_mac_resize_preview_show(x, y, w, h);
}

static void resize_preview_hide(void)
{
    ca_mac_resize_preview_hide();
}
#endif

/* ---- GLFW callbacks ---- */

/*
 * GLFW key callback — buffers key events and posts them to the event queue.
 *
 * Toggles the debug overlay on F9 press before buffering other key events.
 * Only PRESS and REPEAT actions are buffered for focus/input handling.
 */
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
        ca_dyn_array_reserve(&win->key_storage, win->key_count + 1u) &&
        ca_dyn_array_reserve(&win->key_action_storage, win->key_count + 1u) &&
        ca_dyn_array_reserve(&win->key_mods_storage, win->key_count + 1u)) {
        win->key_buf = win->key_storage.data;
        win->key_action_buf = win->key_action_storage.data;
        win->key_mods_buf = win->key_mods_storage.data;
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

/*
 * GLFW character callback — buffers Unicode codepoints and posts a char event.
 *
 * Appends the codepoint to the window's dynamic character buffer and posts a
 * CA_EVENT_CHAR event for the instance event system.
 */
static void glfw_char_cb(GLFWwindow *glfw, unsigned int codepoint)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (ca_dyn_array_reserve(&win->char_storage, win->char_count + 1u)) {
        win->char_buf = win->char_storage.data;
        win->char_buf[win->char_count++] = codepoint;
    }
    Ca_Event ev;
    ev.type              = CA_EVENT_CHAR;
    ev.window            = win;
    ev.character.codepoint = codepoint;
    ca_event_post(win->instance, &ev);
}

/*
 * GLFW mouse button callback — updates button state and posts an event.
 *
 * Tracks the first three mouse buttons in win->mouse_buttons[] and sets
 * mouse_click_this_frame on left-button press.
 */
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

/*
 * GLFW cursor position callback — update mouse coordinates and post a move event.
 *
 * glfw  The GLFW window.
 * x     New cursor x in logical window coordinates.
 * y     New cursor y in logical window coordinates.
 */
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

/*
 * GLFW cursor enter/leave callback — clears hover state when the cursor exits.
 *
 * Moves the recorded cursor position out-of-bounds on exit so the next input
 * pass cannot hit any node, ensuring :hover styles clear correctly.  Wakes
 * the event loop to trigger a repaint.
 */
static void glfw_cursor_enter_cb(GLFWwindow *glfw, int entered)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (!win) return;
    if (!entered) {
        /* Move the recorded cursor position out-of-bounds so the next input
           pass cannot hit any node — this guarantees :hover styles clear
           when the pointer leaves the window (GLFW does not synthesise a
           further cursor-pos event on exit). Button state is intentionally
           left untouched: an active drag must keep running while the cursor
           is outside the window. The live OS button query in the resize and
           widget input passes detects the real release. */
        win->mouse_x = -1.0;
        win->mouse_y = -1.0;
        /* Wake the event loop so the UI repaints with the cleared hover. */
        glfwPostEmptyEvent();
    }
}

/*
 * GLFW scroll callback — accumulate scroll deltas and post a scroll event.
 *
 * glfw  The GLFW window.
 * dx    Horizontal scroll delta (positive = right).
 * dy    Vertical scroll delta (positive = up).
 */
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

/*
 * GLFW window size callback — resizes the swapchain and marks layout dirty.
 *
 * Posts a CA_EVENT_WINDOW_RESIZE event, queries the framebuffer size in
 * device pixels (for HiDPI correctness), calls ca_renderer_window_resize(),
 * and marks the root node layout-dirty so the UI reflows.
 */
static void glfw_window_size_cb(GLFWwindow *glfw, int width, int height)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (!win) return;

    Ca_Event ev;
    ev.type          = CA_EVENT_WINDOW_RESIZE;
    ev.window        = win;
    ev.resize.width  = width;
    ev.resize.height = height;
    ca_event_post(win->instance, &ev);

    /* Defer the swapchain recreation to the next render frame so we do not
       call vkDeviceWaitIdle from inside a GLFW callback (which fires
       synchronously during glfwSetWindowSize). Blocking the main thread here
       prevents Cocoa from delivering mouseUp events, permanently sticking
       mouse button state during a resize drag. */
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(glfw, &fb_w, &fb_h);
    win->pending_swapchain_resize = true;
    win->pending_sc_w = fb_w;
    win->pending_sc_h = fb_h;

    if (win->root)
        win->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

/*
 * GLFW framebuffer size callback — resizes the swapchain to device pixels.
 *
 * Called by GLFW when the framebuffer resolution changes (e.g. on HiDPI).
 * Delegates to ca_renderer_window_resize() and marks the root node dirty.
 */
static void glfw_framebuffer_size_cb(GLFWwindow *glfw, int width, int height)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (!win) return;

    /* Defer — same reason as glfw_window_size_cb. */
    win->pending_swapchain_resize = true;
    win->pending_sc_w = width;
    win->pending_sc_h = height;
    if (win->root)
        win->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

/*
 * GLFW maximize callback - synchronize custom title-bar presentation with
 * compositor-managed maximize and restore state.
 *
 * glfw       The GLFW window whose state changed.
 * maximized  GLFW_TRUE when maximized, GLFW_FALSE when restored.
 */
static void glfw_window_maximize_cb(GLFWwindow *glfw, int maximized)
{
#ifdef __APPLE__
    /* Ignored on macOS: these are borderless windows whose maximize/restore is
       managed explicitly by ca_window_maximize/ca_window_restore. GLFW derives
       this callback from -[NSWindow isZoomed], which returns YES for any
       borderless window large relative to the screen — so a normal manual
       resize would spuriously flip titlebar_maximized and freeze further
       resizing (ca_window_resize_pass early-returns when maximized). */
    (void)glfw; (void)maximized;
#else
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (!win) return;

    win->titlebar_maximized = maximized == GLFW_TRUE;
    win->titlebar_needs_rebuild = true;
    glfwPostEmptyEvent();
#endif
}

/* ---- System ---- */

static int g_glfw_refcount = 0;

static int rect_intersection_area(int ax, int ay, int aw, int ah,
                                  int bx, int by, int bw, int bh)
{
    int left = ax > bx ? ax : bx;
    int top = ay > by ? ay : by;
    int right_a = ax + aw;
    int right_b = bx + bw;
    int bottom_a = ay + ah;
    int bottom_b = by + bh;
    int right = right_a < right_b ? right_a : right_b;
    int bottom = bottom_a < bottom_b ? bottom_a : bottom_b;
    if (right <= left || bottom <= top) return 0;
    return (right - left) * (bottom - top);
}

static bool window_workarea_for_rect(int win_x, int win_y, int win_w, int win_h,
                                     int *out_x, int *out_y,
                                     int *out_w, int *out_h)
{
    int center_x = win_x + win_w / 2;
    int center_y = win_y + win_h / 2;
    int monitor_count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
    GLFWmonitor *target = glfwGetPrimaryMonitor();
    int best_overlap = -1;
    for (int i = 0; monitors && i < monitor_count; ++i) {
        if (!monitors[i]) continue;
        int x = 0, y = 0, width = 0, height = 0;
        glfwGetMonitorWorkarea(monitors[i], &x, &y, &width, &height);
        if (width <= 0 || height <= 0) continue;
        if (center_x >= x && center_x < x + width &&
            center_y >= y && center_y < y + height) {
            target = monitors[i];
            best_overlap = INT_MAX;
            break;
        }
        int overlap = rect_intersection_area(win_x, win_y, win_w, win_h,
                                             x, y, width, height);
        if (overlap > best_overlap) {
            target = monitors[i];
            best_overlap = overlap;
        }
    }

    if (!target) return false;

    int gx = 0, gy = 0, gw = 0, gh = 0;
    glfwGetMonitorWorkarea(target, &gx, &gy, &gw, &gh);
    if (gw <= 0 || gh <= 0) {
        const GLFWvidmode *mode = glfwGetVideoMode(target);
        if (!mode || mode->width <= 0 || mode->height <= 0) return false;
        glfwGetMonitorPos(target, &gx, &gy);
        gw = mode->width;
        gh = mode->height;
    }

#if defined(__linux__)
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        Display *display = glfwGetX11Display();
        if (display) {
            Window root = DefaultRootWindow(display);
            Atom workarea_atom = XInternAtom(display, "_NET_WORKAREA", True);
            Atom desktop_atom = XInternAtom(display, "_NET_CURRENT_DESKTOP", True);
            Atom actual_type = None;
            int actual_format = 0;
            unsigned long nitems = 0, bytes_after = 0;
            unsigned char *desktop_data = NULL;
            unsigned long desktop = 0;

            if (desktop_atom != None &&
                XGetWindowProperty(display, root, desktop_atom, 0, 1, False,
                                   XA_CARDINAL, &actual_type, &actual_format,
                                   &nitems, &bytes_after, &desktop_data) == Success &&
                desktop_data && actual_format == 32 && nitems >= 1) {
                desktop = ((unsigned long *)desktop_data)[0];
            }
            if (desktop_data) XFree(desktop_data);

            if (workarea_atom != None) {
                unsigned char *workarea_data = NULL;
                if (XGetWindowProperty(display, root, workarea_atom, 0,
                                       LONG_MAX / 4, False, XA_CARDINAL,
                                       &actual_type, &actual_format, &nitems,
                                       &bytes_after, &workarea_data) == Success &&
                    workarea_data && actual_format == 32 && nitems >= 4) {
                    unsigned long *values = (unsigned long *)workarea_data;
                    unsigned long desktop_count = nitems / 4;
                    if (desktop >= desktop_count) desktop = 0;
                    unsigned long *wa = &values[desktop * 4];
                    int x = (int)wa[0];
                    int y = (int)wa[1];
                    int w = (int)wa[2];
                    int h = (int)wa[3];
                    int ix = x > gx ? x : gx;
                    int iy = y > gy ? y : gy;
                    int ir = x + w < gx + gw ? x + w : gx + gw;
                    int ib = y + h < gy + gh ? y + h : gy + gh;
                    if (ir > ix && ib > iy) {
                        gx = ix;
                        gy = iy;
                        gw = ir - ix;
                        gh = ib - iy;
                    }
                }
                if (workarea_data) XFree(workarea_data);
            }
        }
    }
#endif

    if (out_x) *out_x = gx;
    if (out_y) *out_y = gy;
    if (out_w) *out_w = gw;
    if (out_h) *out_h = gh;
    return gw > 0 && gh > 0;
}

/*
 * Initialise the GLFW window system, with reference counting.
 *
 * Safe to call multiple times; only the first call actually calls glfwInit().
 * Returns true on success; false if glfwInit() fails.
 */
bool ca_window_system_init(void)
{
    if (g_glfw_refcount > 0) {
        g_glfw_refcount++;
        return true;
    }
#if defined(__linux__)
    /* Custom title-bar movement and preview-resize outlines require global
       window positioning. Native Wayland intentionally forbids that, so use
       X11/XWayland when it is available. */
    const char *display = getenv("DISPLAY");
    if (display && display[0])
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (!glfwInit()) {
        fprintf(stderr, "[causality] glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g_glfw_refcount = 1;
    return true;
}

/*
 * Destroy all open windows for an instance and decrement the GLFW refcount.
 *
 * glfwTerminate() is intentionally omitted to avoid races with MoltenVK/
 * Vulkan-loader background threads on macOS; the OS reclaims GLFW resources
 * at process exit.
 *
 * inst  Instance whose windows are to be destroyed.
 */
void ca_window_system_shutdown(Ca_Instance *inst)
{
    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use) ca_window_destroy(window);
    }
    if (g_glfw_refcount > 0)
        --g_glfw_refcount;
    if (g_glfw_refcount == 0) {
#if !defined(__APPLE__)
        resize_preview_destroy();
#endif
    }
    /* glfwTerminate() is intentionally omitted.  Calling
       glfwTerminate → glfwInit in rapid succession races with
       MoltenVK / Vulkan-loader background threads on macOS,
       producing "mutex lock failed: Invalid argument" crashes.
       The OS reclaims all GLFW resources at process exit. */
}

/*
 * Process one tick of the GLFW event loop for the instance.
 *
 * Clears per-frame input flags, polls or waits for GLFW events (depending on
 * inst->continuous), dispatches event handlers, destroys any windows that
 * have requested close, and re-focuses a remaining window on macOS after a
 * close so the next click is delivered to the app rather than consumed by the OS.
 *
 * inst    Instance to tick.
 * Returns true while at least one window remains open; false when all are gone.
 */
bool ca_window_system_tick(Ca_Instance *inst)
{
    /* Clear per-frame click flags before GLFW fires callbacks */
    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use) {
            window->mouse_click_this_frame = false;
            window->scroll_dx = 0;
            window->scroll_dy = 0;
            window->scroll_this_frame = false;
            window->char_count = 0;
            window->key_count  = 0;
            memset(window->key_consumed, 0, sizeof(window->key_consumed));
        }
    }

    if (inst->continuous) {
        glfwPollEvents();
    } else if (inst->frame_deadline_pending) {
        const double remaining = inst->frame_deadline - glfwGetTime();
        if (remaining <= 0.0) {
            inst->frame_deadline_pending = false;
            glfwPollEvents();
        } else {
            glfwWaitEventsTimeout(remaining);
            if (glfwGetTime() >= inst->frame_deadline)
                inst->frame_deadline_pending = false;
        }
    } else {
        glfwWaitEvents();
    }

    /* Dispatch all queued input / resize events */
    ca_event_dispatch(inst);

    /* Fire WINDOW_CLOSE event then destroy — order matters.
       Track whether we destroyed anything so we can re-focus. */
    bool destroyed_any = false;
    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use && glfwWindowShouldClose(window->glfw)) {
            Ca_Event ev;
            ev.type   = CA_EVENT_WINDOW_CLOSE;
            ev.window = window;
            const Ca_EventHandler *h = &inst->handlers[CA_EVENT_WINDOW_CLOSE];
            if (h->fn) h->fn(&ev, h->user_data);
            ca_window_destroy(window);
            destroyed_any = true;
        }
    }

    /* After any window closes, explicitly focus the first remaining window.
       Without this, macOS moves focus to the desktop and the next click on
       the window content is consumed by the OS to re-focus the window rather
       than being delivered to the app as a button event. */
    if (destroyed_any) {
        for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
            Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
            if (window->in_use && window->glfw) {
                glfwFocusWindow(window->glfw);
                break;
            }
        }
    }

    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use) return true;
    }
    return false;
}

/* ---- Per-window ---- */

/*
 * Create a GLFW window and initialise a Ca_Window at a specific pool slot.
 *
 * Zeros the slot, creates an undecorated GLFW window, installs all GLFW
 * callbacks, boots the Vulkan surface/swapchain, and initialises the UI
 * layer.  Returns the existing window if the slot is already in use.
 *
 * inst        Owning instance.
 * desc        Window configuration (title, size).
 * inst  Instance whose stable window pool owns the new window.
 * Returns     Initialised Ca_Window pointer, or NULL on failure.
 */
static Ca_Window *window_create_in_pool(Ca_Instance *inst,
                                        const Ca_WindowDesc *desc)
{
    if (!inst || !desc) return NULL;
    Ca_Window *slot = ca_pool_acquire(&inst->windows);
    if (!slot) return NULL;

    assert(inst && desc);

    /* Zero the entire slot so no stale state survives from a previous window
       that occupied this slot (dangling pointers, input flags, etc.). */
    memset(slot, 0, sizeof(*slot));

    /* Copy the window title before creating the GLFW window */
    snprintf(slot->title, sizeof(slot->title), "%s",
             desc->title ? desc->title : "");

    /* Custom title bar: always create undecorated GLFW windows.
       Disable OS-level resizability — causality implements software resize
       via ca_window_resize_pass. On macOS, NSWindowStyleMaskResizable on a
       borderless window causes the OS to intercept edge drags and run its own
       resize loop, swallowing the mouseUp and leaving mouse state permanently
       stuck. */
    glfwWindowHint(GLFW_DECORATED,  GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING,   GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
#ifdef GLFW_MOUSE_PASSTHROUGH
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);
#endif

    GLFWwindow *glfw = glfwCreateWindow(
        desc->width  > 0 ? desc->width  : 1280,
        desc->height > 0 ? desc->height : 720,
        desc->title  ? desc->title : "causality",
        NULL, NULL
    );

    if (!glfw) {
        fprintf(stderr, "[causality] glfwCreateWindow failed\n");
        ca_pool_release(&inst->windows, slot);
        return NULL;
    }

    slot->glfw          = glfw;
    slot->instance      = inst;
    slot->in_use        = true;
    slot->on_close      = desc->on_close;
    slot->on_close_data = desc->on_close_data;
    /* Apply the instance-wide default scale if one has been set,
       otherwise fall back to 1.0 (no scaling). */
    slot->ui_scale = (inst->default_ui_scale > 0.0f)
                     ? inst->default_ui_scale : 1.0f;

    glfwSetWindowUserPointer(glfw, slot);
    glfwSetKeyCallback(glfw, glfw_key_cb);
    glfwSetCharCallback(glfw, glfw_char_cb);
    glfwSetMouseButtonCallback(glfw, glfw_mouse_button_cb);
    glfwSetCursorPosCallback(glfw, glfw_cursor_pos_cb);
    glfwSetCursorEnterCallback(glfw, glfw_cursor_enter_cb);
    glfwSetScrollCallback(glfw, glfw_scroll_cb);
    glfwSetWindowSizeCallback(glfw, glfw_window_size_cb);
    glfwSetFramebufferSizeCallback(glfw, glfw_framebuffer_size_cb);
    glfwSetWindowMaximizeCallback(glfw, glfw_window_maximize_cb);

    /* Boot surface + swapchain (renderer must already be initialised) */
    if (inst->vk_device != VK_NULL_HANDLE) {
        if (!ca_renderer_window_init(inst, slot)) {
            glfwDestroyWindow(glfw);
            slot->glfw   = NULL;
            slot->in_use = false;
            ca_pool_release(&inst->windows, slot);
            return NULL;
        }
    }

    if (!ca_ui_window_init(slot)) {
        if (inst->vk_device != VK_NULL_HANDLE)
            ca_renderer_window_shutdown(inst, slot);
        glfwDestroyWindow(glfw);
        ca_pool_release(&inst->windows, slot);
        return NULL;
    }
    if (inst->app_menu_count > 0) ca_app_menu_set(inst);

    /* Explicitly focus the new window.
       On macOS, glfwCreateWindow shows the window but does not guarantee
       keyboard/mouse focus.  Without this the first click on the window
       is consumed by the OS to bring it into focus rather than being
       delivered to the app as a button event. */
    glfwFocusWindow(glfw);

    return slot;
}

/*
 * Create a user application window in the first available pool slot.
 *
 * inst  Owning instance (must not be NULL).
 * desc  Window configuration (must not be NULL).
 * Returns  Newly created Ca_Window, or NULL if allocation or creation fails.
 */
Ca_Window *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc)
{
    assert(inst && desc);
    return window_create_in_pool(inst, desc);
}

/*
 * Create a window in a reserved (internal-use) pool slot.
 *
 * Reserved slots sit above the user window slots and are used for system
 * dialogs such as popup windows.
 *
 * inst            Owning instance.
 * desc            Window configuration.
 * reserved_index  Index within the reserved range [0, CA_RESERVED_POPUP_WINDOWS).
 * Returns         Initialised Ca_Window, or NULL on invalid arguments or failure.
 */
Ca_Window *ca_window_create_reserved(Ca_Instance *inst, const Ca_WindowDesc *desc,
                                     int reserved_index)
{
    if (!inst || !desc) return NULL;
    if (reserved_index < 0 || reserved_index >= CA_RESERVED_POPUP_WINDOWS)
        return NULL;
    return window_create_in_pool(inst, desc);
}

/*
 * Tear down a window and release all associated resources.
 *
 * Shuts down the UI layer, the renderer surface/swapchain, and the GLFW
 * window.  Marks the slot as not in use.  No-op if window is NULL or not in use.
 *
 * window  Window to destroy.
 */
void ca_window_destroy(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    Ca_Instance *instance = window->instance;

    /* Notify the caller before any resources are freed so it can safely null
       out widget pointers it holds into this window's node/widget pools. */
    if (window->on_close)
        window->on_close(window, window->on_close_data);

    /* Renderer shutdown must precede UI shutdown: ca_renderer_window_shutdown
       calls ca_viewport_gpu_destroy on viewport_pool entries, which must still
       be alive.  ca_ui_window_shutdown frees viewport_pool, so reversing this
       order would be a use-after-free. */
    if (window->instance && window->instance->vk_device != VK_NULL_HANDLE)
        ca_renderer_window_shutdown(window->instance, window);
    ca_ui_window_shutdown(window);
    glfwDestroyWindow(window->glfw);
    ca_pool_release(&instance->windows, window);
}

/*
 * Get the underlying GLFW window handle for a Causality window.
 *
 * window  Causality window to query.
 * Returns the GLFWwindow pointer, or NULL if the window is not in use.
 */
GLFWwindow *ca_window_glfw(const Ca_Window *window)
{
    if (!window || !window->in_use) return NULL;
    return window->glfw;
}

/*
 * Request a window to close by setting its GLFW close flag.
 *
 * Flags the window for closure; the next ca_window_system_tick will destroy
 * it and fire a CA_EVENT_WINDOW_CLOSE event before freeing the resources.
 *
 * window  Window to close; no-op if NULL or not in use.
 */
void ca_window_close(Ca_Window *window)
{
    if (!window || !window->in_use || !window->glfw) return;
    glfwSetWindowShouldClose(window->glfw, GLFW_TRUE);
}

/*
 * Ask the platform window manager to maximize the window.
 *
 * The compositor chooses the target monitor and work-area geometry. No-op if
 * the window is already maximized or not in use.
 *
 * window  Window to maximise.
 */
void ca_window_maximize(Ca_Window *window)
{
    if (!window || !window->in_use || !window->glfw) return;
    if (window->titlebar_maximized) return;

    glfwGetWindowPos(window->glfw,
                     &window->titlebar_restore_x, &window->titlebar_restore_y);
    glfwGetWindowSize(window->glfw,
                      &window->titlebar_restore_w, &window->titlebar_restore_h);

    int x = 0, y = 0, width = 0, height = 0;
    if (!window_workarea_for_rect(window->titlebar_restore_x,
                                  window->titlebar_restore_y,
                                  window->titlebar_restore_w,
                                  window->titlebar_restore_h,
                                  &x, &y, &width, &height))
        return;
    glfwSetWindowPos(window->glfw, x, y);
    glfwSetWindowSize(window->glfw, width, height);
    window->titlebar_maximized = true;
    window->titlebar_needs_rebuild = true;
    window->needs_render = true;
    ca_instance_wake();
}

/*
 * Restore a maximized window to its previous geometry.
 *
 * Borderless Cocoa windows restore from Causality's saved geometry. Other
 * platforms delegate restoration to their compositor or window manager.
 *
 * window  Window to restore.
 */
void ca_window_restore(Ca_Window *window)
{
    if (!window || !window->in_use || !window->glfw) return;

    if (!window->titlebar_maximized) return;
    if (window->titlebar_restore_w > 0 && window->titlebar_restore_h > 0) {
        glfwSetWindowPos(window->glfw,
                         window->titlebar_restore_x, window->titlebar_restore_y);
        glfwSetWindowSize(window->glfw,
                          window->titlebar_restore_w, window->titlebar_restore_h);
    }
    window->titlebar_maximized = false;
    window->titlebar_needs_rebuild = true;
    window->needs_render = true;
    ca_instance_wake();
}

/*
 * Get the Causality instance that owns a window.
 *
 * window  Window to query.
 * Returns the owning Ca_Instance, or NULL if the window is not in use.
 */
Ca_Instance *ca_window_instance(Ca_Window *window)
{
    return (window && window->in_use) ? window->instance : NULL;
}

/*
 * Check whether a window is currently open and in use.
 *
 * window  Window to check.
 * Returns true if the window is non-NULL and currently open, false otherwise.
 */
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

/*
 * Compute the resize-edge bitmask for a cursor position within a window.
 *
 * win_w  Window width in logical pixels.
 * win_h  Window height in logical pixels.
 * cx     Cursor x in window-local coordinates.
 * cy     Cursor y in window-local coordinates.
 * Returns  Bitmask of RESIZE_LEFT / RESIZE_RIGHT / RESIZE_TOP / RESIZE_BOTTOM,
 *          or 0 if the cursor is not in any edge hit zone.
 */
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

/*
 * Lazily create the three standard resize cursor shapes.
 *
 * Initializes the static cursor handles for horizontal, vertical, and
 * all-direction resize cursors on first call; subsequent calls are no-ops.
 */
static void ensure_cursors(void)
{
    if (s_cursors_init) return;
    s_cursors[0] = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
    s_cursors[1] = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
    s_cursors[2] = glfwCreateStandardCursor(GLFW_RESIZE_ALL_CURSOR);
    s_cursors_init = true;
}

void ca_window_set_horizontal_drag_cursor(Ca_Window *win)
{
    if (!win || !win->in_use || !win->glfw) return;
    ensure_cursors();
    glfwSetCursor(win->glfw, s_cursors[0]);
}

void ca_window_set_default_cursor(Ca_Window *win)
{
    if (!win || !win->in_use || !win->glfw) return;
    glfwSetCursor(win->glfw, NULL);
}

bool ca_window_left_button_held(Ca_Window *win)
{
    if (!win || !win->in_use || !win->glfw) return false;
#ifdef __APPLE__
    return ca_mac_left_button_held();
#elif defined(_WIN32)
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#elif defined(__linux__)
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        Display *display = glfwGetX11Display();
        Window root = DefaultRootWindow(display);
        Window root_return, child_return;
        int root_x, root_y, win_x, win_y;
        unsigned int mask = 0;
        if (display &&
            XQueryPointer(display, root, &root_return, &child_return,
                          &root_x, &root_y, &win_x, &win_y, &mask)) {
            return (mask & Button1Mask) != 0;
        }
    }
    return glfwGetMouseButton(win->glfw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
#else
    return glfwGetMouseButton(win->glfw, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
#endif
}

void ca_window_cursor_screen_pos(Ca_Window *win, double *out_x, double *out_y)
{
    double sx = 0.0, sy = 0.0;
    if (!win || !win->in_use || !win->glfw) {
        if (out_x) *out_x = sx;
        if (out_y) *out_y = sy;
        return;
    }

#ifdef __APPLE__
    ca_mac_cursor_screen_pos(&sx, &sy);
#elif defined(_WIN32)
    POINT p;
    if (GetCursorPos(&p)) {
        sx = (double)p.x;
        sy = (double)p.y;
    } else {
        int wx = 0, wy = 0;
        double cx = 0.0, cy = 0.0;
        glfwGetWindowPos(win->glfw, &wx, &wy);
        glfwGetCursorPos(win->glfw, &cx, &cy);
        sx = (double)wx + cx;
        sy = (double)wy + cy;
    }
#elif defined(__linux__)
    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
        Display *display = glfwGetX11Display();
        Window root = DefaultRootWindow(display);
        Window root_return, child_return;
        int root_x, root_y, win_x, win_y;
        unsigned int mask = 0;
        if (display &&
            XQueryPointer(display, root, &root_return, &child_return,
                          &root_x, &root_y, &win_x, &win_y, &mask)) {
            sx = (double)root_x;
            sy = (double)root_y;
        } else {
            int wx = 0, wy = 0;
            double cx = 0.0, cy = 0.0;
            glfwGetWindowPos(win->glfw, &wx, &wy);
            glfwGetCursorPos(win->glfw, &cx, &cy);
            sx = (double)wx + cx;
            sy = (double)wy + cy;
        }
    } else {
        int wx = 0, wy = 0;
        double cx = 0.0, cy = 0.0;
        glfwGetWindowPos(win->glfw, &wx, &wy);
        glfwGetCursorPos(win->glfw, &cx, &cy);
        sx = (double)wx + cx;
        sy = (double)wy + cy;
    }
#else
    int wx = 0, wy = 0;
    double cx = 0.0, cy = 0.0;
    glfwGetWindowPos(win->glfw, &wx, &wy);
    glfwGetCursorPos(win->glfw, &cx, &cy);
    sx = (double)wx + cx;
    sy = (double)wy + cy;
#endif

    if (out_x) *out_x = sx;
    if (out_y) *out_y = sy;
}

bool ca_window_titlebar_drag_pass(Ca_Window *win)
{
    if (!win || !win->in_use || !win->glfw || !win->title_bar_node)
        return false;
    if (win->titlebar_maximized) {
        win->titlebar_drag_active = false;
        win->titlebar_mouse_down = ca_window_left_button_held(win);
        return false;
    }

    bool left_down = ca_window_left_button_held(win);
    double screen_x = 0.0, screen_y = 0.0;
    ca_window_cursor_screen_pos(win, &screen_x, &screen_y);

    if (win->titlebar_drag_active) {
        if (left_down) {
            double dx = screen_x - win->titlebar_drag_screen_x;
            double dy = screen_y - win->titlebar_drag_screen_y;
            glfwSetWindowPos(win->glfw,
                             win->titlebar_drag_win_x + (int)dx,
                             win->titlebar_drag_win_y + (int)dy);
            win->titlebar_mouse_down = true;
            ca_instance_wake();
            return true;
        }
        win->titlebar_drag_active = false;
        win->titlebar_mouse_down = false;
        return false;
    }

    bool pressed_this_pass = left_down && !win->titlebar_mouse_down;
    win->titlebar_mouse_down = left_down;
    if (!pressed_this_pass) return false;

    int wx = 0, wy = 0;
    int win_w = 0, win_h = 0;
    glfwGetWindowPos(win->glfw, &wx, &wy);
    glfwGetWindowSize(win->glfw, &win_w, &win_h);
    double local_x = screen_x - (double)wx;
    double local_y = screen_y - (double)wy;

    float title_h = win->title_bar_node->desc.height;
    if (title_h <= 0.0f)
        title_h = win->title_bar_node->h;
    if (title_h <= 0.0f)
        return false;

    if (local_x < 0.0 || local_x >= (double)win_w ||
        local_y < 0.0 || local_y >= (double)title_h)
        return false;
    if (resize_edge_for_pos(win_w, win_h, local_x, local_y) != 0)
        return false;

    if (ca_pool_slot_count(&win->menubar_pool) > 0) {
        for (uint32_t bi = 0; bi < ca_pool_slot_count(&win->menubar_pool); ++bi) {
            Ca_MenuBar *mb = CA_POOL_AT(win->menubar_pool, Ca_MenuBar, bi);
            if (!mb->in_use || !mb->node) continue;
            if ((float)local_x >= mb->node->x &&
                (float)local_x <= mb->node->x + mb->node->w &&
                (float)local_y >= mb->node->y &&
                (float)local_y <= mb->node->y + mb->node->h) {
                return false;
            }
            for (int mi = 0; mi < mb->menu_count; ++mi) {
                Ca_Node *header = mb->menus[mi].header_node;
                if (!header || !header->in_use) continue;
                if ((float)local_x >= header->x &&
                    (float)local_x <= header->x + header->w &&
                    (float)local_y >= header->y &&
                    (float)local_y <= header->y + header->h) {
                    return false;
                }
            }
        }
    }

    float ui_s = win->ui_scale > 0.0f ? win->ui_scale : 1.0f;
    double control_zone_w = 104.0 * (double)ui_s;
    if (local_x >= (double)win_w - control_zone_w)
        return false;

    win->titlebar_drag_active = true;
    win->titlebar_drag_win_x = wx;
    win->titlebar_drag_win_y = wy;
    win->titlebar_drag_screen_x = screen_x;
    win->titlebar_drag_screen_y = screen_y;
    ca_instance_wake();
    return true;
}

/*
 * Handle per-frame edge/corner resize logic for undecorated windows.
 *
 * On each tick: hit-tests the cursor against window edges, shows the
 * appropriate resize cursor, and on left-click begins a resize drag.
 * While dragging, recomputes and applies the new window position and size,
 * enforcing minimum dimensions.  Ends the drag when the button is released.
 * No-op for maximised windows.
 *
 * win  Window to process.
 */
void ca_window_resize_pass(Ca_Window *win)
{
    if (!win || !win->in_use || win->titlebar_maximized) return;

    ensure_cursors();

    /* Query live button state instead of relying on the per-frame GLFW cache;
       this keeps borderless drags coherent when the cursor crosses the window
       edge and a platform stops sending ordinary mouse callbacks. */
    bool left_down = ca_window_left_button_held(win);
    if (!left_down) win->mouse_buttons[0] = false;

    double cx, cy;
    glfwGetCursorPos(win->glfw, &cx, &cy);
    int win_w, win_h;
    glfwGetWindowSize(win->glfw, &win_w, &win_h);

    /* Live cursor position in screen coordinates.  Platform helpers use native
       APIs where available and fall back to window-local GLFW coordinates. */
    int wx, wy;
    glfwGetWindowPos(win->glfw, &wx, &wy);
    double screen_x, screen_y;
    ca_window_cursor_screen_pos(win, &screen_x, &screen_y);

    /* --- Continue active resize --- */
    if (win->resize_active) {
        double ddx = screen_x - win->resize_start_cursor_sx;
        double ddy = screen_y - win->resize_start_cursor_sy;

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

        win->resize_target_x = new_x;
        win->resize_target_y = new_y;
        win->resize_target_w = new_w;
        win->resize_target_h = new_h;

        if (left_down) {
            /* Resizing the real OS window mid-drag can cancel borderless-window
               mouse tracking on multiple platforms. Show a preview outline at
               the target rect instead; the real window is resized on release. */
            resize_preview_show(new_x, new_y, new_w, new_h);
        } else {
            /* End of drag — hide the preview and apply the final geometry. */
            resize_preview_hide();
            glfwSetWindowPos(win->glfw, win->resize_target_x, win->resize_target_y);
            glfwSetWindowSize(win->glfw, win->resize_target_w, win->resize_target_h);
            win->resize_active = false;
            glfwSetCursor(win->glfw, NULL);
        }
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
        win->resize_active          = true;
        win->resize_edge            = edge;
        win->resize_start_win_x     = wx;
        win->resize_start_win_y     = wy;
        win->resize_start_win_w     = win_w;
        win->resize_start_win_h     = win_h;
        win->resize_start_cursor_sx = screen_x;
        win->resize_start_cursor_sy = screen_y;
        win->resize_target_x        = wx;
        win->resize_target_y        = wy;
        win->resize_target_w        = win_w;
        win->resize_target_h        = win_h;
        resize_preview_show(wx, wy, win_w, win_h);
        ca_instance_wake();
    }
}

/*
 * Set the UI scale for this window's instance (affects all windows).
 *
 * Delegates to ca_instance_set_scale(); see its documentation for semantics.
 *
 * window  Any open window belonging to the target instance.
 * scale   Desired scale factor.
 */
void ca_window_set_scale(Ca_Window *window, float scale)
{
    if (!window || !window->in_use) return;
    ca_instance_set_scale(window->instance, scale);
}

/*
 * Return the current UI scale for this window's instance.
 *
 * window  Any open window belonging to the target instance; returns 1.0 if NULL.
 * Returns Current scale factor.
 */
float ca_window_get_scale(Ca_Window *window)
{
    if (!window || !window->in_use) return 1.0f;
    return ca_instance_get_scale(window->instance);
}

/*
 * Return framebuffer pixels per logical window unit.
 *
 * window  Window to query.
 * Returns Current pixel ratio, or 1.0 when unavailable.
 */
float ca_window_get_pixel_ratio(Ca_Window *window)
{
    if (!window || !window->glfw) return 1.0f;
    int logical_w = 0, logical_h = 0;
    int framebuffer_w = 0, framebuffer_h = 0;
    glfwGetWindowSize(window->glfw, &logical_w, &logical_h);
    glfwGetFramebufferSize(window->glfw, &framebuffer_w, &framebuffer_h);
    if (logical_w > 0 && framebuffer_w > 0)
        return (float)framebuffer_w / (float)logical_w;
    if (logical_h > 0 && framebuffer_h > 0)
        return (float)framebuffer_h / (float)logical_h;
    return 1.0f;
}

/*
 * Return the resolved logical height of the system title bar.
 *
 * window  Window to query.
 * Returns Reserved title-bar height, or 0 when unavailable.
 */
float ca_window_get_title_bar_height(Ca_Window *window)
{
    if (!window || !window->title_bar_node) return 0.0f;
    return window->title_bar_node->desc.height;
}

/*
 * Write a UTF-8 string to the system clipboard via GLFW.
 *
 * window  Window whose GLFW context is used for the clipboard call.
 * text    Text to set; passing NULL sets an empty string.
 */
void ca_clipboard_set_text(Ca_Window *window, const char *text)
{
    if (!window || !window->glfw) return;
    glfwSetClipboardString(window->glfw, text ? text : "");
}

/*
 * Read the current clipboard contents as a UTF-8 string.
 *
 * window  Window whose GLFW context is used for the clipboard call.
 * Returns Pointer to the clipboard text (owned by GLFW), or NULL.
 */
const char *ca_clipboard_get_text(Ca_Window *window)
{
    if (!window || !window->glfw) return NULL;
    return glfwGetClipboardString(window->glfw);
}

/*
 * Register or replace the per-window background render callback.
 *
 * window     Target window.
 * fn         Invoked each frame before the UI render pass; NULL disables.
 * user_data  Passed unchanged to fn.
 */
void ca_window_set_bg_render(Ca_Window *window, Ca_BgRenderFn fn, void *user_data)
{
    if (!window) return;
    window->bg_render_fn   = fn;
    window->bg_render_data = user_data;
}
