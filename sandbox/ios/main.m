// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include "ca_ios.h"
#include <stdio.h>

typedef struct {
    unsigned taps;
    Ca_Label *counter;
} Sandbox;

/** Show that widget input and state survive scene and surface changes. */
static void count_tap(Ca_Button *button, void *data)
{
    (void)button;
    Sandbox *sandbox = data;
    char value[32];
    snprintf(value, sizeof value, "Taps %u", ++sandbox->taps);
    ca_set_text(sandbox->counter, value);
}

@interface CaSandboxView : UIView
@property(nonatomic, assign) Ca_Window *caWindow;
@end

@implementation CaSandboxView
+ (Class)layerClass { return CAMetalLayer.class; }

- (void)forwardTouches:(NSSet<UITouch *> *)touches phase:(Ca_IOSPointerPhase)phase
{
    UITouch *touch = touches.anyObject;
    if (!touch) return;
    CGPoint point = [touch locationInView:self];
    ca_ios_window_pointer(self.caWindow, phase, point.x, point.y);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ (void)event; [self forwardTouches:touches phase:CA_IOS_POINTER_BEGIN]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ (void)event; [self forwardTouches:touches phase:CA_IOS_POINTER_MOVE]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ (void)event; [self forwardTouches:touches phase:CA_IOS_POINTER_END]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ (void)event; [self forwardTouches:touches phase:CA_IOS_POINTER_CANCEL]; }
@end

@interface CaSandboxController : UIViewController
- (void)resume;
- (void)pause;
- (void)detach;
- (void)shutdown;
@end

@implementation CaSandboxController {
    CaSandboxView *_surface;
    CADisplayLink *_frames;
    Ca_Instance *_instance;
    Ca_Stylesheet *_stylesheet;
    Sandbox _sandbox;
    bool _active, _attached, _failed;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithWhite:0.09 alpha:1.0];
    _surface = [CaSandboxView new];
    _surface.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_surface];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_surface.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_surface.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_surface.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [_surface.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
    ]];
}

- (void)buildUI
{
    _stylesheet = ca_css_parse(
        ".sandbox { background: #171717; align-items: center; justify-content: center; gap: 16px; }"
        ".title { color: #f1f1f1; font-size: 30px; font-weight: bold; }"
        ".counter { color: #adadad; font-size: 17px; }"
        ".tap { width: 160px; height: 52px; background: #dddddd; color: #171717; border-radius: 12px; }"
    );
    if (_stylesheet) ca_instance_set_stylesheet(_instance, _stylesheet);
    ca_ui_begin(_surface.caWindow, &(Ca_DivDesc){.direction = CA_VERTICAL, .style = "sandbox"});
    ca_text(&(Ca_TextDesc){.text = "Causality", .style = "title"});
    _sandbox.counter = ca_text(&(Ca_TextDesc){.text = "Taps 0", .style = "counter"});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Tap", .style = "tap", .on_click = count_tap, .click_data = &_sandbox});
    ca_btn_end();
    ca_ui_end();
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    if (!_active || _failed || CGRectIsEmpty(_surface.bounds)) return;
    CAMetalLayer *layer = (CAMetalLayer *)_surface.layer;
    CGFloat scale = _surface.traitCollection.displayScale;
    if (scale > 0 && layer.contentsScale != scale) layer.contentsScale = scale;
    if (!_instance) {
        NSURL *cache = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory
                                                         inDomains:NSUserDomainMask].firstObject;
        NSURL *shaders = [cache URLByAppendingPathComponent:@"CausalityShaders" isDirectory:YES];
        _instance = ca_instance_create(&(Ca_InstanceDesc){
            .app_name = "Causality iOS Sandbox", .shader_cache_dir = shaders.fileSystemRepresentation});
        if (_instance) _surface.caWindow = ca_ios_window_create(_instance, layer, NULL);
        if (!_surface.caWindow) {
            _failed = true;
            NSLog(@"Causality could not initialize the iOS renderer");
            return;
        }
        [self buildUI];
        _attached = true;
    } else if (!_attached) {
        _attached = ca_ios_window_set_layer(_surface.caWindow, layer);
        if (!_attached) return;
    }
    ca_ios_window_layout_changed(_surface.caWindow);
    if (!_frames) {
        _frames = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
        [_frames addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
}

- (void)frame:(CADisplayLink *)link
{
    (void)link;
    if (!ca_instance_step(_instance)) [self pause];
}

- (void)resume
{
    _active = true;
    [self.view setNeedsLayout];
}

- (void)pause
{
    _active = false;
    [_frames invalidate];
    _frames = nil;
}

- (void)detach
{
    [self pause];
    if (_attached) ca_ios_window_set_layer(_surface.caWindow, nil);
    _attached = false;
}

- (void)shutdown
{
    [self pause];
    _surface.caWindow = NULL;
    ca_instance_destroy(_instance);
    _instance = NULL;
    if (_stylesheet) ca_css_destroy(_stylesheet);
    _stylesheet = NULL;
    _attached = false;
}
@end

@interface CaSandboxScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation CaSandboxScene {
    CaSandboxController *_controller;
}

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options
{
    (void)session; (void)options;
    _controller = [CaSandboxController new];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = _controller;
    [self.window makeKeyAndVisible];
}

- (void)sceneDidBecomeActive:(UIScene *)scene { (void)scene; [_controller resume]; }
- (void)sceneWillResignActive:(UIScene *)scene { (void)scene; [_controller pause]; }
- (void)sceneDidEnterBackground:(UIScene *)scene { (void)scene; [_controller detach]; }
- (void)sceneDidDisconnect:(UIScene *)scene { (void)scene; [_controller shutdown]; }
@end

@interface CaSandboxApp : UIResponder <UIApplicationDelegate>
@end
@implementation CaSandboxApp
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(CaSandboxApp.class));
    }
}
