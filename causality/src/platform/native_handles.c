// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* native_handles.c — exposes the GLFW window's native surface to web view backends. */

#include "webview_native.h"
#include "ca_internal.h"

#define GLFW_INCLUDE_NONE
#if defined(CA_PLATFORM_WINDOWS)
#  define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(CA_PLATFORM_LINUX)
#  define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

void *ca_platform_native_window(const Ca_Window *window)
{
    if (!window || !window->platform_handle) return NULL;
#if defined(CA_PLATFORM_WINDOWS)
    return glfwGetWin32Window(window->platform_handle);
#elif defined(CA_PLATFORM_LINUX)
    return (void *)(uintptr_t)glfwGetX11Window(window->platform_handle);
#else
    return NULL;
#endif
}

void *ca_platform_native_display(void)
{
#if defined(CA_PLATFORM_LINUX)
    return glfwGetX11Display();
#else
    return NULL;
#endif
}
