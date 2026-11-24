// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_gtk.c — Linux web view backend (WebKit2GTK 4.1 or 4.0 on X11).
 *
 * Each web view owns two X11 windows created on the windowing layer's shared display:
 *   container  child of the GLFW window, sized to the visible clip rectangle; X clips its children.
 *   inner      child of the container, sized to the full page and offset by the clip origin.
 * A GtkPlug embeds a GTK toplevel holding the WebKitWebView into `inner`, so the page is laid out
 * at its full rectangle while only the clip rectangle is visible.
 *
 * Assumptions: logical window points equal device pixels on X11; the GDK integer scale factor is
 * folded into the plug size only. Corner radius is not supported. The decide-policy API does not
 * tell main-frame from sub-frame navigations, so every navigation action is offered to the host.
 * The bridge script is injected into the top frame only; script messages carry no frame
 * information, so messages are accepted from whichever frame can reach the handler.
 * JavaScript dialogs and file choosers use WebKitGTK's defaults, parented to the plug.
 * On load failure WebKitGTK shows no error page, so failures are reported and the handler
 * consumes the signal.
 */

#include "webview_native.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <webkit2/webkit2.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

#ifdef WEBKIT_CHECK_VERSION
#  if WEBKIT_CHECK_VERSION(2, 40, 0)
#    define CA_WK_EVALUATE_JAVASCRIPT 1
#  endif
#endif

#define CA_GTK_PUMP_LIMIT      64
#define CA_GTK_PUMP_DELAY      0.008
#define CA_GTK_COORD_LIMIT     1000000.0f
#define CA_GTK_MAX_EXTENT      32767
#define CA_GTK_MAX_OFFSET      32767

static const char kBridgeScript[] =
    "Object.defineProperty(window,'causality',{value:Object.freeze({"
    "postMessage:function(m){window.webkit.messageHandlers.causality.postMessage("
    "typeof m==='string'?m:JSON.stringify(m));}}),configurable:false});";

/** Reference-counted flag shared with asynchronous completions that may outlive the view. */
typedef struct Liveness {
    Ca_WebView *owner;
    bool        alive;
    unsigned    refs;
} Liveness;

/** One native web view: X11 host windows, GTK widgets and event bookkeeping. */
struct Ca_WebViewNative {
    Liveness                 *live;
    Ca_WebView               *owner;
    Display                  *display;
    Window                    host;
    Window                    container;
    Window                    inner;
    GtkWidget                *plug;
    WebKitWebView            *web;
    WebKitUserContentManager *content;
    GCancellable             *cancellable;
    int                       gtk_width;
    int                       gtk_height;
    bool                      visible;
    bool                      focused;
    bool                      load_failed;
    bool                      can_go_back;
    bool                      can_go_forward;
};

/** Context of one pending script evaluation. */
typedef struct EvalRequest {
    Liveness         *live;
    Ca_WebViewEvalFn  fn;
    void             *user_data;
} EvalRequest;

static size_t     g_live_views;
static bool       g_x_failed;

/** Take a reference on a liveness token. */
static Liveness *liveness_ref(Liveness *live)
{
    ++live->refs;
    return live;
}

/** Drop a reference on a liveness token, freeing it with the last one. */
static void liveness_unref(Liveness *live)
{
    if (--live->refs == 0u) g_free(live);
}

/** Initialise GTK once for the X11 backend. Returns true when the engine can run. */
static bool gtk_ready(void)
{
    static int state;
    if (state != 0) return state == 1;
    if (!ca_platform_native_display()) return false;
    state = 2;
    gdk_set_allowed_backends("x11");
    if (!gtk_init_check(NULL, NULL)) return false;
    GdkDisplay *display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) return false;
    state = 1;
    return true;
}

/** Xlib error handler that records the failure instead of terminating the process. */
static int x_error_recorder(Display *display, XErrorEvent *event)
{
    (void)display;
    (void)event;
    g_x_failed = true;
    return 0;
}

/** Start catching X errors on display; pass the result to x_trap_end. */
static XErrorHandler x_trap_begin(Display *display)
{
    XSync(display, False);
    g_x_failed = false;
    return XSetErrorHandler(x_error_recorder);
}

/** Stop catching X errors, restoring previous. Returns true when no error occurred. */
static bool x_trap_end(Display *display, XErrorHandler previous)
{
    XSync(display, False);
    XSetErrorHandler(previous);
    return !g_x_failed;
}

/** Clamp value into [low, high]. */
static long clamp_long(long value, long low, long high)
{
    return value < low ? low : (value > high ? high : value);
}

/** Round a logical coordinate to a pixel, bounding runaway values. */
static long to_pixel(float value)
{
    if (value > CA_GTK_COORD_LIMIT) value = CA_GTK_COORD_LIMIT;
    if (value < -CA_GTK_COORD_LIMIT) value = -CA_GTK_COORD_LIMIT;
    return lroundf(value);
}

/** The X11 id of the plug's own window, or 0 when it is not realized. */
static Window plug_xid(const Ca_WebViewNative *n)
{
    GdkWindow *window = n->plug ? gtk_widget_get_window(n->plug) : NULL;
    return window ? gdk_x11_window_get_xid(window) : 0;
}

/** Move X input focus to window. Returns true when the server accepted it. */
static bool x_set_focus(Display *display, Window window)
{
    XErrorHandler previous = x_trap_begin(display);
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    return x_trap_end(display, previous);
}

/** Send an event of type with a fresh state snapshot to the host. detail may be NULL. */
static void emit(Ca_WebViewNative *n, Ca_WebViewEventType type, const char *detail)
{
    Ca_WebViewState state = {
        .url            = webkit_web_view_get_uri(n->web),
        .title          = webkit_web_view_get_title(n->web),
        .progress       = webkit_web_view_get_estimated_load_progress(n->web),
        .loading        = webkit_web_view_is_loading(n->web),
        .can_go_back    = webkit_web_view_can_go_back(n->web),
        .can_go_forward = webkit_web_view_can_go_forward(n->web),
    };
    ca_webview_host_event(n->owner, type, &state, detail);
}

/** Emit HISTORY_CHANGED when back/forward availability differs from the last report. */
static void sync_history(Ca_WebViewNative *n)
{
    bool back = webkit_web_view_can_go_back(n->web);
    bool forward = webkit_web_view_can_go_forward(n->web);
    if (back == n->can_go_back && forward == n->can_go_forward) return;
    n->can_go_back = back;
    n->can_go_forward = forward;
    emit(n, CA_WEBVIEW_EVENT_HISTORY_CHANGED, NULL);
}

/** WebKitWebView::load-changed. Maps load phases to host events. */
static void on_load_changed(WebKitWebView *web, WebKitLoadEvent phase, gpointer data)
{
    (void)web;
    Ca_WebViewNative *n = data;
    switch (phase) {
    case WEBKIT_LOAD_STARTED:
        n->load_failed = false;
        emit(n, CA_WEBVIEW_EVENT_LOAD_STARTED, NULL);
        break;
    case WEBKIT_LOAD_COMMITTED:
        sync_history(n);
        break;
    case WEBKIT_LOAD_FINISHED:
        if (n->load_failed) {
            n->load_failed = false;
            emit(n, CA_WEBVIEW_EVENT_PROGRESS_CHANGED, NULL);
        } else {
            emit(n, CA_WEBVIEW_EVENT_LOAD_FINISHED, NULL);
        }
        sync_history(n);
        break;
    default:
        break;
    }
}

/** WebKitWebView::load-failed. Reports real failures, ignores cancellations and policy stops. */
static gboolean on_load_failed(WebKitWebView *web, WebKitLoadEvent phase, gchar *uri,
                               GError *error, gpointer data)
{
    (void)web;
    (void)phase;
    (void)uri;
    Ca_WebViewNative *n = data;
    n->load_failed = true;
    if (error && (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
                  error->domain == WEBKIT_POLICY_ERROR))
        return TRUE;
    emit(n, CA_WEBVIEW_EVENT_LOAD_FAILED,
         error && error->message ? error->message : "load failed");
    return TRUE;
}

/** GObject notify for title, uri and estimated-load-progress. */
static void on_notify(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object;
    Ca_WebViewNative *n = data;
    const char *name = g_param_spec_get_name(pspec);
    if (strcmp(name, "title") == 0) {
        emit(n, CA_WEBVIEW_EVENT_TITLE_CHANGED, NULL);
    } else if (strcmp(name, "uri") == 0) {
        emit(n, CA_WEBVIEW_EVENT_URL_CHANGED, NULL);
        sync_history(n);
    } else if (strcmp(name, "estimated-load-progress") == 0) {
        emit(n, CA_WEBVIEW_EVENT_PROGRESS_CHANGED, NULL);
    }
}

/** WebKitBackForwardList::changed. */
static void on_history_changed(WebKitBackForwardList *list, WebKitBackForwardListItem *added,
                               gpointer removed, gpointer data)
{
    (void)list;
    (void)added;
    (void)removed;
    sync_history(data);
}

/** WebKitWebView::web-process-terminated. */
static void on_process_terminated(WebKitWebView *web, WebKitWebProcessTerminationReason reason,
                                  gpointer data)
{
    (void)web;
    (void)reason;
    emit(data, CA_WEBVIEW_EVENT_PROCESS_TERMINATED, NULL);
}

/** Address a navigation action targets, never NULL. */
static const char *action_uri(WebKitNavigationAction *action)
{
    WebKitURIRequest *request = action ? webkit_navigation_action_get_request(action) : NULL;
    const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;
    return uri ? uri : "";
}

/** WebKitWebView::create. Reports the request and never opens a window. */
static GtkWidget *on_create(WebKitWebView *web, WebKitNavigationAction *action, gpointer data)
{
    (void)web;
    emit(data, CA_WEBVIEW_EVENT_NEW_WINDOW, action_uri(action));
    return NULL;
}

/** WebKitWebView::decide-policy. Applies the host's navigation policy and reports new windows. */
static gboolean on_decide_policy(WebKitWebView *web, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, gpointer data)
{
    (void)web;
    Ca_WebViewNative *n = data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;

    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char *uri = action_uri(action);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        webkit_policy_decision_ignore(decision);
        emit(n, CA_WEBVIEW_EVENT_NEW_WINDOW, uri);
        return TRUE;
    }
    if (ca_webview_host_allow_navigation(n->owner, uri)) return FALSE;
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

/** UserContentManager::script-message-received::causality. Forwards string messages. */
static void on_script_message(WebKitUserContentManager *manager, WebKitJavascriptResult *result,
                              gpointer data)
{
    (void)manager;
    JSCValue *value = webkit_javascript_result_get_js_value(result);
    if (!value || !jsc_value_is_string(value)) return;
    char *text = jsc_value_to_string(value);
    if (!text) return;
    emit(data, CA_WEBVIEW_EVENT_MESSAGE, text);
    g_free(text);
}

/** Give the plug and page keyboard focus. Does nothing while hidden or unrealized. */
static void claim_focus(Ca_WebViewNative *n)
{
    Window target = plug_xid(n);
    if (!n->visible || !target) return;
    if (!x_set_focus(n->display, target)) return;
    gtk_widget_grab_focus(GTK_WIDGET(n->web));
}

/** Hand X input focus back to the Causality window. */
static void release_focus(Ca_WebViewNative *n)
{
    n->focused = false;
    x_set_focus(n->display, n->host);
}

/** button-press-event. Takes keyboard focus without consuming the click. */
static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;
    (void)event;
    claim_focus(data);
    return FALSE;
}

/** focus-in-event. */
static gboolean on_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{
    (void)widget;
    (void)event;
    ((Ca_WebViewNative *)data)->focused = true;
    return FALSE;
}

/** focus-out-event. */
static gboolean on_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{
    (void)widget;
    (void)event;
    ((Ca_WebViewNative *)data)->focused = false;
    return FALSE;
}

/** key-press-event and key-release-event. Forwards F9 to the host and consumes it. */
static gboolean on_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;
    if (event->keyval != GDK_KEY_F9) return FALSE;
    Ca_WebViewNative *n = data;
    ca_webview_host_key(n->owner, CA_KEY_F9, event->type == GDK_KEY_PRESS ? CA_PRESS : CA_RELEASE);
    return TRUE;
}

/** Convert a script completion value to JSON text; "null" for missing or unserializable values. */
static char *json_of(JSCValue *value)
{
    if (!value || jsc_value_is_undefined(value) || jsc_value_is_null(value))
        return g_strdup("null");
    char *json = jsc_value_to_json(value, 0);
    return json ? json : g_strdup("null");
}

/** Completion of a script evaluation; reports to the host only while the view is alive. */
static void on_eval_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    EvalRequest *request = data;
    WebKitWebView *web = WEBKIT_WEB_VIEW(source);
    GError *error = NULL;
    char *json = NULL;

#ifdef CA_WK_EVALUATE_JAVASCRIPT
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(web, result, &error);
    if (!error) json = json_of(value);
    if (value) g_object_unref(value);
#else
    WebKitJavascriptResult *outcome = webkit_web_view_run_javascript_finish(web, result, &error);
    if (!error) json = json_of(outcome ? webkit_javascript_result_get_js_value(outcome) : NULL);
    if (outcome) webkit_javascript_result_unref(outcome);
#endif

    if (request->live->alive) {
        bool ok = error == NULL;
        const char *text = ok ? json : (error->message ? error->message : "script failed");
        ca_webview_host_eval_done(request->live->owner, request->fn, request->user_data, ok, text);
    }
    g_free(json);
    if (error) g_error_free(error);
    liveness_unref(request->live);
    g_free(request);
}

/** Release everything a native view holds. Safe on partially constructed views. */
static void release_native(Ca_WebViewNative *n)
{
    n->live->alive = false;
    if (n->cancellable) g_cancellable_cancel(n->cancellable);
    if (n->focused && n->visible) release_focus(n);
    if (n->content) {
        g_signal_handlers_disconnect_by_data(n->content, n);
        webkit_user_content_manager_unregister_script_message_handler(n->content, "causality");
    }
    if (n->web) {
        g_signal_handlers_disconnect_by_data(n->web, n);
        WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(n->web);
        if (list) g_signal_handlers_disconnect_by_data(list, n);
    }
    if (n->plug) {
        gtk_widget_destroy(n->plug);
        gdk_display_sync(gdk_display_get_default());
    }
    if (n->container) {
        XErrorHandler previous = x_trap_begin(n->display);
        XDestroyWindow(n->display, n->container);
        x_trap_end(n->display, previous);
    }
    if (n->content) g_object_unref(n->content);
    if (n->cancellable) g_object_unref(n->cancellable);
    liveness_unref(n->live);
    g_free(n);
}

/** Install the bridge and optional init script into the content manager. */
static void install_scripts(WebKitUserContentManager *content, const char *init_script)
{
    WebKitUserScript *bridge = webkit_user_script_new(
        kBridgeScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(content, bridge);
    webkit_user_script_unref(bridge);
    if (!init_script) return;
    WebKitUserScript *init = webkit_user_script_new(
        init_script, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(content, init);
    webkit_user_script_unref(init);
}

/** Apply description-driven settings to the web view. */
static void configure_web(WebKitWebView *web, const Ca_WebViewDesc *desc)
{
    WebKitSettings *settings = webkit_web_view_get_settings(web);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, desc->devtools ? TRUE : FALSE);
    if (desc->user_agent && desc->user_agent[0] && g_utf8_validate(desc->user_agent, -1, NULL))
        webkit_settings_set_user_agent(settings, desc->user_agent);
    if (desc->transparent) {
        GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
        webkit_web_view_set_background_color(web, &clear);
    }
}

/** Connect every engine signal of a freshly created view to its native object. */
static void connect_signals(Ca_WebViewNative *n)
{
    GObject *web = G_OBJECT(n->web);
    g_signal_connect(web, "load-changed", G_CALLBACK(on_load_changed), n);
    g_signal_connect(web, "load-failed", G_CALLBACK(on_load_failed), n);
    g_signal_connect(web, "notify::title", G_CALLBACK(on_notify), n);
    g_signal_connect(web, "notify::uri", G_CALLBACK(on_notify), n);
    g_signal_connect(web, "notify::estimated-load-progress", G_CALLBACK(on_notify), n);
    g_signal_connect(web, "web-process-terminated", G_CALLBACK(on_process_terminated), n);
    g_signal_connect(web, "create", G_CALLBACK(on_create), n);
    g_signal_connect(web, "decide-policy", G_CALLBACK(on_decide_policy), n);
    g_signal_connect(web, "button-press-event", G_CALLBACK(on_button_press), n);
    g_signal_connect(web, "focus-in-event", G_CALLBACK(on_focus_in), n);
    g_signal_connect(web, "focus-out-event", G_CALLBACK(on_focus_out), n);
    g_signal_connect(web, "key-press-event", G_CALLBACK(on_key), n);
    g_signal_connect(web, "key-release-event", G_CALLBACK(on_key), n);
    g_signal_connect(webkit_web_view_get_back_forward_list(n->web), "changed",
                     G_CALLBACK(on_history_changed), n);
    g_signal_connect(n->content, "script-message-received::causality",
                     G_CALLBACK(on_script_message), n);
}

bool ca_webview_native_supported(void)
{
    return gtk_ready();
}

void ca_webview_native_pump(Ca_Window *window)
{
    if (g_live_views == 0u) return;
    for (int i = 0; i < CA_GTK_PUMP_LIMIT && gtk_events_pending(); ++i)
        gtk_main_iteration_do(FALSE);
    ca_instance_request_frame_after(ca_window_instance(window), CA_GTK_PUMP_DELAY);
}

Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc)
{
    if (!window || !owner || !desc || !gtk_ready()) return NULL;
    Display *display = ca_platform_native_display();
    Window host = (Window)(uintptr_t)ca_platform_native_window(window);
    if (!display || !host) return NULL;

    Ca_WebViewNative *n = g_new0(Ca_WebViewNative, 1);
    n->live = g_new0(Liveness, 1);
    n->live->owner = owner;
    n->live->alive = true;
    n->live->refs = 1u;
    n->owner = owner;
    n->display = display;
    n->host = host;
    n->cancellable = g_cancellable_new();

    XErrorHandler previous = x_trap_begin(display);
    n->container = XCreateSimpleWindow(display, host, 0, 0, 1, 1, 0, 0, 0);
    n->inner = n->container ? XCreateSimpleWindow(display, n->container, 0, 0, 1, 1, 0, 0, 0) : 0;
    if (n->inner) XMapWindow(display, n->inner);
    if (!x_trap_end(display, previous) || !n->inner) {
        release_native(n);
        return NULL;
    }

    n->content = webkit_user_content_manager_new();
    if (!webkit_user_content_manager_register_script_message_handler(n->content, "causality")) {
        g_object_unref(n->content);
        n->content = NULL;
        release_native(n);
        return NULL;
    }
    install_scripts(n->content, desc->init_script);

    GtkWidget *web = webkit_web_view_new_with_user_content_manager(n->content);
    g_object_ref_sink(web);
    n->web = WEBKIT_WEB_VIEW(web);
    configure_web(n->web, desc);
    gtk_widget_set_size_request(web, 1, 1);
    n->gtk_width = 1;
    n->gtk_height = 1;

    n->plug = gtk_plug_new(n->inner);
    gtk_window_set_default_size(GTK_WINDOW(n->plug), 1, 1);
    gtk_container_add(GTK_CONTAINER(n->plug), web);
    g_object_unref(web);

    connect_signals(n);
    gtk_widget_show_all(n->plug);
    gdk_display_sync(gdk_display_get_default());
    ++g_live_views;
    return n;
}

void ca_webview_native_destroy(Ca_WebViewNative *native)
{
    if (!native) return;
    if (g_live_views > 0u) --g_live_views;
    release_native(native);
}

/** Resize the embedded GTK toplevel and page to pw by ph device pixels. */
static void resize_plug(Ca_WebViewNative *n, long pw, long ph)
{
    int scale = gtk_widget_get_scale_factor(n->plug);
    if (scale < 1) scale = 1;
    int width = (int)((pw + scale - 1) / scale);
    int height = (int)((ph + scale - 1) / scale);
    if (width == n->gtk_width && height == n->gtk_height) return;
    n->gtk_width = width;
    n->gtk_height = height;
    gtk_widget_set_size_request(GTK_WIDGET(n->web), width, height);
    gtk_window_resize(GTK_WINDOW(n->plug), width, height);
}

/** Unmap the container, returning focus to the Causality window first. */
static void hide_view(Ca_WebViewNative *n)
{
    if (!n->visible) return;
    if (n->focused) release_focus(n);
    XUnmapWindow(n->display, n->container);
    XFlush(n->display);
    n->visible = false;
}

void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame)
{
    if (!native || !frame) return;
    const float values[8] = {frame->x, frame->y, frame->w, frame->h,
                             frame->clip_x, frame->clip_y, frame->clip_w, frame->clip_h};
    bool finite = true;
    for (int i = 0; i < 8; ++i) finite = finite && isfinite(values[i]);
    if (!frame->visible || !finite) {
        hide_view(native);
        return;
    }

    long cx = to_pixel(frame->clip_x);
    long cy = to_pixel(frame->clip_y);
    long cw = clamp_long(to_pixel(frame->clip_x + frame->clip_w) - cx, 1, CA_GTK_MAX_EXTENT);
    long ch = clamp_long(to_pixel(frame->clip_y + frame->clip_h) - cy, 1, CA_GTK_MAX_EXTENT);
    long px = to_pixel(frame->x);
    long py = to_pixel(frame->y);
    long pw = clamp_long(to_pixel(frame->x + frame->w) - px, 1, CA_GTK_MAX_EXTENT);
    long ph = clamp_long(to_pixel(frame->y + frame->h) - py, 1, CA_GTK_MAX_EXTENT);

    XMoveResizeWindow(native->display, native->container,
                      (int)clamp_long(cx, -CA_GTK_MAX_OFFSET, CA_GTK_MAX_OFFSET),
                      (int)clamp_long(cy, -CA_GTK_MAX_OFFSET, CA_GTK_MAX_OFFSET),
                      (unsigned)cw, (unsigned)ch);
    XMoveResizeWindow(native->display, native->inner,
                      (int)clamp_long(px - cx, -CA_GTK_MAX_OFFSET, CA_GTK_MAX_OFFSET),
                      (int)clamp_long(py - cy, -CA_GTK_MAX_OFFSET, CA_GTK_MAX_OFFSET),
                      (unsigned)pw, (unsigned)ph);
    XMapRaised(native->display, native->container);
    XFlush(native->display);
    native->visible = true;
    resize_plug(native, pw, ph);
}

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url)
{
    if (!native || !url || !g_utf8_validate(url, -1, NULL)) return false;
    char *scheme = g_uri_parse_scheme(url);
    if (!scheme) return false;
    g_free(scheme);
    webkit_web_view_load_uri(native->web, url);
    return true;
}

bool ca_webview_native_load_html(Ca_WebViewNative *native, const char *html, const char *base_url)
{
    if (!native || !html || !g_utf8_validate(html, -1, NULL)) return false;
    if (base_url && !g_utf8_validate(base_url, -1, NULL)) return false;
    webkit_web_view_load_html(native->web, html, base_url);
    return true;
}

void ca_webview_native_reload(Ca_WebViewNative *native)
{
    if (native) webkit_web_view_reload(native->web);
}

void ca_webview_native_stop(Ca_WebViewNative *native)
{
    if (native) webkit_web_view_stop_loading(native->web);
}

void ca_webview_native_go_back(Ca_WebViewNative *native)
{
    if (native) webkit_web_view_go_back(native->web);
}

void ca_webview_native_go_forward(Ca_WebViewNative *native)
{
    if (native) webkit_web_view_go_forward(native->web);
}

void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor)
{
    if (native) webkit_web_view_set_zoom_level(native->web, factor);
}

bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data)
{
    if (!native || !script || !g_utf8_validate(script, -1, NULL)) return false;
    EvalRequest *request = NULL;
    GAsyncReadyCallback callback = NULL;
    if (on_result) {
        request = g_new0(EvalRequest, 1);
        request->live = liveness_ref(native->live);
        request->fn = on_result;
        request->user_data = user_data;
        callback = on_eval_finished;
    }
#ifdef CA_WK_EVALUATE_JAVASCRIPT
    webkit_web_view_evaluate_javascript(native->web, script, -1, NULL, NULL,
                                        native->cancellable, callback, request);
#else
    webkit_web_view_run_javascript(native->web, script, native->cancellable, callback, request);
#endif
    return true;
}

void ca_webview_native_focus(Ca_WebViewNative *native)
{
    if (native) claim_focus(native);
}

void ca_webview_native_blur(Ca_WebViewNative *native)
{
    if (native && native->focused) release_focus(native);
}

bool ca_webview_native_has_focus(const Ca_WebViewNative *native)
{
    return native && native->visible && native->focused;
}
