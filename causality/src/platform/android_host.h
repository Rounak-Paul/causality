// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* android_host.h — what an Android platform layer provides to the web view backend.
   webview_android.c defines weak defaults for the three provider functions, so the library
   links and reports "unsupported" when no platform layer is present. A platform layer
   overrides them by defining the same symbols with strong linkage. */
#pragma once

#include <jni.h>
#include <stdbool.h>

#include "causality.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The process-wide Java VM.
 *
 * @return The VM, or NULL when no platform layer registered one; web views are then unsupported.
 */
JavaVM *ca_android_java_vm(void);

/**
 * The Android view hierarchy that holds the window's render surface.
 *
 * The web view is added as a child above the render surface. Must be a plain
 * android.widget.FrameLayout (or a subclass accepting FrameLayout.LayoutParams) and should be
 * focusable so that focus can be handed back to it. The returned reference must stay valid
 * (a JNI global reference) for the lifetime of the window.
 *
 * @param window Host window.
 * @return Global reference to the host ViewGroup, or NULL when the window has none.
 */
jobject ca_android_window_host_view(const Ca_Window *window);

/**
 * Conversion factor from logical window points to device pixels (the display density).
 *
 * @param window Host window.
 * @return Pixels per point; values that are not positive are treated as 1.0.
 */
float ca_android_window_density(const Ca_Window *window);

/**
 * Register the companion class org.causality.CausalityWebView and its native methods.
 * Call from JNI_OnLoad (or any thread whose class loader sees the app classes) before the first
 * web view is created. Safe to call repeatedly.
 *
 * @param env JNI environment of the calling thread.
 * @return true when the class is registered and usable.
 */
bool ca_android_webview_register(JNIEnv *env);

#ifdef __cplusplus
}
#endif
