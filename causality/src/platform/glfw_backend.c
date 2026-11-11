// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#endif

#include "window.h"
#include "platform.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>
#include <dlfcn.h>

/* libX11 is loaded on demand, like GLFW does, so the binary has no load-time
   X11 dependency and still starts on Wayland-only systems. */
#define CA_X11_FUNCS(X) \
    X(XAllocNamedColor) X(XCreateWindow) X(XDestroyWindow) X(XFlush) X(XFree) \
    X(XGetWindowProperty) X(XInternAtom) X(XMapRaised) X(XMoveResizeWindow) \
    X(XQueryPointer) X(XUnmapWindow)

static struct {
#define X(fn) __typeof__(fn) *fn;
    CA_X11_FUNCS(X)
#undef X
} s_x11;

static bool ca_x11_ready(void)
{
    static int state;
    if (state) return state > 0;
    state = -1;
    void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
#define X(fn) *(void **)&s_x11.fn = dlsym(lib, #fn);
    CA_X11_FUNCS(X)
#undef X
#define X(fn) if (!s_x11.fn) { dlclose(lib); return false; }
    CA_X11_FUNCS(X)
#undef X
    state = 1;
    return true;
}

#define XAllocNamedColor s_x11.XAllocNamedColor
#define XCreateWindow s_x11.XCreateWindow
#define XDestroyWindow s_x11.XDestroyWindow
#define XFlush s_x11.XFlush
#define XFree s_x11.XFree
#define XGetWindowProperty s_x11.XGetWindowProperty
#define XInternAtom s_x11.XInternAtom
#define XMapRaised s_x11.XMapRaised
#define XMoveResizeWindow s_x11.XMoveResizeWindow
#define XQueryPointer s_x11.XQueryPointer
#define XUnmapWindow s_x11.XUnmapWindow

static Display *ca_x11_display(void)
{
    if (glfwGetPlatform() != GLFW_PLATFORM_X11 || !ca_x11_ready()) return NULL;
    return glfwGetX11Display();
}
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

static bool resize_preview_ensure(void)
{
    Display *display = ca_x11_display();
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
    Display *display = ca_x11_display();
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
    Display *display = ca_x11_display();
    if (!display) return;
    for (int i = 0; i < 4; ++i) {
        if (s_resize_preview_edges[i])
            XUnmapWindow(display, s_resize_preview_edges[i]);
    }
    XFlush(display);
}

static void resize_preview_destroy(void)
{
    Display *display = ca_x11_display();
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

static void cursor_screen_position(const Ca_Window *window, double *x, double *y);

/* ---- GLFW callbacks ---- */

/* Desktop events already use the published Causality key and action values. */
_Static_assert(CA_KEY_SPACE == GLFW_KEY_SPACE && CA_KEY_MENU == GLFW_KEY_MENU &&
               CA_PRESS == GLFW_PRESS && CA_RELEASE == GLFW_RELEASE && CA_REPEAT == GLFW_REPEAT &&
               CA_MOD_SHIFT == GLFW_MOD_SHIFT && CA_MOD_CONTROL == GLFW_MOD_CONTROL &&
               CA_MOD_ALT == GLFW_MOD_ALT && CA_MOD_SUPER == GLFW_MOD_SUPER,
               "GLFW input values must match the Causality event API");

static void glfw_key_cb(GLFWwindow *glfw, int key, int scancode, int action, int mods)
{
    ca_window_input_key(glfwGetWindowUserPointer(glfw), key, scancode, action, mods);
}

static void glfw_char_cb(GLFWwindow *glfw, unsigned int codepoint)
{
    ca_window_input_char(glfwGetWindowUserPointer(glfw), codepoint);
}

static void glfw_mouse_button_cb(GLFWwindow *glfw, int button, int action, int mods)
{
    Ca_Window *window = glfwGetWindowUserPointer(glfw);
    if (!window) return;
    double x = window->mouse_x, y = window->mouse_y;
    if (x < 0.0 || y < 0.0) glfwGetCursorPos(glfw, &x, &y);
    ca_window_input_button(window, button, action, mods, x, y);
}

static void glfw_cursor_pos_cb(GLFWwindow *glfw, double x, double y)
{
    ca_window_input_move(glfwGetWindowUserPointer(glfw), x, y);
}

static void glfw_cursor_enter_cb(GLFWwindow *glfw, int entered)
{
    if (!entered) ca_window_input_leave(glfwGetWindowUserPointer(glfw));
}

static void glfw_scroll_cb(GLFWwindow *glfw, double dx, double dy)
{
    ca_window_input_scroll(glfwGetWindowUserPointer(glfw), dx, dy);
}

static void glfw_window_size_cb(GLFWwindow *glfw, int width, int height)
{
    ca_window_handle_resize(glfwGetWindowUserPointer(glfw), width, height);
}

static void glfw_framebuffer_size_cb(GLFWwindow *glfw, int width, int height)
{
    ca_window_handle_framebuffer_resize(glfwGetWindowUserPointer(glfw), width, height);
}

/** Synchronize compositor-managed maximize and restore with the custom title bar. */
static void glfw_window_maximize_cb(GLFWwindow *glfw, int maximized)
{
#ifdef __APPLE__
    /* Cocoa's isZoomed reports large manually resized windows as maximized. */
    (void)glfw; (void)maximized;
#else
    Ca_Window *win = glfwGetWindowUserPointer(glfw);
    if (!win) return;
    win->titlebar_maximized = maximized == GLFW_TRUE;
    win->titlebar_needs_rebuild = true;
    glfwPostEmptyEvent();
#endif
}

/** Deliver native file drops using the live pointer location in screen coordinates. */
static void glfw_drop_cb(GLFWwindow *glfw, int count, const char **paths)
{
    Ca_Window *win = glfwGetWindowUserPointer(glfw);
    if (!win || !win->in_use || count <= 0 || !paths || !win->instance->file_drop_fn) return;
    double x = 0.0, y = 0.0;
    cursor_screen_position(win, &x, &y);
    win->instance->file_drop_fn(win, (const char *const *)paths, count, (float)x, (float)y,
                                win->instance->file_drop_data);
    glfwPostEmptyEvent();
}

static void glfw_window_focus_cb(GLFWwindow *glfw, int focused)
{
    if (!focused) ca_window_input_unfocus(glfwGetWindowUserPointer(glfw));
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
    {
        Display *display = ca_x11_display();
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
bool ca_platform_init(void)
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

void ca_platform_shutdown(void)
{
    if (g_glfw_refcount > 0)
        --g_glfw_refcount;
#if !defined(__APPLE__)
    if (g_glfw_refcount == 0)
        resize_preview_destroy();
#endif
    /* Retain process-level initialization across instance shutdowns to
       avoid racing MoltenVK loader threads on desktop macOS. */
}

double ca_platform_time(void)
{
    return glfwGetTime();
}

void ca_platform_wake(void)
{
    glfwPostEmptyEvent();
}

void ca_platform_process_events(Ca_Instance *instance, double timeout)
{
    (void)instance;
    if (timeout < 0.0)
        glfwWaitEvents();
    else if (timeout > 0.0)
        glfwWaitEventsTimeout(timeout);
    else
        glfwPollEvents();
}

/** Forward a system close request to the logical window's close handler. */
static void glfw_window_close_cb(GLFWwindow *glfw)
{
    ca_window_close(glfwGetWindowUserPointer(glfw));
}

bool ca_platform_window_create(Ca_Window *slot, const Ca_WindowDesc *desc, void *host_surface)
{
    if (host_surface) return false;
    /* Custom title bar: always create undecorated GLFW windows.
       Disable OS-level resizability — causality implements software resize
       via ca_platform_window_resize_pass. On macOS, NSWindowStyleMaskResizable on a
       borderless window causes the OS to intercept edge drags and run its own
       resize loop, swallowing the mouseUp and leaving mouse state permanently
       stuck. */
    const bool native_frame = slot->native_frame;
    glfwWindowHint(GLFW_DECORATED,  native_frame && !desc->fullscreen ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE,  native_frame && desc->resizable && !desc->fullscreen
                                    ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING,   GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, desc->no_focus ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, desc->no_focus ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
#ifdef GLFW_MOUSE_PASSTHROUGH
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);
#endif

    int width  = desc->width  > 0 ? desc->width  : 1280;
    int height = desc->height > 0 ? desc->height : 720;
    GLFWmonitor *monitor = NULL;
    if (desc->fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : NULL;
        if (mode) {
            glfwWindowHint(GLFW_RED_BITS,     mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS,   mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS,    mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
            width  = mode->width;
            height = mode->height;
        } else {
            monitor = NULL;
        }
    }

    GLFWwindow *glfw = glfwCreateWindow(width, height,
        desc->title ? desc->title : "causality", monitor, NULL);

    if (!glfw) {
        fprintf(stderr, "[causality] glfwCreateWindow failed\n");
        return false;
    }

    slot->platform_handle = glfw;
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
    glfwSetWindowFocusCallback(glfw, glfw_window_focus_cb);
    glfwSetDropCallback(glfw, glfw_drop_cb);
    glfwSetWindowCloseCallback(glfw, glfw_window_close_cb);
    return true;
}

void ca_platform_window_destroy(Ca_Window *window)
{
    if (!window->platform_handle) return;
    glfwDestroyWindow(window->platform_handle);
    window->platform_handle = NULL;
}

void ca_platform_window_focus(Ca_Window *window)
{
    if (window->platform_handle) glfwFocusWindow(window->platform_handle);
}

void ca_platform_window_size(const Ca_Window *window, int *width, int *height)
{
    if (width) *width = 0;
    if (height) *height = 0;
    if (window && window->platform_handle)
        glfwGetWindowSize(window->platform_handle, width, height);
}

void ca_platform_framebuffer_size(const Ca_Window *window, int *width, int *height)
{
    if (width) *width = 0;
    if (height) *height = 0;
    if (window && window->platform_handle)
        glfwGetFramebufferSize(window->platform_handle, width, height);
}

void ca_platform_content_scale(const Ca_Window *window, float *x, float *y)
{
    if (x) *x = 1.0f;
    if (y) *y = 1.0f;
    if (window && window->platform_handle)
        glfwGetWindowContentScale(window->platform_handle, x, y);
}

void ca_platform_window_set_title(Ca_Window *window, const char *title)
{
    if (window->platform_handle) glfwSetWindowTitle(window->platform_handle, title);
}

void ca_platform_window_minimize(Ca_Window *window)
{
    if (window->platform_handle) glfwIconifyWindow(window->platform_handle);
}

void ca_platform_window_set_drag_preview(Ca_Window *window)
{
    if (!window->platform_handle) return;
    glfwSetWindowAttrib(window->platform_handle, GLFW_FLOATING, GLFW_TRUE);
    glfwSetWindowAttrib(window->platform_handle, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
}

const char **ca_platform_vk_extensions(uint32_t *count)
{
    return glfwGetRequiredInstanceExtensions(count);
}

bool ca_platform_vk_presentation_supported(VkInstance instance, VkPhysicalDevice device,
                                            uint32_t queue_family, VkQueueFlags flags)
{
    (void)flags;
    return glfwGetPhysicalDevicePresentationSupport(instance, device, queue_family) == GLFW_TRUE;
}

VkResult ca_platform_vk_surface_create(VkInstance instance, const Ca_Window *window,
                                        VkSurfaceKHR *surface)
{
    return glfwCreateWindowSurface(instance, window->platform_handle, NULL, surface);
}

/** Restores an iconified window and requests OS input focus. */
void ca_window_focus(Ca_Window *window)
{
    if (!window || !window->in_use || !window->platform_handle) return;
    if (glfwGetWindowAttrib(window->platform_handle, GLFW_ICONIFIED)) glfwRestoreWindow(window->platform_handle);
    glfwFocusWindow(window->platform_handle);
}

/*
 * Report the window's client-area rectangle in screen coordinates.
 *
 * window  Window to query.
 * x, y    Receive the top-left corner; optional.
 * width, height  Receive the client size; optional.
 */
void ca_window_get_rect(const Ca_Window *window,
                        int *x, int *y, int *width, int *height)
{
    int px = 0, py = 0, pw = 0, ph = 0;
    if (window && window->in_use && window->platform_handle) {
        if (x || y) glfwGetWindowPos(window->platform_handle, &px, &py);
        if (width || height) glfwGetWindowSize(window->platform_handle, &pw, &ph);
    }
    if (x) *x = px;
    if (y) *y = py;
    if (width) *width = pw;
    if (height) *height = ph;
}

/*
 * Read the cursor live from the platform in screen coordinates.
 *
 * window  Window whose coordinate space the platform cursor is queried in.
 * x, y    Receive the screen position.
 * Returns false when the window is invalid.
 */
bool ca_window_cursor_screen_position(const Ca_Window *window, float *x, float *y)
{
    if (!window || !window->in_use || !window->platform_handle) return false;
    double sx = 0.0, sy = 0.0;
    cursor_screen_position(window, &sx, &sy);
    if (x) *x = (float)sx;
    if (y) *y = (float)sy;
    return true;
}

/*
 * Report whether a mouse button is currently held.
 *
 * window  Window tracking the button state.
 * button  0 = left, 1 = right, 2 = middle.
 */
bool ca_window_mouse_button_down(const Ca_Window *window, int button)
{
    if (!window || !window->in_use || !window->platform_handle || button < 0 || button > 2) return false;
    if (button == 0) return ca_platform_primary_button_held((Ca_Window *)window);
    return glfwGetMouseButton(window->platform_handle, button) == GLFW_PRESS;
}

void ca_window_cancel_move(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    window->titlebar_drag_active = false;
    window->titlebar_mouse_down = true;
}

bool ca_window_key_down(const Ca_Window *window, int key)
{
    return window && window->in_use && window->platform_handle && key >= GLFW_KEY_SPACE &&
           key <= GLFW_KEY_LAST && glfwGetKey(window->platform_handle, key) == GLFW_PRESS;
}

bool ca_window_focused(const Ca_Window *window)
{
    return window && window->in_use && window->platform_handle &&
           glfwGetWindowAttrib(window->platform_handle, GLFW_FOCUSED) == GLFW_TRUE;
}

/*
 * Set whole-window opacity.
 *
 * window   Window to change; no-op when invalid.
 * opacity  Value in [0, 1], clamped.
 */
void ca_window_set_opacity(Ca_Window *window, float opacity)
{
    if (!window || !window->in_use || !window->platform_handle) return;
    if (!(opacity > 0.0f)) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    glfwSetWindowOpacity(window->platform_handle, opacity);
}

/*
 * Move the window's client area to a screen position.
 *
 * window  Window to move; no-op when invalid.
 * x, y    Target top-left corner in screen coordinates.
 */
void ca_window_set_position(Ca_Window *window, int x, int y)
{
    if (!window || !window->in_use || !window->platform_handle) return;
    glfwSetWindowPos(window->platform_handle, x, y);
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
    if (!window || !window->in_use || !window->platform_handle) return;
    if (window->titlebar_maximized) return;

    glfwGetWindowPos(window->platform_handle,
                     &window->titlebar_restore_x, &window->titlebar_restore_y);
    glfwGetWindowSize(window->platform_handle,
                      &window->titlebar_restore_w, &window->titlebar_restore_h);

    int x = 0, y = 0, width = 0, height = 0;
    if (!window_workarea_for_rect(window->titlebar_restore_x,
                                  window->titlebar_restore_y,
                                  window->titlebar_restore_w,
                                  window->titlebar_restore_h,
                                  &x, &y, &width, &height))
        return;
    glfwSetWindowPos(window->platform_handle, x, y);
    glfwSetWindowSize(window->platform_handle, width, height);
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
    if (!window || !window->in_use || !window->platform_handle) return;

    if (!window->titlebar_maximized) return;
    if (window->titlebar_restore_w > 0 && window->titlebar_restore_h > 0) {
        glfwSetWindowPos(window->platform_handle,
                         window->titlebar_restore_x, window->titlebar_restore_y);
        glfwSetWindowSize(window->platform_handle,
                          window->titlebar_restore_w, window->titlebar_restore_h);
    }
    window->titlebar_maximized = false;
    window->titlebar_needs_rebuild = true;
    window->needs_render = true;
    ca_instance_wake();
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

void ca_platform_set_horizontal_drag_cursor(Ca_Window *win)
{
    if (!win || !win->in_use || !win->platform_handle) return;
    ensure_cursors();
    glfwSetCursor(win->platform_handle, s_cursors[0]);
}

void ca_platform_set_default_cursor(Ca_Window *win)
{
    if (!win || !win->in_use || !win->platform_handle) return;
    glfwSetCursor(win->platform_handle, NULL);
}

bool ca_platform_primary_button_held(Ca_Window *win)
{
    if (!win || !win->in_use || !win->platform_handle) return false;
#ifdef __APPLE__
    return ca_mac_left_button_held();
#elif defined(_WIN32)
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#elif defined(__linux__)
    Display *display = ca_x11_display();
    if (display) {
        Window root = DefaultRootWindow(display);
        Window root_return, child_return;
        int root_x, root_y, win_x, win_y;
        unsigned int mask = 0;
        if (XQueryPointer(display, root, &root_return, &child_return,
                          &root_x, &root_y, &win_x, &win_y, &mask)) {
            return (mask & Button1Mask) != 0;
        }
    }
    return glfwGetMouseButton(win->platform_handle, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
#else
    return glfwGetMouseButton(win->platform_handle, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
#endif
}

static void cursor_screen_position(const Ca_Window *win, double *out_x, double *out_y)
{
    double sx = 0.0, sy = 0.0;
    if (!win || !win->in_use || !win->platform_handle) {
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
        glfwGetWindowPos(win->platform_handle, &wx, &wy);
        glfwGetCursorPos(win->platform_handle, &cx, &cy);
        sx = (double)wx + cx;
        sy = (double)wy + cy;
    }
#elif defined(__linux__)
    Display *display = ca_x11_display();
    Window root_return, child_return;
    int root_x, root_y, win_x, win_y;
    unsigned int mask = 0;
    if (display &&
        XQueryPointer(display, DefaultRootWindow(display), &root_return, &child_return,
                      &root_x, &root_y, &win_x, &win_y, &mask)) {
        sx = (double)root_x;
        sy = (double)root_y;
    } else {
        int wx = 0, wy = 0;
        double cx = 0.0, cy = 0.0;
        glfwGetWindowPos(win->platform_handle, &wx, &wy);
        glfwGetCursorPos(win->platform_handle, &cx, &cy);
        sx = (double)wx + cx;
        sy = (double)wy + cy;
    }
#else
    int wx = 0, wy = 0;
    double cx = 0.0, cy = 0.0;
    glfwGetWindowPos(win->platform_handle, &wx, &wy);
    glfwGetCursorPos(win->platform_handle, &cx, &cy);
    sx = (double)wx + cx;
    sy = (double)wy + cy;
#endif

    if (out_x) *out_x = sx;
    if (out_y) *out_y = sy;
}

bool ca_platform_titlebar_drag_pass(Ca_Window *win)
{
    if (!win || !win->in_use || !win->platform_handle || !win->title_bar_node || win->native_frame || win->titlebar_hidden)
        return false;
    if (win->titlebar_maximized) {
        win->titlebar_drag_active = false;
        win->titlebar_mouse_down = ca_platform_primary_button_held(win);
        return false;
    }

    bool left_down = ca_platform_primary_button_held(win);
    double screen_x = 0.0, screen_y = 0.0;
    cursor_screen_position(win, &screen_x, &screen_y);

    if (win->titlebar_drag_active) {
        if (left_down) {
            double dx = screen_x - win->titlebar_drag_screen_x;
            double dy = screen_y - win->titlebar_drag_screen_y;
            glfwSetWindowPos(win->platform_handle,
                             win->titlebar_drag_win_x + (int)dx,
                             win->titlebar_drag_win_y + (int)dy);
            win->titlebar_mouse_down = true;
            ca_instance_wake();
            return true;
        }
        win->titlebar_drag_active = false;
        win->titlebar_mouse_down = false;
        if (win->on_title_drag) win->on_title_drag(win, false, win->on_title_drag_data);
        return false;
    }

    bool pressed_this_pass = left_down && !win->titlebar_mouse_down;
    win->titlebar_mouse_down = left_down;
    if (!pressed_this_pass) return false;

    int wx = 0, wy = 0;
    int win_w = 0, win_h = 0;
    glfwGetWindowPos(win->platform_handle, &wx, &wy);
    glfwGetWindowSize(win->platform_handle, &win_w, &win_h);
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

    const Ca_Node *drag = win->title_drag_node;
    if (!drag || !drag->in_use || drag->desc.hidden ||
        local_x < drag->x || local_x >= drag->x + drag->w ||
        local_y < drag->y || local_y >= drag->y + drag->h)
        return false;

    win->titlebar_drag_active = true;
    win->titlebar_drag_win_x = wx;
    win->titlebar_drag_win_y = wy;
    win->titlebar_drag_screen_x = screen_x;
    win->titlebar_drag_screen_y = screen_y;
    if (win->on_title_drag) win->on_title_drag(win, true, win->on_title_drag_data);
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
void ca_platform_window_resize_pass(Ca_Window *win)
{
    if (!win || !win->in_use || win->titlebar_maximized || win->native_frame) return;

    ensure_cursors();

    /* Query live button state instead of relying on the per-frame GLFW cache;
       this keeps borderless drags coherent when the cursor crosses the window
       edge and a platform stops sending ordinary mouse callbacks. */
    bool left_down = ca_platform_primary_button_held(win);
    if (!left_down) win->mouse_buttons[0] = false;

    double cx, cy;
    glfwGetCursorPos(win->platform_handle, &cx, &cy);
    int win_w, win_h;
    glfwGetWindowSize(win->platform_handle, &win_w, &win_h);

    /* Live cursor position in screen coordinates.  Platform helpers use native
       APIs where available and fall back to window-local GLFW coordinates. */
    int wx, wy;
    glfwGetWindowPos(win->platform_handle, &wx, &wy);
    double screen_x, screen_y;
    cursor_screen_position(win, &screen_x, &screen_y);

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
            glfwSetWindowPos(win->platform_handle, win->resize_target_x, win->resize_target_y);
            glfwSetWindowSize(win->platform_handle, win->resize_target_w, win->resize_target_h);
            win->resize_active = false;
            glfwSetCursor(win->platform_handle, NULL);
        }
        return;
    }

    /* --- Not resizing — hit-test edges to show cursor feedback --- */
    int edge = resize_edge_for_pos(win_w, win_h, cx, cy);

    if (edge == 0) {
        glfwSetCursor(win->platform_handle, NULL);
        return;
    }

    /* Choose cursor shape */
    bool horiz = (edge == RESIZE_LEFT  || edge == RESIZE_RIGHT);
    bool vert  = (edge == RESIZE_TOP   || edge == RESIZE_BOTTOM);
    if (horiz && !vert)       glfwSetCursor(win->platform_handle, s_cursors[0]);
    else if (vert && !horiz)  glfwSetCursor(win->platform_handle, s_cursors[1]);
    else                      glfwSetCursor(win->platform_handle, s_cursors[2]);

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
 * Write a UTF-8 string to the system clipboard via GLFW.
 *
 * window  Window whose GLFW context is used for the clipboard call.
 * text    Text to set; passing NULL sets an empty string.
 */
void ca_clipboard_set_text(Ca_Window *window, const char *text)
{
    if (!window || !window->platform_handle) return;
    glfwSetClipboardString(window->platform_handle, text ? text : "");
}

/*
 * Read the current clipboard contents as a UTF-8 string.
 *
 * window  Window whose GLFW context is used for the clipboard call.
 * Returns Pointer to the clipboard text (owned by GLFW), or NULL.
 */
const char *ca_clipboard_get_text(Ca_Window *window)
{
    if (!window || !window->platform_handle) return NULL;
    return glfwGetClipboardString(window->platform_handle);
}
