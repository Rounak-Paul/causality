// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* webview_uikit.m — iOS web view backend. Hosts a WKWebView inside a clipping container that is a
   subview of the UIView backing the Causality window's CAMetalLayer. Shared WebKit logic lives in
   webview_wk.m. The host application needs no extra setup. */

#import <UIKit/UIKit.h>

/* QuartzCore defines an unrelated CA_MALLOC attribute macro. */
#undef CA_MALLOC

#include "webview_backend.h"
#include "webview_wk.h"

/** The UIView backing a window's render layer, or nil (ios_backend.m). */
UIView *ca_ios_window_host_view(const Ca_Window *window);

static const CGFloat kFrameEpsilon = 0.01;

/** Container that clips the page to the visible part of its layout rectangle. */
@interface CaWebViewClip : UIView
@end

@implementation CaWebViewClip
@end

/** iOS host: view tree, focus and native dialogs. */
@interface CaWebViewHost : CaWebKitHost
@property (nonatomic, weak) UIView *hostView;
@property (nonatomic, strong) CaWebViewClip *clip;
- (void)applyFrame:(const Ca_WebViewFrame *)frame;
@end

/** Whether view or any descendant is the first responder. */
static BOOL contains_first_responder(UIView *view)
{
    if (view.isFirstResponder) return YES;
    for (UIView *child in view.subviews)
        if (contains_first_responder(child)) return YES;
    return NO;
}

/** The view controller that should present a dialog over view. */
static UIViewController *presenter_for(UIView *view)
{
    UIViewController *controller = view.window.rootViewController;
    while (controller.presentedViewController) controller = controller.presentedViewController;
    return controller;
}

@implementation CaWebViewHost

- (BOOL)hasFocus
{
    return self.clip && contains_first_responder(self.clip);
}

- (void)applyFrame:(const Ca_WebViewFrame *)frame
{
    if (!self.hostView || !frame->visible) {
        if ([self hasFocus]) [self.clip endEditing:YES];
        self.clip.hidden = YES;
        return;
    }
    self.clip.frame = CGRectMake(frame->clip_x, frame->clip_y, frame->clip_w, frame->clip_h);
    self.page.frame = CGRectMake(frame->x - frame->clip_x, frame->y - frame->clip_y, frame->w, frame->h);
    BOOL unclipped = fabs(frame->clip_x - frame->x) < kFrameEpsilon &&
                     fabs(frame->clip_y - frame->y) < kFrameEpsilon &&
                     fabs(frame->clip_w - frame->w) < kFrameEpsilon &&
                     fabs(frame->clip_h - frame->h) < kFrameEpsilon;
    self.clip.layer.cornerRadius = unclipped ? frame->corner_radius : 0.0;
    self.clip.hidden = NO;
    [self.hostView bringSubviewToFront:self.clip];
}

- (void)teardown
{
    if ([self hasFocus]) [self.clip endEditing:YES];
    [super teardown];
    [self.clip removeFromSuperview];
}

#pragma mark WKUIDelegate dialogs

/** Present alert over the host window; completion runs once it is dismissed. */
- (void)present:(UIAlertController *)alert orFail:(void (^)(void))fail
{
    UIViewController *presenter = presenter_for(self.hostView);
    if (!presenter) {
        fail();
        return;
    }
    [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))handler
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:nil message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) { handler(); }]];
    [self present:alert orFail:handler];
}

- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message
initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))handler
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:nil message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                            handler:^(UIAlertAction *action) { handler(NO); }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) { handler(YES); }]];
    [self present:alert orFail:^{ handler(NO); }];
}

- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
    defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame
completionHandler:(void (^)(NSString *))handler
{
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:nil message:prompt
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) { field.text = defaultText; }];
    __weak UIAlertController *weakAlert = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                            handler:^(UIAlertAction *action) { handler(nil); }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        handler(weakAlert.textFields.firstObject.text ?: @"");
    }]];
    [self present:alert orFail:^{ handler(nil); }];
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
    if (![NSThread isMainThread]) return NULL;
    @autoreleasepool {
        UIView *hostView = ca_ios_window_host_view(window);
        if (!hostView) return NULL;

        CaWebViewHost *host = [CaWebViewHost new];
        host.owner = owner;
        host.hostView = hostView;
        WKWebViewConfiguration *config = [host makeConfigurationFor:desc];

        CaWebViewClip *clip = [[CaWebViewClip alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
        clip.clipsToBounds = YES;
        clip.hidden = YES;

        WKWebView *page = [[WKWebView alloc] initWithFrame:clip.bounds configuration:config];
        page.scrollView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
        if (desc->transparent) {
            page.opaque = NO;
            page.backgroundColor = UIColor.clearColor;
            page.scrollView.backgroundColor = UIColor.clearColor;
        }
        [clip addSubview:page];
        [hostView addSubview:clip];

        host.clip = clip;
        [host adoptPage:page description:desc];
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
    if (!host.clip.hidden) [host.page becomeFirstResponder];
}

void ca_webview_native_blur(Ca_WebViewNative *native)
{
    CaWebViewHost *host = (CaWebViewHost *)ca_wk_host(native);
    if ([host hasFocus]) [host.clip endEditing:YES];
}
