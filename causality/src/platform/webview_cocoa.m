// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_cocoa.m — macOS web view backend. Hosts a WKWebView inside a
   clipping container that is a subview of the GLFW window's content view. */

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

/* QuartzCore defines an unrelated CA_MALLOC attribute macro. */
#undef CA_MALLOC

#include "webview_backend.h"

static NSString *const kBridgeName = @"causality";
static NSString *const kBridgeScript =
    @"Object.defineProperty(window,'causality',{value:Object.freeze({"
     "postMessage:function(m){window.webkit.messageHandlers.causality.postMessage("
     "typeof m==='string'?m:JSON.stringify(m));}}),configurable:false});";
static const CGFloat kFrameEpsilon = 0.01;

static void *const kObserverContext = (void *)&kObserverContext;

@class CaWebViewHost;

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

/** Breaks the retain cycle WKUserContentController would otherwise form with its handler. */
@interface CaWebViewMessageProxy : NSObject <WKScriptMessageHandler>
@property (nonatomic, weak) CaWebViewHost *host;
@end

/** Owns the native objects of one Ca_WebView and translates engine callbacks. */
@interface CaWebViewHost : NSObject <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic) Ca_WebView *owner;
@property (nonatomic, weak) NSWindow *window;
@property (nonatomic, strong) CaWebViewClip *clip;
@property (nonatomic, strong) CaWebViewPage *page;
@property (nonatomic, strong) CaWebViewMessageProxy *proxy;
@property (nonatomic, strong) WKUserContentController *controller;
- (BOOL)hasFocus;
- (void)publish:(Ca_WebViewEventType)type detail:(NSString *)detail;
- (void)applyFrame:(const Ca_WebViewFrame *)frame;
- (void)teardown;
@end

@implementation CaWebViewMessageProxy
- (void)userContentController:(WKUserContentController *)controller
      didReceiveScriptMessage:(WKScriptMessage *)message
{
    CaWebViewHost *host = self.host;
    if (!host || !message.frameInfo.isMainFrame || ![message.body isKindOfClass:[NSString class]])
        return;
    [host publish:CA_WEBVIEW_EVENT_MESSAGE detail:message.body];
}
@end

@implementation CaWebViewHost

+ (NSArray<NSString *> *)observedKeys
{
    return @[@"title", @"URL", @"estimatedProgress", @"canGoBack", @"canGoForward"];
}

- (BOOL)hasFocus
{
    NSResponder *responder = self.window.firstResponder;
    return self.clip && [responder isKindOfClass:[NSView class]] &&
           [(NSView *)responder isDescendantOf:self.clip];
}

- (void)publish:(Ca_WebViewEventType)type detail:(NSString *)detail
{
    /* A callback may remove the web view, which releases this host mid-call. */
    CaWebViewHost *guard __attribute__((objc_precise_lifetime)) = self;
    (void)guard;
    if (!self.owner) return;
    Ca_WebViewState state = {
        .url            = self.page.URL.absoluteString.UTF8String,
        .title          = self.page.title.UTF8String,
        .progress       = self.page.estimatedProgress,
        .loading        = self.page.loading,
        .can_go_back    = self.page.canGoBack,
        .can_go_forward = self.page.canGoForward,
    };
    ca_webview_host_event(self.owner, type, &state, detail.UTF8String);
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context
{
    if (context != kObserverContext) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    Ca_WebViewEventType type = CA_WEBVIEW_EVENT_HISTORY_CHANGED;
    if ([keyPath isEqualToString:@"title"])                   type = CA_WEBVIEW_EVENT_TITLE_CHANGED;
    else if ([keyPath isEqualToString:@"URL"])                type = CA_WEBVIEW_EVENT_URL_CHANGED;
    else if ([keyPath isEqualToString:@"estimatedProgress"])  type = CA_WEBVIEW_EVENT_PROGRESS_CHANGED;
    [self publish:type detail:nil];
}

- (void)applyFrame:(const Ca_WebViewFrame *)frame
{
    NSView *content = self.window.contentView;
    if (!content || !frame->visible) {
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

- (void)teardown
{
    self.owner = NULL;
    if ([self hasFocus]) [self.window makeFirstResponder:self.window.contentView];
    for (NSString *key in [CaWebViewHost observedKeys])
        [self.page removeObserver:self forKeyPath:key context:kObserverContext];
    self.page.navigationDelegate = nil;
    self.page.UIDelegate = nil;
    [self.page stopLoading];
    [self.controller removeScriptMessageHandlerForName:kBridgeName];
    [self.controller removeAllUserScripts];
    self.proxy.host = nil;
    [self.clip removeFromSuperview];
}

#pragma mark WKNavigationDelegate

- (void)webView:(WKWebView *)webView
decidePolicyForNavigationAction:(WKNavigationAction *)action
decisionHandler:(void (^)(WKNavigationActionPolicy))handler
{
    if (!self.owner || !action.targetFrame.isMainFrame) {
        handler(WKNavigationActionPolicyAllow);
        return;
    }
    CaWebViewHost *guard __attribute__((objc_precise_lifetime)) = self;
    (void)guard;
    const char *url = action.request.URL.absoluteString.UTF8String;
    BOOL allowed = ca_webview_host_allow_navigation(self.owner, url);
    handler(allowed ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation
{
    [self publish:CA_WEBVIEW_EVENT_LOAD_STARTED detail:nil];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
    [self publish:CA_WEBVIEW_EVENT_LOAD_FINISHED detail:nil];
}

- (void)reportFailure:(NSError *)error
{
    if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) return;
    [self publish:CA_WEBVIEW_EVENT_LOAD_FAILED detail:error.localizedDescription];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation
      withError:(NSError *)error
{
    [self reportFailure:error];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation
      withError:(NSError *)error
{
    [self reportFailure:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    [self publish:CA_WEBVIEW_EVENT_PROCESS_TERMINATED detail:nil];
}

#pragma mark WKUIDelegate

- (WKWebView *)webView:(WKWebView *)webView
createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
   forNavigationAction:(WKNavigationAction *)action
        windowFeatures:(WKWindowFeatures *)features
{
    [self publish:CA_WEBVIEW_EVENT_NEW_WINDOW detail:action.request.URL.absoluteString];
    return nil;
}

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

/** Convert a JavaScript completion value to JSON text, "null" when it has no JSON form. */
static NSString *json_text(id value)
{
    if (!value || value == [NSNull null]) return @"null";
    @try {
        BOOL fragment = [value isKindOfClass:[NSString class]] || [value isKindOfClass:[NSNumber class]];
        if (!fragment && ![NSJSONSerialization isValidJSONObject:value]) return @"null";
        NSData *data = [NSJSONSerialization dataWithJSONObject:value
                                                       options:NSJSONWritingFragmentsAllowed
                                                         error:nil];
        NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
        return text ?: @"null";
    } @catch (NSException *exception) {
        return @"null";
    }
}

/** Describe a failed script, preferring the page's own exception message. */
static NSString *script_error_text(NSError *error)
{
    NSString *message = error.userInfo[@"WKJavaScriptExceptionMessage"];
    return message ?: error.localizedDescription;
}

/** Resolve the Objective-C host behind an opaque native handle. */
static CaWebViewHost *host_of(const Ca_WebViewNative *native)
{
    return (__bridge CaWebViewHost *)(void *)native;
}

bool ca_webview_native_supported(void)
{
    return true;
}

Ca_WebViewNative *ca_webview_native_create(Ca_Window *window, Ca_WebView *owner,
                                           const Ca_WebViewDesc *desc)
{
    if (![NSThread isMainThread] || !window->platform_handle) return NULL;
    @autoreleasepool {
        NSWindow *nswindow = glfwGetCocoaWindow(window->platform_handle);
        NSView *content = nswindow.contentView;
        if (!content) return NULL;

        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        WKUserContentController *controller = config.userContentController;
        CaWebViewMessageProxy *proxy = [CaWebViewMessageProxy new];
        [controller addScriptMessageHandler:proxy name:kBridgeName];
        [controller addUserScript:[[WKUserScript alloc]
            initWithSource:kBridgeScript
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:YES]];
        NSString *init = desc->init_script ? [NSString stringWithUTF8String:desc->init_script] : nil;
        if (init)
            [controller addUserScript:[[WKUserScript alloc]
                initWithSource:init
                 injectionTime:WKUserScriptInjectionTimeAtDocumentStart
              forMainFrameOnly:NO]];
        if (desc->devtools)
            [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];

        CaWebViewHost *host = [CaWebViewHost new];
        host.owner = owner;
        host.window = nswindow;
        host.proxy = proxy;
        host.controller = controller;
        proxy.host = host;

        CaWebViewClip *clip = [[CaWebViewClip alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
        clip.wantsLayer = YES;
        clip.layer.masksToBounds = YES;
        clip.hidden = YES;

        CaWebViewPage *page = [[CaWebViewPage alloc] initWithFrame:clip.bounds configuration:config];
        page.navigationDelegate = host;
        page.UIDelegate = host;
        if (desc->user_agent) {
            NSString *agent = [NSString stringWithUTF8String:desc->user_agent];
            if (agent) page.customUserAgent = agent;
        }
        if (desc->transparent) {
            @try { [page setValue:@NO forKey:@"drawsBackground"]; }
            @catch (NSException *exception) {}
        }
        if (desc->devtools) {
            if (@available(macOS 13.3, *)) page.inspectable = YES;
        }
        [clip addSubview:page];
        /* The content view hosts a CAMetalLayer as its layer, and AppKit does not
           composite subviews of a layer-hosting view. Sit beside it instead. */
        NSView *parent = content.superview ?: content;
        [parent addSubview:clip positioned:NSWindowAbove relativeTo:content];

        host.clip = clip;
        host.page = page;
        for (NSString *key in [CaWebViewHost observedKeys])
            [page addObserver:host forKeyPath:key options:0 context:kObserverContext];
        return (__bridge_retained Ca_WebViewNative *)host;
    }
}

void ca_webview_native_destroy(Ca_WebViewNative *native)
{
    if (!native) return;
    CaWebViewHost *host = (__bridge_transfer CaWebViewHost *)native;
    [host teardown];
}

void ca_webview_native_set_frame(Ca_WebViewNative *native, const Ca_WebViewFrame *frame)
{
    [host_of(native) applyFrame:frame];
}

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url)
{
    NSString *text = [NSString stringWithUTF8String:url];
    NSURL *target = text ? [NSURL URLWithString:text] : nil;
    if (!target || target.scheme.length == 0) return false;
    WKWebView *page = host_of(native).page;
    if (target.isFileURL) {
        BOOL directory = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:target.path isDirectory:&directory];
        NSURL *readable = directory ? target : [target URLByDeletingLastPathComponent];
        return [page loadFileURL:target allowingReadAccessToURL:readable] != nil;
    }
    return [page loadRequest:[NSURLRequest requestWithURL:target]] != nil;
}

bool ca_webview_native_load_html(Ca_WebViewNative *native, const char *html, const char *base_url)
{
    NSString *source = [NSString stringWithUTF8String:html];
    if (!source) return false;
    NSString *base = base_url ? [NSString stringWithUTF8String:base_url] : nil;
    NSURL *baseURL = base ? [NSURL URLWithString:base] : nil;
    return [host_of(native).page loadHTMLString:source baseURL:baseURL] != nil;
}

void ca_webview_native_reload(Ca_WebViewNative *native)
{
    [host_of(native).page reload];
}

void ca_webview_native_stop(Ca_WebViewNative *native)
{
    [host_of(native).page stopLoading];
}

void ca_webview_native_go_back(Ca_WebViewNative *native)
{
    [host_of(native).page goBack];
}

void ca_webview_native_go_forward(Ca_WebViewNative *native)
{
    [host_of(native).page goForward];
}

void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor)
{
    if (@available(macOS 11.0, *)) host_of(native).page.pageZoom = factor;
}

bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data)
{
    NSString *source = [NSString stringWithUTF8String:script];
    if (!source) return false;
    CaWebViewHost *host = host_of(native);
    if (!on_result) {
        [host.page evaluateJavaScript:source completionHandler:nil];
        return true;
    }
    [host.page evaluateJavaScript:source completionHandler:^(id result, NSError *error) {
        Ca_WebView *owner = host.owner;
        if (!owner) return;
        NSString *text = error ? script_error_text(error) : json_text(result);
        ca_webview_host_eval_done(owner, on_result, user_data, error == nil, text.UTF8String);
    }];
    return true;
}

void ca_webview_native_focus(Ca_WebViewNative *native)
{
    CaWebViewHost *host = host_of(native);
    if (host.window.isKeyWindow && !host.clip.hidden)
        [host.window makeFirstResponder:host.page];
}

void ca_webview_native_blur(Ca_WebViewNative *native)
{
    CaWebViewHost *host = host_of(native);
    if ([host hasFocus]) [host.window makeFirstResponder:host.window.contentView];
}

bool ca_webview_native_has_focus(const Ca_WebViewNative *native)
{
    return [host_of(native) hasFocus];
}
