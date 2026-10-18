// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "widget.h"
#include "scrollbar.h"
#include "paint.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
    Ca_Window *source;
    uint64_t serial;
} DragCapture;

/** Release win's preview before drop callbacks or source teardown. */
void ca_drag_preview_clear(Ca_Window *win)
{
    ++win->user_drag_serial;
    win->user_drag_preview_attempted = false;
    Ca_Window *preview = win->user_drag_preview;
    win->user_drag_preview = NULL;
    if (preview) ca_window_destroy(preview);
    if (win->user_drag_image) ca_image_destroy(win->instance, win->user_drag_image);
    win->user_drag_image = NULL;
}

/** Freeze the isolated preview render; reject cancelled or superseded drags. */
static void captured(const uint8_t *rgba, uint32_t width, uint32_t height, void *data)
{
    DragCapture *capture = data;
    Ca_Window *win = capture->source;
    win->user_drag_capture = false;
    if (rgba && capture->serial == win->user_drag_serial && win->user_drag_active && win->user_drag_node) {
        win->user_drag_image = ca_image_create(win->instance, rgba, (int)width, (int)height);
        if (win->user_drag_image) ca_instance_request_frame_after(win->instance, 0);
    }
    free(capture);
}

/** Follow the screen cursor with an input-transparent snapshot after the drag threshold. */
void ca_drag_preview_update(Ca_Window *win)
{
    Ca_Node *node = win->user_drag_node;
    if (!node || !node->drag_preview) return;
    if (!win->user_drag_preview_attempted && hypotf((float)win->mouse_x - win->user_drag_start_x,
               (float)win->mouse_y - win->user_drag_start_y) < 5.0f * win->ui_scale) return;
    if (!win->user_drag_preview_attempted) {
        float x = fmaxf(0, node->x), y = fmaxf(0, node->y);
        float right = fminf(win->root->w, node->x + node->w);
        float bottom = fminf(win->root->h, node->y + node->h);
        for (Ca_Node *parent = node->parent; parent; parent = parent->parent) {
            if (!parent->desc.overflow_x && !parent->desc.overflow_y) continue;
            x = fmaxf(x, parent->x); y = fmaxf(y, parent->y);
            right = fminf(right, parent->x + ca_scrollbar_viewport_width(parent));
            bottom = fminf(bottom, parent->y + ca_scrollbar_viewport_height(parent));
        }
        float width = right - x, height = bottom - y;
        if (width <= 0 || height <= 0) return;
        float scale = fminf(1.0f, fminf(240.0f * win->ui_scale / width, 160.0f * win->ui_scale / height));
        int w = (int)fmaxf(1, width * scale), h = (int)fmaxf(1, height * scale);
        Ca_Window *preview = ca_window_create(win->instance, &(Ca_WindowDesc){
            .title = "Drag preview", .width = w, .height = h, .no_focus = true});
        if (!preview) return;
        glfwSetWindowAttrib(preview->glfw, GLFW_FLOATING, GLFW_TRUE);
        glfwSetWindowAttrib(preview->glfw, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
        ca_window_set_title_bar_visible(preview, false);
        ca_window_set_opacity(preview, 0.85f);
        ca_ui_begin(preview, &(Ca_DivDesc){.width = w / win->ui_scale, .height = h / win->ui_scale});
        ca_ui_end();
        if (!ca_paint_snapshot(preview, node, x, y, scale)) { ca_window_destroy(preview); return; }
        win->user_drag_preview = preview;
        win->user_drag_preview_attempted = true;
        DragCapture *capture = malloc(sizeof(*capture));
        if (capture) {
            *capture = (DragCapture){win, win->user_drag_serial};
            if (ca_window_capture(preview, captured, capture)) win->user_drag_capture = true;
            else free(capture);
        }
    }
    if (win->user_drag_capture) ca_instance_request_frame_after(win->instance, 1.0 / 120.0);
    Ca_Window *preview = win->user_drag_preview;
    if (win->user_drag_image && preview->paint_snapshot) {
        int w, h;
        ca_window_get_rect(preview, NULL, NULL, &w, &h);
        preview->paint_snapshot = false;
        ca_ui_begin(preview, &(Ca_DivDesc){.width = w / win->ui_scale, .height = h / win->ui_scale});
        ca_image(&(Ca_ImageDesc){.image = win->user_drag_image, .width = w / win->ui_scale, .height = h / win->ui_scale});
        ca_ui_end();
    }
    float x, y;
    if (ca_window_cursor_screen_position(win, &x, &y))
        ca_window_set_position(preview, (int)(x + 14), (int)(y + 18));
}
