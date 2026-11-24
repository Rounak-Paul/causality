// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/*
 * ca_webview.h — embedded platform-native web view widget.
 *
 * A web view is a layout node whose content is drawn by the operating
 * system's own web engine, hosted as a native child view of the window
 * (WKWebView on macOS). It is not rendered through Vulkan, so:
 *
 *   - it always paints above Causality content inside its rectangle;
 *     popups, menus and tooltips cannot overlap it. While a modal is
 *     visible, or the F9 debug overlay is on, web views are hidden so
 *     Causality's own UI can show.
 *   - it receives pointer, wheel and keyboard input directly from the OS
 *     while it has focus; Causality widgets beneath it see nothing.
 *   - its rectangle follows the node's layout, clipped by every ancestor
 *     with overflow other than visible, and is hidden with its ancestors.
 *
 * All functions must be called on the UI thread. Callbacks run on the UI
 * thread, and may be invoked from inside the platform event pump: they must
 * not destroy windows. A web view is destroyed with its node.
 *
 * Usage:
 *
 *     static void on_event(Ca_WebView *view, const Ca_WebViewEvent *event, void *data) {
 *         if (event->type == CA_WEBVIEW_EVENT_TITLE_CHANGED)
 *             printf("title: %s\n", event->title);
 *     }
 *
 *     ca_webview(&(Ca_WebViewDesc){
 *         .url      = "https://example.com",
 *         .on_event = on_event,
 *     });
 *
 * JavaScript reaches the host through window.causality.postMessage(value)
 * (strings pass through, other values are JSON encoded) and the host reaches
 * JavaScript through ca_webview_post_message, delivered as a "causality"
 * CustomEvent on window whose detail is the message string.
 */

#pragma once

#include "causality.h"

typedef struct Ca_WebView Ca_WebView;

/* Kinds of Ca_WebViewEvent. */
typedef enum Ca_WebViewEventType {
    CA_WEBVIEW_EVENT_LOAD_STARTED,       /* navigation began                               */
    CA_WEBVIEW_EVENT_LOAD_FINISHED,      /* main frame finished loading                    */
    CA_WEBVIEW_EVENT_LOAD_FAILED,        /* detail holds the error description             */
    CA_WEBVIEW_EVENT_URL_CHANGED,        /* url is the new address                         */
    CA_WEBVIEW_EVENT_TITLE_CHANGED,      /* title is the new document title                */
    CA_WEBVIEW_EVENT_PROGRESS_CHANGED,   /* progress is in [0, 1]                          */
    CA_WEBVIEW_EVENT_HISTORY_CHANGED,    /* can_go_back / can_go_forward changed           */
    CA_WEBVIEW_EVENT_MESSAGE,            /* detail holds a window.causality.postMessage    */
    CA_WEBVIEW_EVENT_NEW_WINDOW,         /* page asked for a new window; detail is its url */
    CA_WEBVIEW_EVENT_PROCESS_TERMINATED, /* web content process died; page is blank        */
} Ca_WebViewEventType;

/* State snapshot delivered with every event. Strings are borrowed for the
 * duration of the callback; copy anything that must outlive it. */
typedef struct Ca_WebViewEvent {
    Ca_WebViewEventType type;
    const char *url;             /* current address, "" when none                  */
    const char *title;           /* current document title, "" when none           */
    const char *detail;          /* message, error or new-window url; else ""      */
    double      progress;        /* load progress in [0, 1]                        */
    bool        loading;
    bool        can_go_back;
    bool        can_go_forward;
} Ca_WebViewEvent;

/* Observes view state changes, loads, and messages posted by the page. */
typedef void (*Ca_WebViewEventFn)(Ca_WebView *view, const Ca_WebViewEvent *event,
                                  void *user_data);

/* Navigation policy, asked before each main-frame navigation commits.
 * Return false to cancel. Runs synchronously inside the web engine. */
typedef bool (*Ca_WebViewNavigateFn)(Ca_WebView *view, const char *url, void *user_data);

/* Completes ca_webview_eval. json is the JSON encoding of the script's
 * completion value ("null" when undefined) when ok, else an error message.
 * Never invoked if the view is destroyed first. */
typedef void (*Ca_WebViewEvalFn)(Ca_WebView *view, bool ok, const char *json,
                                 void *user_data);

typedef struct Ca_WebViewDesc {
    float                width, height;  /* display size (0 = fill available space)          */
    const char          *url;            /* initial address; ignored when html is set        */
    const char          *html;           /* initial document, loaded relative to base_url    */
    const char          *base_url;       /* base for html and its relative links (optional)  */
    const char          *user_agent;     /* custom user agent (optional)                     */
    const char          *init_script;    /* JavaScript run at document start in every frame  */
    Ca_WebViewEventFn    on_event;       /* optional                                         */
    Ca_WebViewNavigateFn on_navigate;    /* optional; null allows every navigation           */
    void                *user_data;      /* passed to every callback                         */
    bool                 transparent;    /* let the page background show Causality through   */
    bool                 devtools;       /* enable the inspector (context menu)              */
    const char          *id, *style;
} Ca_WebViewDesc;

/* Reports whether this platform build embeds a native web view. When false,
 * ca_webview returns NULL and every other function is a no-op. */
CA_API bool ca_webview_supported(void);

/*
 * Create a web view in the current UI tree. A retained node keeps its page
 * across rebuilds; only the callbacks are refreshed, so rebuilding does not
 * navigate. Use the load functions to change the page.
 *
 * desc     Initial content, behaviour and callbacks.
 * Returns  The view, or NULL when unsupported, out of memory, or the native
 *          engine failed to start.
 */
CA_API Ca_WebView *ca_webview(const Ca_WebViewDesc *desc);

/* Replace the event, navigation and user data callbacks on an existing view. */
CA_API void ca_webview_set_callbacks(Ca_WebView *view, Ca_WebViewEventFn on_event,
                                     Ca_WebViewNavigateFn on_navigate, void *user_data);

/* Navigate to an absolute URL. Returns false when url is NULL or malformed. */
CA_API bool ca_webview_load_url(Ca_WebView *view, const char *url);

/* Load an HTML document. base_url may be NULL. Returns false on a NULL html. */
CA_API bool ca_webview_load_html(Ca_WebView *view, const char *html, const char *base_url);

/* Reload the current page, stop a load in progress, or move through history. */
CA_API void ca_webview_reload(Ca_WebView *view);
CA_API void ca_webview_stop(Ca_WebView *view);
CA_API void ca_webview_go_back(Ca_WebView *view);
CA_API void ca_webview_go_forward(Ca_WebView *view);

/* Current state as of the last delivered event. NULL view reads as empty/false.
 * Returned strings stay valid until the next event or the view's destruction. */
CA_API const char *ca_webview_url(const Ca_WebView *view);
CA_API const char *ca_webview_title(const Ca_WebView *view);
CA_API double      ca_webview_progress(const Ca_WebView *view);
CA_API bool        ca_webview_is_loading(const Ca_WebView *view);
CA_API bool        ca_webview_can_go_back(const Ca_WebView *view);
CA_API bool        ca_webview_can_go_forward(const Ca_WebView *view);

/*
 * Run JavaScript in the page's main frame.
 *
 * script     Source text.
 * on_result  Optional completion; see Ca_WebViewEvalFn.
 * Returns    false when the script could not be submitted.
 */
CA_API bool ca_webview_eval(Ca_WebView *view, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data);

/* Deliver message to the page as a "causality" CustomEvent on window. */
CA_API bool ca_webview_post_message(Ca_WebView *view, const char *message);

/* Page zoom factor; clamped to [0.25, 5]. */
CA_API void ca_webview_set_zoom(Ca_WebView *view, double factor);

/* Give or take keyboard focus. Focus returns to Causality automatically when
 * the user presses the pointer on any other part of the window. */
CA_API void ca_webview_focus(Ca_WebView *view);
CA_API void ca_webview_blur(Ca_WebView *view);
CA_API bool ca_webview_has_focus(const Ca_WebView *view);
