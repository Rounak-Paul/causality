// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "widget.h"
#include "scrollbar.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Ca_Window *source;
    uint64_t serial;
    float x, y, width, height;
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

/** Crop a completed source frame; ignore cancelled or superseded drags. */
static void captured(const uint8_t *rgba, uint32_t width, uint32_t height, void *data)
{
    DragCapture *capture = data;
    Ca_Window *win = capture->source;
    win->user_drag_capture = false;
    if (!rgba || capture->serial != win->user_drag_serial || !win->user_drag_active || !win->user_drag_node) {
        free(capture);
        return;
    }
    int ww, wh;
    ca_window_get_rect(win, NULL, NULL, &ww, &wh);
    if (ww <= 0 || wh <= 0) { free(capture); return; }
    float sx = (float)width / ww, sy = (float)height / wh;
    int x = (int)fmaxf(0, floorf(capture->x * sx));
    int y = (int)fmaxf(0, floorf(capture->y * sy));
    int right = (int)fminf(width, ceilf((capture->x + capture->width) * sx));
    int bottom = (int)fminf(height, ceilf((capture->y + capture->height) * sy));
    int w = right - x, h = bottom - y;
    if (w > 0 && h > 0 && (size_t)w <= SIZE_MAX / 4 / (size_t)h) {
        uint8_t *pixels = malloc((size_t)w * h * 4);
        if (pixels) {
            for (int row = 0; row < h; ++row)
                memcpy(pixels + (size_t)row * w * 4, rgba + ((size_t)(y + row) * width + x) * 4, (size_t)w * 4);
            win->user_drag_image = ca_image_create(win->instance, pixels, w, h);
            free(pixels);
        }
    }
    if (win->user_drag_image) ca_instance_request_frame_after(win->instance, 0);
    free(capture);
}

/** Follow the screen cursor with an input-transparent snapshot after the drag threshold. */
void ca_drag_preview_update(Ca_Window *win)
{
    Ca_Node *node = win->user_drag_node;
    if (!node || !node->drag_preview) return;
    if (!win->user_drag_preview_attempted && hypotf((float)win->mouse_x - win->user_drag_start_x,
               (float)win->mouse_y - win->user_drag_start_y) < 5.0f * win->ui_scale) return;
    if (!win->user_drag_preview_attempted && !win->user_drag_capture) {
        DragCapture *capture = malloc(sizeof(*capture));
        if (!capture) { win->user_drag_preview_attempted = true; return; }
        float x = fmaxf(0, node->x), y = fmaxf(0, node->y);
        float right = fminf(win->root->w, node->x + node->w);
        float bottom = fminf(win->root->h, node->y + node->h);
        for (Ca_Node *parent = node->parent; parent; parent = parent->parent) {
            if (!parent->desc.overflow_x && !parent->desc.overflow_y) continue;
            x = fmaxf(x, parent->x); y = fmaxf(y, parent->y);
            right = fminf(right, parent->x + ca_scrollbar_viewport_width(parent));
            bottom = fminf(bottom, parent->y + ca_scrollbar_viewport_height(parent));
        }
        *capture = (DragCapture){win, win->user_drag_serial, x, y, right - x, bottom - y};
        if (right <= x || bottom <= y || !ca_window_capture(win, captured, capture)) { free(capture); return; }
        win->user_drag_preview_width = right - x;
        win->user_drag_preview_height = bottom - y;
        win->user_drag_capture = true;
        win->user_drag_preview_attempted = true;
    }
    if (win->user_drag_capture) ca_instance_request_frame_after(win->instance, 1.0 / 120.0);
    float x, y;
    if (!ca_window_cursor_screen_position(win, &x, &y)) return;
    if (win->user_drag_image && !win->user_drag_preview) {
        float width = win->user_drag_preview_width, height = win->user_drag_preview_height;
        float scale = fminf(1.0f, fminf(240.0f * win->ui_scale / width, 160.0f * win->ui_scale / height));
        int w = (int)fmaxf(1, width * scale), h = (int)fmaxf(1, height * scale);
        Ca_Window *preview = ca_window_create(win->instance, &(Ca_WindowDesc){
            .title = "Drag preview", .width = w, .height = h, .no_focus = true});
        if (!preview) return;
        win->user_drag_preview = preview;
        glfwSetWindowAttrib(preview->glfw, GLFW_FLOATING, GLFW_TRUE);
        glfwSetWindowAttrib(preview->glfw, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
        ca_window_set_title_bar_visible(preview, false);
        ca_window_set_opacity(preview, 0.85f);
        ca_ui_begin(preview, &(Ca_DivDesc){.width = w / win->ui_scale, .height = h / win->ui_scale});
        ca_image(&(Ca_ImageDesc){.image = win->user_drag_image, .width = w / win->ui_scale, .height = h / win->ui_scale});
        ca_ui_end();
    }
    if (win->user_drag_preview)
        ca_window_set_position(win->user_drag_preview, (int)(x + 14), (int)(y + 18));
}
