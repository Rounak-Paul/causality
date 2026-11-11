// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

#include "causality.h"

#ifdef __OBJC__
@class CAMetalLayer, UIView;

#ifdef __cplusplus
extern "C" {
#endif

/** Create an iOS window for a laid-out layer. Call all host functions on the main thread. */
CA_API Ca_Window *ca_ios_window_create(Ca_Instance *instance, CAMetalLayer *layer,
                                       const Ca_WindowDesc *desc);

/** Replace the backing layer, or pass nil to suspend rendering. The UI tree stays live. */
CA_API bool ca_ios_window_set_layer(Ca_Window *window, CAMetalLayer *layer);

/** Report a change to the layer bounds or contentsScale before the next frame. */
CA_API void ca_ios_window_layout_changed(Ca_Window *window);

typedef enum {
    CA_IOS_POINTER_BEGIN,
    CA_IOS_POINTER_MOVE,
    CA_IOS_POINTER_END,
    CA_IOS_POINTER_CANCEL,
} Ca_IOSPointerPhase;

/** Queue a primary touch in layer-local UIKit points for the next frame. */
CA_API void ca_ios_window_pointer(Ca_Window *window, Ca_IOSPointerPhase phase, double x, double y);

/** Queue a scroll gesture anchored at x and y, with deltas in UIKit points. */
CA_API void ca_ios_window_scroll(Ca_Window *window, double x, double y, double dx, double dy);

/** Return whether a layer-local point hits a Causality splitter gutter. */
CA_API bool ca_ios_window_splitter_at(Ca_Window *window, double x, double y);

/** Update the UIKit responder for a focused single-line input. Call after each frame.
 * Pass nil to dismiss and detach the responder. */
CA_API void ca_ios_window_sync_keyboard(Ca_Window *window, UIView *view);

#ifdef __cplusplus
}
#endif
#endif
