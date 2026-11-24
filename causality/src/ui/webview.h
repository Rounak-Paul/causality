// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview.h — internal lifecycle hooks for the web view widget. */
#pragma once

#include "ca_internal.h"

/**
 * Bind a freshly acquired web view to its node and start its native engine.
 *
 * @param view Zeroed pool slot; owned by node's window on success.
 * @param node Layout node that positions the native view.
 * @param desc Creation description.
 * @return false on failure; the slot holds no resources and the caller releases it.
 */
bool ca_webview_attach(Ca_WebView *view, Ca_Node *node, const Ca_WebViewDesc *desc);

/** Release a web view's native engine and storage, then return its slot to the pool. */
void ca_webview_release(Ca_Window *window, Ca_WebView *view);

/** Release engines and storage of every live web view without returning slots (window teardown). */
void ca_webview_release_all(Ca_Window *window);

/** Update every web view in window to its node's laid-out rectangle and visibility. */
void ca_webview_sync_window(Ca_Window *window);
