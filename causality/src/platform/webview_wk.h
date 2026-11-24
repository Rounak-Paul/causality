// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_wk.h — WKWebView logic shared by the macOS and iOS backends. Platform files subclass
   CaWebKitHost for view hosting, focus and native dialogs. */
#pragma once

#import <WebKit/WebKit.h>

/* QuartzCore defines an unrelated CA_MALLOC attribute macro. */
#undef CA_MALLOC

#include "webview_native.h"

/** Breaks the retain cycle WKUserContentController would otherwise form with its handler. */
@class CaWebKitHost;
@interface CaWebViewMessageProxy : NSObject <WKScriptMessageHandler>
@property (nonatomic, weak) CaWebKitHost *host;
@end

/**
 * Owns the engine objects of one Ca_WebView and translates WebKit callbacks into host events.
 * Subclasses add the platform view tree, focus handling and native dialogs.
 */
@interface CaWebKitHost : NSObject <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic) Ca_WebView *owner;
@property (nonatomic, strong) WKWebView *page;
@property (nonatomic, strong) WKUserContentController *controller;
@property (nonatomic, strong) CaWebViewMessageProxy *proxy;

/** Build a configuration carrying the JavaScript bridge, the init script and developer settings. */
- (WKWebViewConfiguration *)makeConfigurationFor:(const Ca_WebViewDesc *)desc;

/** Take ownership of a page created from makeConfigurationFor: and wire delegates and observers. */
- (void)adoptPage:(WKWebView *)page description:(const Ca_WebViewDesc *)desc;

/** Whether the page (or one of its internal views) is the first responder. */
- (BOOL)hasFocus;

/** Apply a page zoom factor. */
- (void)applyZoom:(double)factor;

/** Publish the page's current state with an event. detail may be nil. */
- (void)publish:(Ca_WebViewEventType)type detail:(NSString *)detail;

/** Detach engine objects and stop callbacks. Subclasses override, release platform views, then call super. */
- (void)teardown;
@end

/** Resolve the host behind an opaque native handle. */
static inline CaWebKitHost *ca_wk_host(const Ca_WebViewNative *native)
{
    return (__bridge CaWebKitHost *)(void *)native;
}
