// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* Drives a real window and the platform web engine: loading, events, the
   JavaScript bridge, navigation policy, geometry, visibility and teardown. */

#include "ca_internal.h"
#include "ca_reactive.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                      \
                    __FILE__, __LINE__, #condition);                            \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define PAGE_WIDTH  400.0f
#define PAGE_HEIGHT 300.0f
#define PUMP_TIMEOUT_SECONDS 15.0

static const char *const k_page =
    "<!doctype html><title>Probe</title><body style='margin:0'>"
    "<script>"
    "window.addEventListener('causality',function(e){window.causality.postMessage('echo:'+e.detail);});"
    "window.causality.postMessage({ready:true});"
    "</script></body>";

typedef struct {
    int  finished;
    int  failed;
    int  messages;
    int  titles;
    int  history;
    int  new_windows;
    int  terminated;
    int  started;
    char last_message[512];
    char title[64];
    char new_window_url[128];
    bool remove_on_finish;
    Ca_Signal *show;
    Ca_Signal *hidden;
    Ca_WebView *view;
    int  evals;
    bool eval_ok;
    char eval_json[128];
} Probe;

/** Records every event, optionally removing the view from inside its own callback. */
static void on_event(Ca_WebView *view, const Ca_WebViewEvent *event, void *data)
{
    Probe *probe = data;
    switch (event->type) {
    case CA_WEBVIEW_EVENT_LOAD_STARTED: probe->started++; break;
    case CA_WEBVIEW_EVENT_LOAD_FINISHED:
        probe->finished++;
        if (probe->remove_on_finish) ca_signal_set_bool(probe->show, false);
        break;
    case CA_WEBVIEW_EVENT_LOAD_FAILED: probe->failed++; break;
    case CA_WEBVIEW_EVENT_TITLE_CHANGED:
        probe->titles++;
        snprintf(probe->title, sizeof probe->title, "%s", event->title);
        break;
    case CA_WEBVIEW_EVENT_HISTORY_CHANGED: probe->history++; break;
    case CA_WEBVIEW_EVENT_MESSAGE:
        probe->messages++;
        snprintf(probe->last_message, sizeof probe->last_message, "%s", event->detail);
        break;
    case CA_WEBVIEW_EVENT_NEW_WINDOW:
        probe->new_windows++;
        snprintf(probe->new_window_url, sizeof probe->new_window_url, "%s", event->detail);
        break;
    case CA_WEBVIEW_EVENT_PROCESS_TERMINATED: probe->terminated++; break;
    default: break;
    }
    (void)view;
}

/** Cancels every navigation whose address contains "blocked". */
static bool on_navigate(Ca_WebView *view, const char *url, void *data)
{
    (void)view; (void)data;
    return strstr(url, "blocked") == NULL;
}

/** Content builder: a header above a wrapper that optionally hosts the web view. */
static void build(Ca_Div *div, void *data)
{
    Probe *probe = data;
    (void)div;
    ca_div_begin(&(Ca_DivDesc){ .id = "header", .height = 40.0f, .width = 100.0f });
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){ .id = "wrap", .hidden = ca_signal_get_bool(probe->hidden) });
    if (ca_signal_get_bool(probe->show))
        probe->view = ca_webview(&(Ca_WebViewDesc){
            .id = "web", .width = PAGE_WIDTH, .height = PAGE_HEIGHT, .html = k_page,
            .on_event = on_event, .on_navigate = on_navigate, .user_data = probe,
        });
    else
        probe->view = NULL;
    ca_div_end();
}

/** Completion of a probe eval. */
static void on_eval(Ca_WebView *view, bool ok, const char *json, void *data)
{
    Probe *probe = data;
    (void)view;
    probe->evals++;
    probe->eval_ok = ok;
    snprintf(probe->eval_json, sizeof probe->eval_json, "%s", json);
}

/** Step the instance until condition(probe) holds or the timeout elapses. */
static bool pump_until(Ca_Instance *instance, bool (*condition)(const Probe *), const Probe *probe)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (!ca_instance_step(instance)) return false;
        if (condition(probe)) return true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1.0e9;
        if (elapsed > PUMP_TIMEOUT_SECONDS) return false;
        usleep(2000);
    }
}

/** Step a fixed number of frames. */
static void pump_frames(Ca_Instance *instance, int frames)
{
    for (int i = 0; i < frames; ++i) {
        ca_instance_step(instance);
        usleep(2000);
    }
}

static bool ready(const Probe *p)       { return p->finished >= 1 && p->messages >= 1 && p->titles >= 1; }
static bool eval_done(const Probe *p)   { return p->evals >= 1; }
static bool echoed(const Probe *p)      { return strncmp(p->last_message, "echo:", 5) == 0; }
static bool failed_load(const Probe *p) { return p->failed >= 1; }
static bool nav_done(const Probe *p)    { return p->finished >= 1; }
static bool new_window(const Probe *p)  { return p->new_windows >= 1; }

/** Run script and wait for its result. */
static bool eval_sync(Ca_Instance *instance, Probe *probe, const char *script)
{
    probe->evals = 0;
    if (!ca_webview_eval(probe->view, script, on_eval, probe)) return false;
    return pump_until(instance, eval_done, probe);
}

/** Loading, state, bridge, eval, policy and navigation. */
static bool test_page_lifecycle(Ca_Instance *instance, Probe *probe)
{
    CHECK(pump_until(instance, ready, probe));
    CHECK(probe->view);
    CHECK(strcmp(probe->title, "Probe") == 0);
    CHECK(strcmp(ca_webview_title(probe->view), "Probe") == 0);
    CHECK(strcmp(probe->last_message, "{\"ready\":true}") == 0);
    CHECK(!ca_webview_is_loading(probe->view));
    CHECK(ca_webview_progress(probe->view) > 0.99);

    CHECK(eval_sync(instance, probe, "1+2"));
    CHECK(probe->eval_ok && strcmp(probe->eval_json, "3") == 0);
    CHECK(eval_sync(instance, probe, "'a\"b'"));
    CHECK(probe->eval_ok && strcmp(probe->eval_json, "\"a\\\"b\"") == 0);
    CHECK(eval_sync(instance, probe, "undefined"));
    CHECK(probe->eval_ok && strcmp(probe->eval_json, "null") == 0);
    CHECK(eval_sync(instance, probe, "throw new Error('boom')"));
    CHECK(!probe->eval_ok && strstr(probe->eval_json, "boom"));
    CHECK(eval_sync(instance, probe, "({a:[1,2]})"));
    CHECK(probe->eval_ok && strcmp(probe->eval_json, "{\"a\":[1,2]}") == 0);
    CHECK(!ca_webview_eval(probe->view, NULL, NULL, NULL));

    const char *tricky = "q\"uote\\ back\nline\ttab \xE2\x80\xA8 sep \xC3\xA9 </script>";
    probe->messages = 0;
    CHECK(ca_webview_post_message(probe->view, tricky));
    CHECK(pump_until(instance, echoed, probe));
    char expected[512];
    snprintf(expected, sizeof expected, "echo:%s", tricky);
    CHECK(strcmp(probe->last_message, expected) == 0);

    CHECK(eval_sync(instance, probe, "window.innerWidth"));
    CHECK(probe->eval_ok &&
          fabs(atof(probe->eval_json) - PAGE_WIDTH * probe->view->window->ui_scale) <= 1.0);

    probe->finished = 0;
    CHECK(ca_webview_load_url(probe->view, "data:text/html,<title>First</title>a"));
    CHECK(pump_until(instance, nav_done, probe));
    probe->finished = 0;
    CHECK(ca_webview_load_url(probe->view, "data:text/html,<title>Second</title>b"));
    CHECK(pump_until(instance, nav_done, probe));
    CHECK(strcmp(ca_webview_title(probe->view), "Second") == 0);
    CHECK(strncmp(ca_webview_url(probe->view), "data:", 5) == 0);
    CHECK(ca_webview_can_go_back(probe->view));

    probe->finished = 0;
    CHECK(ca_webview_load_url(probe->view, "data:text/html,blocked"));
    pump_frames(instance, 60);
    CHECK(probe->finished == 0);
    CHECK(strcmp(ca_webview_title(probe->view), "Second") == 0);

    CHECK(!ca_webview_load_url(probe->view, "not a url"));
    CHECK(!ca_webview_load_url(probe->view, NULL));
    CHECK(!ca_webview_load_html(probe->view, NULL, NULL));

    probe->finished = 0;
    ca_webview_go_back(probe->view);
    CHECK(pump_until(instance, nav_done, probe));
    CHECK(strcmp(ca_webview_title(probe->view), "First") == 0);
    CHECK(ca_webview_can_go_forward(probe->view));

    CHECK(eval_sync(instance, probe,
                    "window.open('https://example.invalid/popup'); 0"));
    CHECK(pump_until(instance, new_window, probe));
    CHECK(strcmp(probe->new_window_url, "https://example.invalid/popup") == 0);

    CHECK(ca_webview_load_url(probe->view, "file:///nonexistent-causality-dir/missing.html"));
    CHECK(pump_until(instance, failed_load, probe));
    return true;
}

/** The native view follows layout, clips and hides with its ancestors, and reacts to modals. */
static bool test_geometry_and_visibility(Ca_Instance *instance, Probe *probe)
{
    pump_frames(instance, 10);
    Ca_WebView *view = probe->view;
    CHECK(view && view->applied_valid);
    const Ca_Node *node = view->node;
    CHECK(view->applied.visible);
    CHECK(fabsf(view->applied.x - node->x) < 0.01f && fabsf(view->applied.y - node->y) < 0.01f);
    CHECK(fabsf(view->applied.w - node->w) < 0.01f && fabsf(view->applied.h - node->h) < 0.01f);
    CHECK(node->y > 0.0f);
    CHECK(fabsf(view->applied.clip_w - node->w) < 0.01f);

    probe->view->window->debug_overlay = true;
    ca_instance_wake();
    pump_frames(instance, 10);
    CHECK(!probe->view->applied.visible);
    probe->view->window->debug_overlay = false;
    ca_instance_wake();
    pump_frames(instance, 10);
    CHECK(probe->view->applied.visible);

    ca_signal_set_bool(probe->hidden, true);
    pump_frames(instance, 10);
    CHECK(probe->view && !probe->view->applied.visible);
    ca_signal_set_bool(probe->hidden, false);
    pump_frames(instance, 10);
    CHECK(probe->view && probe->view->applied.visible);
    return true;
}

/** Focus handover and zoom/blur calls are safe and observable. */
static bool test_focus_and_zoom(Ca_Instance *instance, Probe *probe)
{
    ca_webview_set_zoom(probe->view, 1.5);
    ca_webview_set_zoom(probe->view, NAN);
    ca_webview_set_zoom(probe->view, 1000.0);
    ca_webview_set_zoom(probe->view, 1.0);
    ca_webview_blur(probe->view);
    CHECK(!ca_webview_has_focus(probe->view));
    ca_webview_set_zoom(NULL, 1.0);
    ca_webview_focus(NULL);
    CHECK(!ca_webview_has_focus(NULL));
    CHECK(strcmp(ca_webview_url(NULL), "") == 0);
    pump_frames(instance, 5);
    return true;
}

/** A retained node keeps its page across rebuilds, and removal destroys the native view. */
static bool test_rebuild_and_removal(Ca_Instance *instance, Ca_Window *window, Probe *probe)
{
    Ca_WebView *before = probe->view;
    int finished = probe->finished;
    ca_div_invalidate(ca_window_content_div(window));
    pump_frames(instance, 15);
    CHECK(probe->view == before);
    CHECK(probe->finished == finished);
    CHECK(window->webview_count == 1u);

    ca_signal_set_bool(probe->show, false);
    pump_frames(instance, 10);
    CHECK(window->webview_count == 0u);
    CHECK(ca_pool_live_count(&window->webview_pool) == 0u);

    ca_signal_set_bool(probe->show, true);
    pump_frames(instance, 10);
    CHECK(window->webview_count == 1u);
    return true;
}

/** Removing the view from inside its own event callback is deferred until the callback returns. */
static bool test_removal_inside_callback(Ca_Instance *instance, Ca_Window *window, Probe *probe)
{
    probe->finished = 0;
    probe->messages = 0;
    probe->remove_on_finish = true;
    CHECK(ca_webview_load_url(probe->view, "data:text/html,again"));
    for (int i = 0; i < 600 && window->webview_count > 0u; ++i) {
        ca_instance_step(instance);
        usleep(2000);
    }
    CHECK(window->webview_count == 0u);
    CHECK(ca_pool_live_count(&window->webview_pool) == 0u);
    probe->remove_on_finish = false;
    ca_signal_set_bool(probe->show, true);
    pump_frames(instance, 10);
    CHECK(window->webview_count == 1u);
    return true;
}

int main(void)
{
    if (!ca_webview_supported()) {
        puts("webview tests skipped: unsupported platform");
        return 0;
    }
    Ca_Instance *instance = ca_instance_create(&(Ca_InstanceDesc){ .app_name = "webview tests" });
    if (!instance) {
        fputs("instance creation failed\n", stderr);
        return 1;
    }
    Ca_Window *window = ca_window_create(instance, &(Ca_WindowDesc){
        .title = "webview tests", .width = 800, .height = 600, .native_frame = true });
    if (!window) {
        fputs("window creation failed\n", stderr);
        ca_instance_destroy(instance);
        return 1;
    }

    Probe probe = {0};
    probe.show   = ca_signal_bool(instance, true);
    probe.hidden = ca_signal_bool(instance, false);
    if (!probe.show || !probe.hidden ||
        !ca_div_set_builder(ca_window_content_div(window), build, &probe)) {
        fputs("fixture setup failed\n", stderr);
        ca_instance_destroy(instance);
        return 1;
    }

    bool ok = test_page_lifecycle(instance, &probe) &&
              test_geometry_and_visibility(instance, &probe) &&
              test_focus_and_zoom(instance, &probe) &&
              test_rebuild_and_removal(instance, window, &probe) &&
              test_removal_inside_callback(instance, window, &probe);

    ca_window_destroy(window);
    ca_instance_destroy(instance);
    if (!ok) return 1;
    puts("causality webview tests passed");
    return 0;
}
