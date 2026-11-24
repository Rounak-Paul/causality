// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_unsupported.c — web view backend for platforms without a native
   engine binding. ca_webview_supported() reports false and widgets are never
   created, so only the support query and creation can be reached. */
#include "webview_backend.h"

bool ca_webview_native_supported(void)
{
    return false;
}

Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc)
{
    (void)window; (void)owner; (void)desc;
    return NULL;
}

void ca_webview_native_destroy(Ca_WebViewNative *native) { (void)native; }

void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame)
{
    (void)native; (void)frame;
}

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url)
{
    (void)native; (void)url;
    return false;
}

bool ca_webview_native_load_html(Ca_WebViewNative *native, const char *html, const char *base_url)
{
    (void)native; (void)html; (void)base_url;
    return false;
}

void ca_webview_native_reload(Ca_WebViewNative *native) { (void)native; }
void ca_webview_native_stop(Ca_WebViewNative *native) { (void)native; }
void ca_webview_native_go_back(Ca_WebViewNative *native) { (void)native; }
void ca_webview_native_go_forward(Ca_WebViewNative *native) { (void)native; }
void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor)
{
    (void)native; (void)factor;
}

bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data)
{
    (void)native; (void)script; (void)on_result; (void)user_data;
    return false;
}

void ca_webview_native_focus(Ca_WebViewNative *native) { (void)native; }
void ca_webview_native_blur(Ca_WebViewNative *native) { (void)native; }
bool ca_webview_native_has_focus(const Ca_WebViewNative *native)
{
    (void)native;
    return false;
}
