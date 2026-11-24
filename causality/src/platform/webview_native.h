// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_native.h — contract between the web view widget and a platform's native web engine.
   One backend implementation is linked per platform. This header is valid C and C++ and pulls in
   only the public API, so backends written in C++ (WebView2) or Objective-C can include it. */
#pragma once

#include "ca_webview.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque native web view owned by one Ca_WebView. */
typedef struct Ca_WebViewNative Ca_WebViewNative;

/** Where a native view sits, in logical window points, top-left origin. */
typedef struct Ca_WebViewFrame {
    float x, y, w, h;                     /* full page rectangle                    */
    float clip_x, clip_y, clip_w, clip_h; /* visible part; subset of the rectangle  */
    float corner_radius;                  /* applied only when nothing is clipped   */
    bool  visible;
} Ca_WebViewFrame;

/** Live engine state the backend reports with every event. */
typedef struct Ca_WebViewState {
    const char *url;
    const char *title;
    double      progress;
    bool        loading;
    bool        can_go_back;
    bool        can_go_forward;
} Ca_WebViewState;

/** Reports whether this build links a native engine that can run on this machine right now. */
bool ca_webview_native_supported(void);

/**
 * Give the engine a chance to run its own event processing. Called once per UI tick for every
 * window that hosts a web view, on the UI thread, before frames are synced. Engines that run on
 * the platform's message loop leave this empty; engines with a private loop (GTK) iterate it here
 * without blocking, and ask the instance for another frame while work is pending.
 */
void ca_webview_native_pump(Ca_Window *window);

/**
 * Create a hidden native web view attached to window.
 *
 * @param window Host window; must have a live platform handle.
 * @param owner  Receives host callbacks until ca_webview_native_destroy.
 * @param desc   Configuration; load content is applied by the caller.
 * @return The native view, or NULL on failure.
 */
Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc);

/** Detach from the window and release the engine. After return no host callback fires. */
void ca_webview_native_destroy(Ca_WebViewNative *native);

/** Position, clip and show or hide the native view. */
void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame);

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url);
bool ca_webview_native_load_html(Ca_WebViewNative *native, const char *html, const char *base_url);
void ca_webview_native_reload(Ca_WebViewNative *native);
void ca_webview_native_stop(Ca_WebViewNative *native);
void ca_webview_native_go_back(Ca_WebViewNative *native);
void ca_webview_native_go_forward(Ca_WebViewNative *native);
void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor);

/** Run script; on completion the backend calls ca_webview_host_eval_done unless destroyed first. */
bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data);

void ca_webview_native_focus(Ca_WebViewNative *native);
void ca_webview_native_blur(Ca_WebViewNative *native);
bool ca_webview_native_has_focus(const Ca_WebViewNative *native);

/* ---- Implemented by src/ui/webview.c; called by backends on the UI thread ---- */

/** Record state and notify the user's event callback. detail may be NULL. */
void ca_webview_host_event(Ca_WebView *owner, Ca_WebViewEventType type,
                           const Ca_WebViewState *state, const char *detail);

/** Ask the user's navigation policy whether url may load. */
bool ca_webview_host_allow_navigation(Ca_WebView *owner, const char *url);

/** Deliver an eval completion to its caller. */
void ca_webview_host_eval_done(Ca_WebView *owner, Ca_WebViewEvalFn fn, void *user_data,
                               bool ok, const char *json);

/**
 * Forward a key the host window must handle itself (the F9 debug overlay) from a focused web view.
 * Engines swallow keyboard input, so each backend detects the key and calls this.
 *
 * @param key    CA_KEY_* value.
 * @param action CA_PRESS or CA_RELEASE.
 */
void ca_webview_host_key(Ca_WebView *owner, int key, int action);

/* ---- Native handles of the window a backend attaches to (src/platform/native_handles.c) ---- */

/**
 * The window's native surface: HWND on Windows, the X11 Window id (as an integer) on Linux.
 * Returns NULL when the window has no native handle.
 */
void *ca_platform_native_window(const Ca_Window *window);

/** The X11 Display* shared with the windowing layer on Linux; NULL elsewhere. */
void *ca_platform_native_display(void);

#ifdef __cplusplus
}
#endif
