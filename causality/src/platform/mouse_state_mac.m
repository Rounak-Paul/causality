// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#import <AppKit/AppKit.h>
#include <stdbool.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

/*
 * Query the live OS-level left mouse button state.
 *
 * On macOS, GLFW's cached mouseButtons[] can get permanently stuck after a
 * resize drag because Cocoa does not deliver mouseUp to an NSView that lost
 * mouse capture (e.g. when the window was resized while the cursor was near
 * its edge). This bypasses the GLFW cache and queries the WindowServer
 * directly via NSEvent so resize_active is always cleared on true release.
 *
 * Returns true if the left button is physically held, false otherwise.
 */
bool ca_mac_left_button_held(void)
{
    return ([NSEvent pressedMouseButtons] & 1) != 0;
}

/*
 * Query the global cursor position in screen coordinates (top-left origin).
 *
 * During a software resize drag, Cocoa can stop delivering mouseDragged to the
 * NSView (the OS captures the event stream for borderless windows), freezing
 * GLFW's cursor position. [NSEvent mouseLocation] reports the WindowServer's
 * live cursor position regardless of focus, so the resize can track correctly.
 *
 * Coordinates are flipped to a top-left origin to match GLFW screen space.
 *
 * out_x, out_y  Receive the cursor position in screen pixels.
 */
void ca_mac_cursor_screen_pos(double *out_x, double *out_y)
{
    NSPoint p = [NSEvent mouseLocation];
    NSScreen *primary = [[NSScreen screens] firstObject];
    double screen_h = primary ? primary.frame.size.height : 0.0;
    if (out_x) *out_x = p.x;
    if (out_y) *out_y = screen_h - p.y;
}

/*
 * Resize and reposition an NSWindow directly, in GLFW screen coordinates.
 *
 * Used to apply the final geometry once the resize drag ends. The drag itself
 * only tracks the target size; applying any frame change mid-drag cancels the
 * borderless window's mouse-tracking session, freezing the cursor.
 *
 * glfw_window  The GLFWwindow* whose Cocoa window will be resized.
 * x, y         New top-left position in screen coordinates (top-left origin).
 * w, h         New content size in points.
 */
void ca_mac_set_window_frame(void *glfw_window, int x, int y, int w, int h)
{
    NSWindow *win = glfwGetCocoaWindow((GLFWwindow *)glfw_window);
    if (!win) return;

    NSScreen *primary = [[NSScreen screens] firstObject];
    double screen_h = primary ? primary.frame.size.height : 0.0;

    /* Convert GLFW top-left content origin to Cocoa bottom-left frame origin. */
    NSRect contentRect = NSMakeRect(x, screen_h - (y + h), w, h);
    NSRect frameRect = [win frameRectForContentRect:contentRect];
    [win setFrame:frameRect display:YES];
}
