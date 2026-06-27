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
 * Resize-preview outline window.
 *
 * Resizing the real OS window mid-drag (any setFrame/setContentSize variant)
 * cancels a borderless window's mouse-tracking session, freezing the cursor and
 * swallowing mouseUp. Instead we show a lightweight, non-interactive overlay
 * window that draws just a coloured border at the target rect, following the
 * cursor live; the real window is resized once on release (outside the drag).
 *
 * The overlay is a single shared NSWindow with a bordered CALayer. It ignores
 * mouse events (so it never steals the drag) and floats above other windows.
 */
static NSWindow *s_preview_window = nil;

static void ca_mac_preview_ensure(void)
{
    if (s_preview_window) return;

    s_preview_window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1)
                                    styleMask:NSWindowStyleMaskBorderless
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    s_preview_window.opaque = NO;
    s_preview_window.backgroundColor = [NSColor clearColor];
    s_preview_window.level = NSFloatingWindowLevel;
    s_preview_window.ignoresMouseEvents = YES;
    s_preview_window.hasShadow = NO;
    s_preview_window.releasedWhenClosed = NO;

    NSView *content = s_preview_window.contentView;
    content.wantsLayer = YES;
    CALayer *layer = content.layer;
    layer.backgroundColor = [NSColor clearColor].CGColor;
    layer.borderWidth = 2.0;
    layer.borderColor = [NSColor colorWithCalibratedWhite:1.0 alpha:0.85].CGColor;
    layer.cornerRadius = 6.0;
}

/*
 * Show/update the resize-preview outline at the given rect.
 *
 * x, y  Top-left position in screen coordinates (top-left origin).
 * w, h  Target size in points.
 */
void ca_mac_resize_preview_show(int x, int y, int w, int h)
{
    ca_mac_preview_ensure();

    NSScreen *primary = [[NSScreen screens] firstObject];
    double screen_h = primary ? primary.frame.size.height : 0.0;

    NSRect frame = NSMakeRect(x, screen_h - (y + h), w, h);
    [s_preview_window setFrame:frame display:YES];
    if (!s_preview_window.isVisible)
        [s_preview_window orderFront:nil];
}

/*
 * Hide the resize-preview outline (called when the drag ends).
 */
void ca_mac_resize_preview_hide(void)
{
    if (s_preview_window && s_preview_window.isVisible)
        [s_preview_window orderOut:nil];
}
