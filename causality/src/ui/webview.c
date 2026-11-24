// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview.c — web view widget: node geometry sync, state cache and public API.
   The web engine itself lives behind webview_backend.h. */
#include "webview.h"
#include "scrollbar.h"
#include "widget.h"
#include "../platform/webview_backend.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define CA_WEBVIEW_ZOOM_MIN 0.25
#define CA_WEBVIEW_ZOOM_MAX 5.0
#define CA_WEBVIEW_FRAME_EPSILON 0.01f
#define CA_WEBVIEW_MIN_VISIBLE_EXTENT 0.5f

/** Copy src into *dst, treating NULL as empty. Keeps the old string when allocation fails. */
static void replace_string(char **dst, const char *src)
{
    const char *text = src ? src : "";
    size_t length = strlen(text) + 1u;
    char *copy = CA_MALLOC(length);
    if (!copy) return;
    memcpy(copy, text, length);
    CA_FREE(*dst);
    *dst = copy;
}

/** Returns text, or the empty string for NULL. */
static const char *text_or_empty(const char *text)
{
    return text ? text : "";
}

/** Reports whether a frame differs visibly from the one last applied. */
static bool frame_changed(const Ca_WebViewFrame *a, const Ca_WebViewFrame *b)
{
    const float *fa = &a->x;
    const float *fb = &b->x;
    for (int i = 0; i < 9; ++i)
        if (fabsf(fa[i] - fb[i]) > CA_WEBVIEW_FRAME_EPSILON) return true;
    return a->visible != b->visible;
}

/** Reports whether node lies inside root's subtree. */
static bool node_within(const Ca_Node *node, const Ca_Node *root)
{
    for (const Ca_Node *cur = node; cur; cur = cur->parent)
        if (cur == root) return true;
    return false;
}

/** Reports whether a visible modal exists that does not contain node. */
static bool blocked_by_modal(Ca_Window *window, const Ca_Node *node)
{
    bool modal_open = false;
    for (size_t i = 0; i < ca_pool_slot_count(&window->modal_pool); ++i) {
        if (!ca_pool_slot_live(&window->modal_pool, i)) continue;
        const Ca_Modal *modal = CA_POOL_AT(window->modal_pool, Ca_Modal, i);
        if (!modal->in_use || !modal->visible || !modal->node) continue;
        if (node_within(node, modal->node)) return false;
        modal_open = true;
    }
    return modal_open;
}

/** Intersect the clip rectangle in frame with the given rectangle. */
static void intersect_clip(Ca_WebViewFrame *frame, float x, float y, float w, float h)
{
    float left   = fmaxf(frame->clip_x, x);
    float top    = fmaxf(frame->clip_y, y);
    float right  = fminf(frame->clip_x + frame->clip_w, x + w);
    float bottom = fminf(frame->clip_y + frame->clip_h, y + h);
    frame->clip_x = left;
    frame->clip_y = top;
    frame->clip_w = fmaxf(0.0f, right - left);
    frame->clip_h = fmaxf(0.0f, bottom - top);
}

/** Resolve a view's page rectangle, visible clip and visibility from its node chain. */
static Ca_WebViewFrame resolve_frame(const Ca_WebView *view)
{
    const Ca_Node *node = view->node;
    Ca_WebViewFrame frame = {
        .x = node->x, .y = node->y, .w = node->w, .h = node->h,
        .clip_x = node->x, .clip_y = node->y, .clip_w = node->w, .clip_h = node->h,
        .corner_radius = node->desc.corner_radius,
        .visible = true,
    };
    for (const Ca_Node *cur = node; cur; cur = cur->parent) {
        if (cur->desc.hidden) frame.visible = false;
        if (cur != node && (cur->desc.overflow_x >= 1 || cur->desc.overflow_y >= 1))
            intersect_clip(&frame, cur->x, cur->y,
                           ca_scrollbar_viewport_width(cur),
                           ca_scrollbar_viewport_height(cur));
    }
    bool finite = isfinite(frame.x) && isfinite(frame.y) && isfinite(frame.w) &&
                  isfinite(frame.h) && isfinite(frame.clip_x) && isfinite(frame.clip_y) &&
                  isfinite(frame.clip_w) && isfinite(frame.clip_h);
    if (!finite || frame.w < CA_WEBVIEW_MIN_VISIBLE_EXTENT ||
        frame.h < CA_WEBVIEW_MIN_VISIBLE_EXTENT ||
        frame.clip_w < CA_WEBVIEW_MIN_VISIBLE_EXTENT ||
        frame.clip_h < CA_WEBVIEW_MIN_VISIBLE_EXTENT ||
        blocked_by_modal(node->window, node) || node->window->debug_overlay)
        frame.visible = false;
    return frame;
}

/** Reports whether (x, y) lies inside the frame's visible clip. */
static bool frame_contains(const Ca_WebViewFrame *frame, double x, double y)
{
    return x >= frame->clip_x && x < frame->clip_x + frame->clip_w &&
           y >= frame->clip_y && y < frame->clip_y + frame->clip_h;
}

/** Release a view's native engine and heap state, leaving its slot acquired. */
static void detach(Ca_WebView *view)
{
    if (view->native) {
        ca_webview_native_destroy(view->native);
        view->native = NULL;
        if (view->window && view->window->webview_count > 0u)
            --view->window->webview_count;
    }
    CA_FREE(view->url);
    CA_FREE(view->title);
    view->url = NULL;
    view->title = NULL;
}

/** Drop Causality's own keyboard focus, since the OS now routes keys to a web view. */
static void release_widget_focus(Ca_Window *window)
{
    Ca_Node *previous = window->focused_node;
    if (!previous) return;
    window->focused_node = NULL;
    for (Ca_Node *node = previous; node; node = node->parent)
        ca_widget_reapply_css(node);
}

bool ca_webview_attach(Ca_WebView *view, Ca_Node *node, const Ca_WebViewDesc *desc)
{
    Ca_Window *window = node->window;
    view->node        = node;
    view->window      = window;
    view->on_event    = desc->on_event;
    view->on_navigate = desc->on_navigate;
    view->user_data   = desc->user_data;
    replace_string(&view->url, "");
    replace_string(&view->title, "");
    if (!view->url || !view->title) {
        detach(view);
        return false;
    }

    view->native = ca_webview_native_create(window, view, desc);
    if (!view->native) {
        detach(view);
        return false;
    }
    ++window->webview_count;

    bool queued = true;
    if (desc->html)
        queued = ca_webview_native_load_html(view->native, desc->html, desc->base_url);
    else if (desc->url)
        queued = ca_webview_native_load_url(view->native, desc->url);
    if (!queued)
        ca_webview_host_event(view, CA_WEBVIEW_EVENT_LOAD_FAILED, NULL,
                              "initial content could not be loaded");
    return true;
}

void ca_webview_release(Ca_Window *window, Ca_WebView *view)
{
    if (view->dispatch_depth > 0u) {
        view->release_pending = true;
        view->on_event = NULL;
        view->on_navigate = NULL;
        return;
    }
    detach(view);
    ca_pool_release(&window->webview_pool, view);
}

/** Mark a user callback as running so a removal from inside it is deferred. */
static void dispatch_begin(Ca_WebView *view)
{
    ++view->dispatch_depth;
}

/** Finish a user callback and complete any release it requested. */
static void dispatch_end(Ca_WebView *view)
{
    if (--view->dispatch_depth == 0u && view->release_pending) {
        Ca_Window *window = view->window;
        detach(view);
        ca_pool_release(&window->webview_pool, view);
    }
}

void ca_webview_release_all(Ca_Window *window)
{
    for (size_t i = 0; i < ca_pool_slot_count(&window->webview_pool); ++i) {
        if (!ca_pool_slot_live(&window->webview_pool, i)) continue;
        detach(CA_POOL_AT(window->webview_pool, Ca_WebView, i));
    }
}

void ca_webview_sync_window(Ca_Window *window)
{
    if (window->webview_count == 0u) return;
    for (size_t i = 0; i < ca_pool_slot_count(&window->node_pool); ++i) {
        Ca_Node *node = CA_POOL_AT(window->node_pool, Ca_Node, i);
        if (!node->in_use || node->widget_type != CA_WIDGET_WEBVIEW || !node->widget) continue;
        Ca_WebView *view = node->widget;
        if (!view->native) continue;

        Ca_WebViewFrame frame = resolve_frame(view);
        if (!view->applied_valid || frame_changed(&frame, &view->applied)) {
            ca_webview_native_set_frame(view->native, &frame);
            view->applied = frame;
            view->applied_valid = true;
        }
        if (!ca_webview_native_has_focus(view->native)) continue;
        if (window->mouse_click_this_frame &&
            !(frame.visible && frame_contains(&frame, window->mouse_press_x,
                                              window->mouse_press_y))) {
            ca_webview_native_blur(view->native);
            continue;
        }
        release_widget_focus(window);
    }
}

void ca_webview_host_event(Ca_WebView *owner, Ca_WebViewEventType type,
                           const Ca_WebViewState *state, const char *detail)
{
    if (!owner || owner->release_pending) return;
    if (state) {
        replace_string(&owner->url, state->url);
        replace_string(&owner->title, state->title);
        owner->progress       = state->progress;
        owner->loading        = state->loading;
        owner->can_go_back    = state->can_go_back;
        owner->can_go_forward = state->can_go_forward;
    }
    if (owner->on_event) {
        Ca_WebViewEvent event = {
            .type           = type,
            .url            = text_or_empty(owner->url),
            .title          = text_or_empty(owner->title),
            .detail         = text_or_empty(detail),
            .progress       = owner->progress,
            .loading        = owner->loading,
            .can_go_back    = owner->can_go_back,
            .can_go_forward = owner->can_go_forward,
        };
        dispatch_begin(owner);
        owner->on_event(owner, &event, owner->user_data);
        dispatch_end(owner);
    }
    ca_instance_wake();
}

bool ca_webview_host_allow_navigation(Ca_WebView *owner, const char *url)
{
    if (!owner || owner->release_pending) return false;
    if (!owner->on_navigate) return true;
    dispatch_begin(owner);
    bool allowed = owner->on_navigate(owner, text_or_empty(url), owner->user_data);
    dispatch_end(owner);
    return allowed;
}

void ca_webview_host_eval_done(Ca_WebView *owner, Ca_WebViewEvalFn fn, void *user_data,
                               bool ok, const char *json)
{
    if (!owner || owner->release_pending) return;
    if (fn) {
        dispatch_begin(owner);
        fn(owner, ok, text_or_empty(json), user_data);
        dispatch_end(owner);
    }
    ca_instance_wake();
}

/** Append the JavaScript string literal for UTF-8 text to out and return the new write position. */
static char *append_js_literal(char *out, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    *out++ = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
        case '"':  *out++ = '\\'; *out++ = '"';  break;
        case '\\': *out++ = '\\'; *out++ = '\\'; break;
        case '\n': *out++ = '\\'; *out++ = 'n';  break;
        case '\r': *out++ = '\\'; *out++ = 'r';  break;
        case '\t': *out++ = '\\'; *out++ = 't';  break;
        default:
            if (c < 0x20u) {
                memcpy(out, "\\u00", 4);
                out += 4;
                *out++ = hex[c >> 4];
                *out++ = hex[c & 0xFu];
            } else if (c == 0xE2u && p[1] == 0x80u && (p[2] == 0xA8u || p[2] == 0xA9u)) {
                memcpy(out, p[2] == 0xA8u ? "\\u2028" : "\\u2029", 6);
                out += 6;
                p += 2;
            } else {
                *out++ = (char)c;
            }
        }
    }
    *out++ = '"';
    return out;
}

bool ca_webview_supported(void)
{
    return ca_webview_native_supported();
}

void ca_webview_set_callbacks(Ca_WebView *view, Ca_WebViewEventFn on_event,
                              Ca_WebViewNavigateFn on_navigate, void *user_data)
{
    if (!view) return;
    view->on_event    = on_event;
    view->on_navigate = on_navigate;
    view->user_data   = user_data;
}

bool ca_webview_load_url(Ca_WebView *view, const char *url)
{
    return view && view->native && url && ca_webview_native_load_url(view->native, url);
}

bool ca_webview_load_html(Ca_WebView *view, const char *html, const char *base_url)
{
    return view && view->native && html &&
           ca_webview_native_load_html(view->native, html, base_url);
}

void ca_webview_reload(Ca_WebView *view)
{
    if (view && view->native) ca_webview_native_reload(view->native);
}

void ca_webview_stop(Ca_WebView *view)
{
    if (view && view->native) ca_webview_native_stop(view->native);
}

void ca_webview_go_back(Ca_WebView *view)
{
    if (view && view->native) ca_webview_native_go_back(view->native);
}

void ca_webview_go_forward(Ca_WebView *view)
{
    if (view && view->native) ca_webview_native_go_forward(view->native);
}

const char *ca_webview_url(const Ca_WebView *view)
{
    return view ? text_or_empty(view->url) : "";
}

const char *ca_webview_title(const Ca_WebView *view)
{
    return view ? text_or_empty(view->title) : "";
}

double ca_webview_progress(const Ca_WebView *view)
{
    return view ? view->progress : 0.0;
}

bool ca_webview_is_loading(const Ca_WebView *view)
{
    return view && view->loading;
}

bool ca_webview_can_go_back(const Ca_WebView *view)
{
    return view && view->can_go_back;
}

bool ca_webview_can_go_forward(const Ca_WebView *view)
{
    return view && view->can_go_forward;
}

bool ca_webview_eval(Ca_WebView *view, const char *script,
                     Ca_WebViewEvalFn on_result, void *user_data)
{
    return view && view->native && script &&
           ca_webview_native_eval(view->native, script, on_result, user_data);
}

bool ca_webview_post_message(Ca_WebView *view, const char *message)
{
    static const char prefix[] = "window.dispatchEvent(new CustomEvent('causality',{detail:";
    static const char suffix[] = "}));";
    if (!view || !view->native || !message) return false;

    size_t length = strlen(message);
    if (length > (SIZE_MAX - sizeof(prefix) - sizeof(suffix) - 2u) / 6u) return false;
    size_t capacity = sizeof(prefix) + sizeof(suffix) + 2u + length * 6u;
    char *script = CA_MALLOC(capacity);
    if (!script) return false;

    char *out = script;
    memcpy(out, prefix, sizeof(prefix) - 1u);
    out += sizeof(prefix) - 1u;
    out = append_js_literal(out, message);
    memcpy(out, suffix, sizeof(suffix));
    bool queued = ca_webview_native_eval(view->native, script, NULL, NULL);
    CA_FREE(script);
    return queued;
}

void ca_webview_set_zoom(Ca_WebView *view, double factor)
{
    if (!view || !view->native || !isfinite(factor)) return;
    if (factor < CA_WEBVIEW_ZOOM_MIN) factor = CA_WEBVIEW_ZOOM_MIN;
    if (factor > CA_WEBVIEW_ZOOM_MAX) factor = CA_WEBVIEW_ZOOM_MAX;
    ca_webview_native_set_zoom(view->native, factor);
}

void ca_webview_focus(Ca_WebView *view)
{
    if (view && view->native) ca_webview_native_focus(view->native);
}

void ca_webview_blur(Ca_WebView *view)
{
    if (view && view->native) ca_webview_native_blur(view->native);
}

bool ca_webview_has_focus(const Ca_WebView *view)
{
    return view && view->native && ca_webview_native_has_focus(view->native);
}
