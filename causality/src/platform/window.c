// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#include "window.h"
#include "platform.h"
#include "event.h"
#include "renderer.h"
#include "ui.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void ca_window_input_key(Ca_Window *window, int key, int scancode, int action, int mods)
{
    if (!window || !window->in_use) return;
    if (key == CA_KEY_F9 && action == CA_PRESS) {
        window->debug_overlay = !window->debug_overlay;
        window->dbg_force_repaint = true;
        return;
    }
    size_t index = window->key_count;
    if ((action == CA_PRESS || action == CA_REPEAT) &&
        ca_dyn_array_reserve(&window->key_storage, index + 1) &&
        ca_dyn_array_reserve(&window->key_action_storage, index + 1) &&
        ca_dyn_array_reserve(&window->key_mods_storage, index + 1)) {
        window->key_buf = window->key_storage.data;
        window->key_action_buf = window->key_action_storage.data;
        window->key_mods_buf = window->key_mods_storage.data;
        window->key_buf[index] = key;
        window->key_action_buf[index] = action;
        window->key_mods_buf[index] = mods;
        ++window->key_count;
    }
    ca_event_post(window->instance, &(Ca_Event){
        .type = CA_EVENT_KEY, .window = window, .key = {key, scancode, action, mods}});
}

void ca_window_input_char(Ca_Window *window, uint32_t codepoint)
{
    if (!window || !window->in_use) return;
    if (ca_dyn_array_reserve(&window->char_storage, window->char_count + 1u)) {
        window->char_buf = window->char_storage.data;
        window->char_buf[window->char_count++] = codepoint;
    }
    ca_event_post(window->instance, &(Ca_Event){
        .type = CA_EVENT_CHAR, .window = window, .character = {codepoint}});
}

void ca_window_input_button(Ca_Window *window, int button, int action, int mods,
                            double x, double y)
{
    if (!window || !window->in_use) return;
    if (button >= 0 && button < 3)
        window->mouse_buttons[button] = action == CA_PRESS;
    if (button == 0 && action == CA_PRESS) {
        window->mouse_click_this_frame = true;
        window->mouse_press_x = x;
        window->mouse_press_y = y;
    }
    if (button == 1 && action == CA_PRESS) {
        window->right_click_this_frame = true;
        window->right_press_x = x;
        window->right_press_y = y;
    }
    if (button >= 0 && button < 3 && action == CA_RELEASE) {
        int window_x, window_y;
        ca_window_get_rect(window, &window_x, &window_y, NULL, NULL);
        window->mouse_release_x[button] = window_x + x;
        window->mouse_release_y[button] = window_y + y;
        window->mouse_released[button] = true;
    }
    window->mouse_mods = mods;
    ca_event_post(window->instance, &(Ca_Event){
        .type = CA_EVENT_MOUSE_BUTTON, .window = window, .mouse_button = {button, action, mods}});
}

void ca_window_input_move(Ca_Window *window, double x, double y)
{
    if (!window || !window->in_use) return;
    window->mouse_x = x;
    window->mouse_y = y;
    ca_event_post(window->instance, &(Ca_Event){
        .type = CA_EVENT_MOUSE_MOVE, .window = window, .mouse_pos = {x, y}});
}

void ca_window_input_leave(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    window->mouse_x = -1.0;
    window->mouse_y = -1.0;
    ca_instance_wake();
}

void ca_window_input_scroll(Ca_Window *window, double dx, double dy)
{
    if (!window || !window->in_use) return;
    window->scroll_dx += dx;
    window->scroll_dy += dy;
    window->scroll_this_frame = true;
    ca_event_post(window->instance, &(Ca_Event){
        .type = CA_EVENT_MOUSE_SCROLL, .window = window, .mouse_scroll = {dx, dy}});
}

void ca_window_input_unfocus(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    memset(window->mouse_buttons, 0, sizeof(window->mouse_buttons));
    window->prev_mouse_right = false;
    window->focused_node = NULL;
    memset(window->key_consumed, 0, sizeof(window->key_consumed));
    ca_instance_wake();
}

void ca_window_handle_framebuffer_resize(Ca_Window *window, int width, int height)
{
    if (!window || !window->in_use) return;
    window->pending_swapchain_resize = true;
    window->pending_sc_w = width;
    window->pending_sc_h = height;
    if (window->root)
        window->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
}

void ca_window_handle_resize(Ca_Window *window, int width, int height)
{
    if (!window || !window->in_use) return;
    ca_event_post(window->instance, &(Ca_Event){
        .type = CA_EVENT_WINDOW_RESIZE, .window = window, .resize = {width, height}});
    int framebuffer_w, framebuffer_h;
    ca_platform_framebuffer_size(window, &framebuffer_w, &framebuffer_h);
    ca_window_handle_framebuffer_resize(window, framebuffer_w, framebuffer_h);
}

bool ca_window_system_tick(Ca_Instance *inst, bool wait)
{
    /* Clear per-frame click flags before the backend delivers input */
    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use) {
            window->mouse_click_this_frame = false;
            window->right_click_this_frame = false;
            memset(window->mouse_released, 0, sizeof(window->mouse_released));
            window->scroll_dx = 0;
            window->scroll_dy = 0;
            window->scroll_this_frame = false;
            window->char_count = 0;
            window->key_count  = 0;
            memset(window->key_consumed, 0, sizeof(window->key_consumed));
        }
    }

    ca_profile_begin(inst, "Platform Event Wait");
    double timeout = 0.0;
    if (wait && !inst->continuous) {
        timeout = inst->frame_deadline_pending
            ? fmax(0.0, inst->frame_deadline - ca_platform_time())
            : -1.0;
    }
    ca_platform_process_events(inst, timeout);
    if (inst->frame_deadline_pending && ca_platform_time() >= inst->frame_deadline)
        inst->frame_deadline_pending = false;
    ca_profile_end(inst, "Platform Event Wait");

    /* Dispatch all queued input / resize events */
    ca_profile_begin(inst, "Platform Event Dispatch");
    ca_event_dispatch(inst);
    ca_profile_end(inst, "Platform Event Dispatch");

    /* Fire WINDOW_CLOSE event then destroy — order matters.
       Track whether we destroyed anything so we can re-focus. */
    ca_profile_begin(inst, "Platform Window Close");
    bool destroyed_any = false;
    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use && window->close_requested) {
            if (window->on_close_request &&
                !window->on_close_request(window, window->on_close_data)) {
                window->close_requested = false;
                continue;
            }
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
            if (window->in_use) {
                ca_platform_window_focus(window);
                break;
            }
        }
    }
    ca_profile_end(inst, "Platform Window Close");

    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use) return true;
    }
    return false;
}

/** Allocates a logical window, attaches its platform handle, and initializes rendering and UI. */
Ca_Window *ca_window_create_hosted(Ca_Instance *inst, const Ca_WindowDesc *desc, void *host_surface)
{
    if (!inst || !desc) return NULL;
    Ca_Window *slot = ca_pool_acquire(&inst->windows);
    if (!slot) return NULL;

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->title, sizeof(slot->title), "%s", desc->title ? desc->title : "");
    slot->instance      = inst;
    slot->in_use        = true;
    slot->native_frame  = desc->native_frame || desc->fullscreen;
    slot->title_left_aligned = desc->title_left_aligned;
    slot->on_close      = desc->on_close;
    slot->on_close_data = desc->on_close_data;
    slot->on_close_request = desc->on_close_request;
    slot->on_title_drag = desc->on_title_drag;
    slot->on_title_drag_data = desc->on_title_drag_data;
    /* Apply the instance-wide default scale if one has been set,
       otherwise fall back to 1.0 (no scaling). */
    slot->ui_scale = (inst->default_ui_scale > 0.0f)
                     ? inst->default_ui_scale : 1.0f;

    if (!ca_platform_window_create(slot, desc, host_surface))
        goto failed;

    if (inst->vk_device != VK_NULL_HANDLE && !ca_renderer_window_init(inst, slot))
        goto failed_renderer;
    if (!ca_ui_window_init(slot))
        goto failed_renderer;

    if (!desc->no_focus) ca_platform_window_focus(slot);
    return slot;

failed_renderer:
    if (inst->vk_device != VK_NULL_HANDLE)
        ca_renderer_window_shutdown(inst, slot);
    ca_platform_window_destroy(slot);
failed:
    slot->in_use = false;
    ca_pool_release(&inst->windows, slot);
    return NULL;
}

/** Create a user window in inst using desc. */
Ca_Window *ca_window_create(Ca_Instance *inst, const Ca_WindowDesc *desc)
{
    assert(inst && desc);
    return ca_window_create_hosted(inst, desc, NULL);
}

Ca_Window *ca_window_create_reserved(Ca_Instance *inst, const Ca_WindowDesc *desc,
                                     int reserved_index)
{
    if (!inst || !desc) return NULL;
    if (reserved_index < 0 || reserved_index >= CA_RESERVED_POPUP_WINDOWS)
        return NULL;
    return ca_window_create_hosted(inst, desc, NULL);
}

/** Notify the owner and release the window's render, UI, and native resources. */
void ca_window_destroy(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    Ca_Instance *instance = window->instance;

    /* Notify the caller before any resources are freed so it can safely null
       out widget pointers it holds into this window's node/widget pools. */
    if (window->on_close)
        window->on_close(window, window->on_close_data);

    /* Renderer shutdown must precede UI shutdown: ca_renderer_window_shutdown
       calls ca_viewport_gpu_retire on viewport_pool entries, which must still
       be alive.  ca_ui_window_shutdown frees viewport_pool, so reversing this
       order would be a use-after-free. */
    if (window->instance && window->instance->vk_device != VK_NULL_HANDLE)
        ca_renderer_window_shutdown(window->instance, window);
    ca_ui_window_shutdown(window);
    ca_platform_window_destroy(window);
    window->in_use = false;
    ca_pool_release(&instance->windows, window);
}

void ca_instance_destroy_other_windows(Ca_Instance *instance, Ca_Window *keep)
{
    if (!instance) return;
    for (size_t i = 0; i < ca_pool_slot_count(&instance->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(instance->windows, Ca_Window, i);
        if (window != keep && window->in_use) ca_window_destroy(window);
    }
}

void ca_window_system_shutdown(Ca_Instance *inst)
{
    for (size_t i = 0; i < ca_pool_slot_count(&inst->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(inst->windows, Ca_Window, i);
        if (window->in_use) ca_window_destroy(window);
    }
}

/** Queue window for close processing at the next frame boundary. */
void ca_window_close(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    window->close_requested = true;
    ca_instance_wake();
}

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

void ca_instance_set_file_drop_handler(Ca_Instance *instance, Ca_FileDropFn fn, void *user_data)
{
    if (!instance) return;
    instance->file_drop_fn   = fn;
    instance->file_drop_data = fn ? user_data : NULL;
}

bool ca_window_mouse_release_position(const Ca_Window *window, int button, float *x, float *y)
{
    if (!window || !window->in_use || button < 0 || button > 2 || !window->mouse_released[button])
        return false;
    if (x) *x = (float)window->mouse_release_x[button];
    if (y) *y = (float)window->mouse_release_y[button];
    return true;
}

int ca_window_mouse_mods(const Ca_Window *window)
{
    return window ? window->mouse_mods : 0;
}

float ca_window_get_pixel_ratio(Ca_Window *window)
{
    if (!window || !window->in_use) return 1.0f;
    int logical_w = 0, logical_h = 0;
    int framebuffer_w = 0, framebuffer_h = 0;
    ca_platform_window_size(window, &logical_w, &logical_h);
    ca_platform_framebuffer_size(window, &framebuffer_w, &framebuffer_h);
    if (logical_w > 0 && framebuffer_w > 0)
        return (float)framebuffer_w / (float)logical_w;
    if (logical_h > 0 && framebuffer_h > 0)
        return (float)framebuffer_h / (float)logical_h;
    return 1.0f;
}

void ca_window_set_canvas_color(Ca_Window *window, uint32_t color)
{
    if (!window || !window->in_use || !window->root)
        return;
    Ca_Node *root = window->root;
    root->desc.background = color;
    if (root->has_base_desc)
        root->base_desc.background = color;
    root->dirty |= CA_DIRTY_CONTENT;
    window->needs_render = true;
    ca_instance_wake();
}

float ca_window_get_title_bar_height(Ca_Window *window)
{
    if (!window || !window->title_bar_node || window->native_frame || window->titlebar_hidden) return 0.0f;
    return window->title_bar_node->desc.height;
}

void ca_window_set_bg_render(Ca_Window *window, Ca_BgRenderFn fn, void *user_data)
{
    if (!window) return;
    window->bg_render_fn   = fn;
    window->bg_render_data = user_data;
}

/*
 * Request that the registered background render callback run on the next
 * frame for this window.
 *
 * The caller (whatever owns the background content — an animated shader
 * effect, a one-shot theme/settings change, etc.) is responsible for calling
 * this only when the background actually needs to redraw, so an event that
 * merely wakes the loop for an unrelated reason (pointer motion, a keypress,
 * a scroll) does not also re-run the background render and any GPU work
 * chained after it.
 *
 * window  Target window; no-op if NULL or not open.
 */
void ca_window_request_bg_render(Ca_Window *window)
{
    if (!window || !window->in_use) return;
    window->needs_render = true;
    ca_instance_wake();
}

/*
 * Request a background render on the next frame for every open window on
 * the instance that has a background render source (its own per-window
 * callback, or the instance-wide default).
 *
 * Convenience for callers whose background content is instance-wide (e.g.
 * one animated effect shared by the primary window and every auxiliary
 * window via ca_instance_set_bg_render) so they don't need to track and
 * iterate windows themselves. Same "only when it actually needs to redraw"
 * contract as ca_window_request_bg_render.
 *
 * instance  Instance whose windows should render their background; no-op if NULL.
 */
void ca_instance_request_bg_render(Ca_Instance *instance)
{
    if (!instance) return;
    for (size_t i = 0; i < ca_pool_slot_count(&instance->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(instance->windows, Ca_Window, i);
        if (window->in_use && (window->bg_render_fn || instance->default_bg_render_fn))
            window->needs_render = true;
    }
    ca_instance_wake();
}
