// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_cocoa.m — macOS web view backend. Hosts a WKWebView inside a clipping container that is
   a sibling of the GLFW window's content view. Shared WebKit logic lives in webview_wk.m. */

#import <AppKit/AppKit.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

/* QuartzCore defines an unrelated CA_MALLOC attribute macro. */
#undef CA_MALLOC

#include "webview_backend.h"
#include "webview_wk.h"

static const CGFloat kFrameEpsilon = 0.01;

/* Keys the host window handles itself (debug overlay). WebKit would otherwise swallow them. */
static const unsigned short kHostKeyCodeF9 = 101;

/** Container that clips the page to the visible part of its layout rectangle. */
@interface CaWebViewClip : NSView
@end

@implementation CaWebViewClip
- (BOOL)isFlipped { return YES; }
@end

/** WKWebView that routes standard editing shortcuts when the host app has no Edit menu. */
@interface CaWebViewPage : WKWebView
@end

@implementation CaWebViewPage
- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    NSEventModifierFlags relevant = mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                            NSEventModifierFlagOption);
    NSResponder *responder = self.window.firstResponder;
    BOOL focused = [responder isKindOfClass:[NSView class]] &&
                   [(NSView *)responder isDescendantOf:self];
    if (event.type == NSEventTypeKeyDown && focused && relevant == NSEventModifierFlagCommand) {
        NSString *key = event.charactersIgnoringModifiers.lowercaseString;
        BOOL shift = (mods & NSEventModifierFlagShift) != 0;
        SEL action = NULL;
        if ([key isEqualToString:@"c"] && !shift)      action = @selector(copy:);
        else if ([key isEqualToString:@"x"] && !shift) action = @selector(cut:);
        else if ([key isEqualToString:@"v"] && !shift) action = @selector(paste:);
        else if ([key isEqualToString:@"a"] && !shift) action = @selector(selectAll:);
        else if ([key isEqualToString:@"z"])           action = shift ? @selector(redo:) : @selector(undo:);
        if (action && [NSApp sendAction:action to:nil from:self]) return YES;
    }
    return [super performKeyEquivalent:event];
}
@end

/** macOS host: view tree, key routing, focus and native dialogs. */
@interface CaWebViewHost : CaWebKitHost
@property (nonatomic, weak) NSWindow *window;
@property (nonatomic, strong) CaWebViewClip *clip;
@property (nonatomic, strong) id keyMonitor;
- (void)applyFrame:(const Ca_WebViewFrame *)frame;
@end

@implementation CaWebViewHost

- (BOOL)hasFocus
{
    NSResponder *responder = self.window.firstResponder;
    return self.clip && [responder isKindOfClass:[NSView class]] &&
           [(NSView *)responder isDescendantOf:self.clip];
}

- (void)applyFrame:(const Ca_WebViewFrame *)frame
{
    NSView *content = self.window.contentView;
    if (!content || !frame->visible) {
        if ([self hasFocus]) [self.window makeFirstResponder:content];
        self.clip.hidden = YES;
        return;
    }
    CGFloat top = content.isFlipped ? frame->clip_y
                                    : content.bounds.size.height - (frame->clip_y + frame->clip_h);
    NSRect clipInContent = NSMakeRect(frame->clip_x, top, frame->clip_w, frame->clip_h);
    self.clip.frame = [self.clip.superview convertRect:clipInContent fromView:content];
    self.page.frame = NSMakeRect(frame->x - frame->clip_x, frame->y - frame->clip_y,
                                 frame->w, frame->h);
    BOOL unclipped = fabs(frame->clip_x - frame->x) < kFrameEpsilon &&
                     fabs(frame->clip_y - frame->y) < kFrameEpsilon &&
                     fabs(frame->clip_w - frame->w) < kFrameEpsilon &&
                     fabs(frame->clip_h - frame->h) < kFrameEpsilon;
    self.clip.layer.cornerRadius = unclipped ? frame->corner_radius : 0.0;
    self.clip.hidden = NO;
}

/** Route host-reserved keys to the window's own input instead of the page. */
- (void)installKeyMonitor
{
    __weak CaWebViewHost *weakSelf = self;
    self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | NSEventMaskKeyUp)
                                                            handler:^NSEvent *(NSEvent *event) {
        CaWebViewHost *host = weakSelf;
        if (!host || event.window != host.window || event.keyCode != kHostKeyCodeF9 || ![host hasFocus])
            return event;
        NSView *content = host.window.contentView;
        if (event.type == NSEventTypeKeyDown) [content keyDown:event];
        else [content keyUp:event];
        return nil;
    }];
}

- (void)teardown
{
    if (self.keyMonitor) [NSEvent removeMonitor:self.keyMonitor];
    self.keyMonitor = nil;
    if ([self hasFocus]) [self.window makeFirstResponder:self.window.contentView];
    [super teardown];
    [self.clip removeFromSuperview];
}

#pragma mark WKUIDelegate dialogs

/** Run alert as a sheet on the host window; completion receives the pressed button. */
- (void)presentAlert:(NSAlert *)alert completion:(void (^)(NSModalResponse))completion
{
    NSWindow *window = self.window;
    if (!window) {
        completion(NSModalResponseCancel);
        return;
    }
    [alert beginSheetModalForWindow:window completionHandler:completion];
}

- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))handler
{
    NSAlert *alert = [NSAlert new];
    alert.messageText = message;
    [self presentAlert:alert completion:^(NSModalResponse response) { handler(); }];
}

- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))handler
{
    NSAlert *alert = [NSAlert new];
    alert.messageText = message;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    [self presentAlert:alert completion:^(NSModalResponse response) {
        handler(response == NSAlertFirstButtonReturn);
    }];
}

- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
    defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame
completionHandler:(void (^)(NSString *))handler
{
    NSAlert *alert = [NSAlert new];
    alert.messageText = prompt;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = defaultText ?: @"";
    alert.accessoryView = field;
    [self presentAlert:alert completion:^(NSModalResponse response) {
        handler(response == NSAlertFirstButtonReturn ? field.stringValue : nil);
    }];
}

- (void)webView:(WKWebView *)webView
runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSArray<NSURL *> *))handler
{
    NSWindow *window = self.window;
    if (!window) {
        handler(nil);
        return;
    }
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    panel.canChooseDirectories = parameters.allowsDirectories;
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        handler(response == NSModalResponseOK ? panel.URLs : nil);
    }];
}

@end

bool ca_webview_native_supported(void)
{
    return true;
}

void ca_webview_native_pump(Ca_Window *window)
{
    (void)window;
}

Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc)
{
    if (![NSThread isMainThread] || !window->platform_handle) return NULL;
    @autoreleasepool {
        NSWindow *nswindow = glfwGetCocoaWindow(window->platform_handle);
        NSView *content = nswindow.contentView;
        if (!content) return NULL;

        CaWebViewHost *host = [CaWebViewHost new];
        host.owner = owner;
        host.window = nswindow;
        WKWebViewConfiguration *config = [host makeConfigurationFor:desc];

        CaWebViewClip *clip = [[CaWebViewClip alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
        clip.wantsLayer = YES;
        clip.layer.masksToBounds = YES;
        clip.hidden = YES;

        CaWebViewPage *page = [[CaWebViewPage alloc] initWithFrame:clip.bounds configuration:config];
        if (desc->transparent) {
            @try { [page setValue:@NO forKey:@"drawsBackground"]; }
            @catch (NSException *exception) {}
        }
        [clip addSubview:page];
        /* The content view hosts a CAMetalLayer as its layer, and AppKit does not
           composite subviews of a layer-hosting view. Sit beside it instead. */
        NSView *parent = content.superview ?: content;
        [parent addSubview:clip positioned:NSWindowAbove relativeTo:content];

        host.clip = clip;
        [host adoptPage:page description:desc];
        [host installKeyMonitor];
        return (__bridge_retained Ca_WebViewNative *)host;
    }
}

void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame)
{
    [(CaWebViewHost *)ca_wk_host(native) applyFrame:frame];
}

void ca_webview_native_focus(Ca_WebViewNative *native)
{
    CaWebViewHost *host = (CaWebViewHost *)ca_wk_host(native);
    if (host.window.isKeyWindow && !host.clip.hidden)
        [host.window makeFirstResponder:host.page];
}

void ca_webview_native_blur(Ca_WebViewNative *native)
{
    CaWebViewHost *host = (CaWebViewHost *)ca_wk_host(native);
    if ([host hasFocus]) [host.window makeFirstResponder:host.window.contentView];
}
