// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "window.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"

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

/*
 * GLFW character callback — buffers Unicode codepoints and posts a char event.
 *
 * Appends the codepoint to the window's char_buf (up to CA_CHAR_BUF_MAX)
 * and posts a CA_EVENT_CHAR event for the instance event system.
 */
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
           further cursor-pos event on exit). */
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

    /* GLFW window size is in logical screen coordinates. Vulkan swapchains
       must track framebuffer pixels, otherwise maximized/HiDPI windows can
       be compositor-scaled and the UI appears blurred. */
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(glfw, &fb_w, &fb_h);
    ca_renderer_window_resize(win->instance, win, fb_w, fb_h);

    /* Mark the root layout-dirty so ui_update re-flows and repaints */
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

    ca_renderer_window_resize(win->instance, win, width, height);
    if (win->root)
        win->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

/*
 * GLFW window refresh callback — unblocks rendering during OS-modal resize.
 *
 * On macOS, native window decorations can enter a brief modal tracking loop
 * that prevents glfwPollEvents from returning.  This callback fires inside
 * that loop.  We only mark the pending resize and dirty flags here; the
 * actual swapchain rebuild and render happen in ca_renderer_frame on the
 * next normal tick, safely outside any GLFW callback context.
 */
static void glfw_window_refresh_cb(GLFWwindow *glfw)
{
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (!win || !win->instance) return;

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(glfw, &fb_w, &fb_h);
    if (fb_w > 0 && fb_h > 0)
        ca_renderer_window_resize(win->instance, win, fb_w, fb_h);

    if (win->root)
        win->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;

    win->needs_render = true;
    glfwPostEmptyEvent();
}

static void glfw_window_maximize_cb(GLFWwindow *glfw, int maximized)
{
    (void)maximized;
    Ca_Window *win = (Ca_Window *)glfwGetWindowUserPointer(glfw);
    if (!win) return;
    glfwPostEmptyEvent();
}

/* ---- System ---- */

static int g_glfw_refcount = 0;

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
    for (int i = 0; i < CA_MAX_WINDOWS_TOTAL; ++i)
        if (inst->windows[i].in_use) {
            inst->windows[i].mouse_click_this_frame = false;
            inst->windows[i].scroll_dx = 0;
            inst->windows[i].scroll_dy = 0;
            inst->windows[i].scroll_this_frame = false;
            inst->windows[i].char_count = 0;
            inst->windows[i].key_count  = 0;
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

/*
 * Create a GLFW window and initialise a Ca_Window at a specific pool slot.
 *
 * Zeros the slot, creates an undecorated GLFW window, installs all GLFW
 * callbacks, boots the Vulkan surface/swapchain, and initialises the UI
 * layer.  Returns the existing window if the slot is already in use.
 *
 * inst        Owning instance.
 * desc        Window configuration (title, size).
 * slot_index  Index into inst->windows[]; must be in [0, CA_MAX_WINDOWS_TOTAL).
 * Returns     Initialised Ca_Window pointer, or NULL on failure.
 */
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

    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

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
    glfwSetCursorEnterCallback(glfw, glfw_cursor_enter_cb);
    glfwSetScrollCallback(glfw, glfw_scroll_cb);
    glfwSetWindowSizeCallback(glfw, glfw_window_size_cb);
    glfwSetFramebufferSizeCallback(glfw, glfw_framebuffer_size_cb);
    glfwSetWindowRefreshCallback(glfw, glfw_window_refresh_cb);
    glfwSetWindowMaximizeCallback(glfw, glfw_window_maximize_cb);

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

/*
 * Create a user application window in the first available pool slot.
 *
 * inst  Owning instance (must not be NULL).
 * desc  Window configuration (must not be NULL).
 * Returns  Newly created Ca_Window, or NULL if the pool is exhausted.
 */
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
    return window_create_in_slot(inst, desc, CA_MAX_WINDOWS + reserved_index);
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
    /* Renderer shutdown must precede UI shutdown: ca_renderer_window_shutdown
       calls ca_viewport_gpu_destroy on viewport_pool entries, which must still
       be alive.  ca_ui_window_shutdown frees viewport_pool, so reversing this
       order would be a use-after-free. */
    if (window->instance && window->instance->vk_device != VK_NULL_HANDLE)
        ca_renderer_window_shutdown(window->instance, window);
    ca_ui_window_shutdown(window);
    glfwDestroyWindow(window->glfw);
    window->glfw     = NULL;
    window->instance = NULL;
    window->in_use   = false;
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
    if (!glfwGetWindowAttrib(window->glfw, GLFW_MAXIMIZED))
        glfwMaximizeWindow(window->glfw);
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
    glfwRestoreWindow(window->glfw);
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

/*
 * Show the horizontal-resize cursor to indicate a numeric drag control.
 *
 * window  Window on which to set the cursor.
 */
void ca_window_set_horizontal_drag_cursor(Ca_Window *win)
{
    if (!win || !win->in_use || !win->glfw) return;
    static GLFWcursor *s_hresize = NULL;
    if (!s_hresize)
        s_hresize = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
    glfwSetCursor(win->glfw, s_hresize);
}

/*
 * Restore the platform-default cursor for a window.
 *
 * window  Window on which to reset the cursor.
 */
void ca_window_set_default_cursor(Ca_Window *win)
{
    if (!win || !win->in_use || !win->glfw) return;
    glfwSetCursor(win->glfw, NULL);
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
