// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_wk.m — WKWebView logic shared by the macOS and iOS backends: configuration, JavaScript
   bridge, state observation, navigation policy, loading, evaluation and the common native entry
   points. Platform files provide create/destroy, frame placement, focus, pump and dialogs. */

#import <Foundation/Foundation.h>

#include "webview_wk.h"

static NSString *const kBridgeName = @"causality";
static NSString *const kBridgeScript =
    @"Object.defineProperty(window,'causality',{value:Object.freeze({"
     "postMessage:function(m){window.webkit.messageHandlers.causality.postMessage("
     "typeof m==='string'?m:JSON.stringify(m));}}),configurable:false});";

static void *const kObserverContext = (void *)&kObserverContext;

@implementation CaWebViewMessageProxy
- (void)userContentController:(WKUserContentController *)controller
      didReceiveScriptMessage:(WKScriptMessage *)message
{
    CaWebKitHost *host = self.host;
    if (!host || !message.frameInfo.isMainFrame || ![message.body isKindOfClass:[NSString class]])
        return;
    [host publish:CA_WEBVIEW_EVENT_MESSAGE detail:message.body];
}
@end

@implementation CaWebKitHost

+ (NSArray<NSString *> *)observedKeys
{
    return @[@"title", @"URL", @"estimatedProgress", @"canGoBack", @"canGoForward"];
}

- (WKWebViewConfiguration *)makeConfigurationFor:(const Ca_WebViewDesc *)desc
{
    WKWebViewConfiguration *config = [WKWebViewConfiguration new];
    WKUserContentController *controller = config.userContentController;
    CaWebViewMessageProxy *proxy = [CaWebViewMessageProxy new];
    proxy.host = self;
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
    self.controller = controller;
    self.proxy = proxy;
    return config;
}

- (void)adoptPage:(WKWebView *)page description:(const Ca_WebViewDesc *)desc
{
    page.navigationDelegate = self;
    page.UIDelegate = self;
    if (desc->user_agent) {
        NSString *agent = [NSString stringWithUTF8String:desc->user_agent];
        if (agent) page.customUserAgent = agent;
    }
    if (desc->devtools) {
        if (@available(macOS 13.3, iOS 16.4, *)) page.inspectable = YES;
    }
    self.page = page;
    for (NSString *key in [CaWebKitHost observedKeys])
        [page addObserver:self forKeyPath:key options:0 context:kObserverContext];
}

- (BOOL)hasFocus
{
    return NO;
}

- (void)applyZoom:(double)factor
{
    if (@available(macOS 11.0, iOS 14.0, *)) self.page.pageZoom = factor;
}

- (void)publish:(Ca_WebViewEventType)type detail:(NSString *)detail
{
    /* A callback may remove the web view, which releases this host mid-call. */
    CaWebKitHost *guard __attribute__((objc_precise_lifetime)) = self;
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

- (void)teardown
{
    self.owner = NULL;
    for (NSString *key in [CaWebKitHost observedKeys])
        [self.page removeObserver:self forKeyPath:key context:kObserverContext];
    self.page.navigationDelegate = nil;
    self.page.UIDelegate = nil;
    [self.page stopLoading];
    [self.controller removeScriptMessageHandlerForName:kBridgeName];
    [self.controller removeAllUserScripts];
    self.proxy.host = nil;
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
    CaWebKitHost *guard __attribute__((objc_precise_lifetime)) = self;
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

void ca_webview_native_destroy(Ca_WebViewNative *native)
{
    if (!native) return;
    CaWebKitHost *host = (__bridge_transfer CaWebKitHost *)native;
    [host teardown];
}

bool ca_webview_native_load_url(Ca_WebViewNative *native, const char *url)
{
    NSString *text = [NSString stringWithUTF8String:url];
    NSURL *target = text ? [NSURL URLWithString:text] : nil;
    if (!target || target.scheme.length == 0) return false;
    WKWebView *page = ca_wk_host(native).page;
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
    return [ca_wk_host(native).page loadHTMLString:source baseURL:baseURL] != nil;
}

void ca_webview_native_reload(Ca_WebViewNative *native)
{
    [ca_wk_host(native).page reload];
}

void ca_webview_native_stop(Ca_WebViewNative *native)
{
    [ca_wk_host(native).page stopLoading];
}

void ca_webview_native_go_back(Ca_WebViewNative *native)
{
    [ca_wk_host(native).page goBack];
}

void ca_webview_native_go_forward(Ca_WebViewNative *native)
{
    [ca_wk_host(native).page goForward];
}

void ca_webview_native_set_zoom(Ca_WebViewNative *native, double factor)
{
    [ca_wk_host(native) applyZoom:factor];
}

bool ca_webview_native_eval(Ca_WebViewNative *native, const char *script,
                            Ca_WebViewEvalFn on_result, void *user_data)
{
    NSString *source = [NSString stringWithUTF8String:script];
    if (!source) return false;
    CaWebKitHost *host = ca_wk_host(native);
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

bool ca_webview_native_has_focus(const Ca_WebViewNative *native)
{
    return [ca_wk_host(native) hasFocus];
}
