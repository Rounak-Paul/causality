// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#undef CA_MALLOC
#include "ca_ios.h"
#include "platform.h"
#include "renderer.h"
#include "window.h"
#include "widget.h"
#include <limits.h>

typedef struct {
    Ca_IOSPointerPhase phase;
    double x, y, dx, dy;
    bool scroll;
} Ca_IOSPointer;

@interface CaIOSEditor : UITextField <UITextFieldDelegate>
@property(nonatomic, assign) Ca_Window *caWindow;
@property(nonatomic, assign) Ca_TextInput *input;
@property(nonatomic) BOOL synchronizing;
@end

@interface CaIOSWindowState : NSObject {
@public
    CAMetalLayer *layer;
    NSString *clipboard_read;
    CaIOSEditor *editor;
    Ca_DynArray events;
    int width, height, drawable_width, drawable_height;
    float scale;
    bool geometry_pending, tracking, hide_pointer, cancel_pending, keyboard_requested, return_pending;
}
@end

@implementation CaIOSWindowState
- (void)dealloc
{
    ca_dyn_array_destroy(&events);
}
@end

/** Retrieve the iOS state for a live hosted window. */
static CaIOSWindowState *state_for(const Ca_Window *window)
{
    return window && window->in_use ? (__bridge CaIOSWindowState *)window->platform_handle : nil;
}

/** Returns the editable input if its entire ancestor chain is enabled and visible. */
static Ca_TextInput *focused_input(Ca_Window *window)
{
    Ca_Node *node = window->focused_node;
    if (!node || node->widget_type != CA_WIDGET_TEXT_INPUT) return NULL;
    for (Ca_Node *ancestor = node; ancestor; ancestor = ancestor->parent)
        if (!ancestor->in_use || ancestor->desc.hidden || ancestor->desc.disabled) return NULL;
    Ca_TextInput *input = node->widget;
    return input && input->in_use && !input->multiline ? input : NULL;
}

@implementation CaIOSEditor

/** Leaves hit testing to the Causality surface. */
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event
{
    (void)point; (void)event;
    return NO;
}

/** Reject edits exceeding the widget's UTF-8 byte limit. */
- (BOOL)textField:(UITextField *)field shouldChangeCharactersInRange:(NSRange)range
    replacementString:(NSString *)replacement
{
    if (!self.input || self.input != focused_input(self.caWindow)) return NO;
    NSString *text = [field.text stringByReplacingCharactersInRange:range withString:replacement];
    if ([text rangeOfCharacterFromSet:NSCharacterSet.newlineCharacterSet].location != NSNotFound)
        return NO;
    return field.markedTextRange || [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= self.input->max_length;
}

/** Mirrors committed native text and selection into the focused widget. */
- (void)publish
{
    Ca_TextInput *input = self.input;
    if (self.synchronizing || self.markedTextRange || !input || input != focused_input(self.caWindow))
        return;
    const char *text = self.text.UTF8String;
    if (!text || [self.text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > input->max_length) return;

    bool changed = strcmp(ca_input_text(input), text) != 0;
    if (changed) ca_set_text(input, text);
    UITextRange *selection = self.selectedTextRange;
    if (selection) {
        NSInteger start = [self offsetFromPosition:self.beginningOfDocument toPosition:selection.start];
        NSInteger end = [self offsetFromPosition:self.beginningOfDocument toPosition:selection.end];
        if (start >= 0 && end >= start && (NSUInteger)end <= self.text.length) {
            int cursor = (int)[[self.text substringToIndex:(NSUInteger)end] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            int anchor = start == end ? -1 :
                (int)[[self.text substringToIndex:(NSUInteger)start] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            if (input->cursor != cursor || input->sel_start != anchor)
                input->node->dirty |= CA_DIRTY_CONTENT;
            input->cursor = cursor;
            input->sel_start = anchor;
        }
    }
    if (changed && input->on_change) input->on_change(input, input->change_data);
}

/** Mirrors committed text from typing, paste, and hardware keyboard input. */
- (void)editingChanged:(UITextField *)field
{
    (void)field;
    [self publish];
}

/** Mirrors selection changes and input method commits. */
- (void)textFieldDidChangeSelection:(UITextField *)field
{
    (void)field;
    [self publish];
}

/** Queues Return for the same widget and app handlers used on desktop. */
- (BOOL)textFieldShouldReturn:(UITextField *)field
{
    (void)field;
    [self publish];
    CaIOSWindowState *state = state_for(self.caWindow);
    if (state && self.input && self.input == focused_input(self.caWindow)) state->return_pending = true;
    return NO;
}
@end

/** Match drawable pixels to the layer's current point bounds and backing scale. */
static bool update_geometry(CaIOSWindowState *state)
{
    CAMetalLayer *layer = state->layer;
    if (!layer) return false;
    CGSize size = layer.bounds.size;
    CGFloat scale = layer.contentsScale;
    if (!(scale > 0.0) || !isfinite(scale)) scale = 1.0;
    if (!(size.width >= 1.0 && size.height >= 1.0) || size.width > INT_MAX || size.height > INT_MAX ||
        size.width * scale > INT_MAX || size.height * scale > INT_MAX) return false;

    const int drawable_width = (int)lround(size.width * scale);
    const int drawable_height = (int)lround(size.height * scale);
    if (drawable_width <= 0 || drawable_height <= 0) return false;
    state->width = (int)lround(size.width);
    state->height = (int)lround(size.height);
    state->drawable_width = drawable_width;
    state->drawable_height = drawable_height;
    state->scale = (float)scale;
    CGSize drawable = CGSizeMake(state->drawable_width, state->drawable_height);
    if (!CGSizeEqualToSize(layer.drawableSize, drawable)) layer.drawableSize = drawable;
    return true;
}

/** Discard a cancelled gesture without activating a widget at its endpoint. */
static void cancel_pointer(Ca_Window *window, CaIOSWindowState *state)
{
    window->mouse_click_this_frame = false;
    state->keyboard_requested = false;
    memset(window->mouse_buttons, 0, sizeof window->mouse_buttons);
    ca_window_input_leave(window);
    state->hide_pointer = false;
}

bool ca_platform_init(void)
{
    return NSThread.isMainThread;
}

void ca_platform_shutdown(void) {}

double ca_platform_time(void) { return CACurrentMediaTime(); }

void ca_platform_wake(void) { CFRunLoopWakeUp(CFRunLoopGetMain()); }

void ca_platform_process_events(Ca_Instance *instance, double timeout)
{
    (void)timeout;
    for (size_t i = 0; i < ca_pool_slot_count(&instance->windows); ++i) {
        Ca_Window *window = CA_POOL_AT(instance->windows, Ca_Window, i);
        CaIOSWindowState *state = state_for(window);
        if (!state || !state->layer) continue;
        if (state->geometry_pending) {
            int width = state->width, height = state->height;
            int drawable_w = state->drawable_width, drawable_h = state->drawable_height;
            state->geometry_pending = false;
            if (update_geometry(state)) {
                if (width != state->width || height != state->height)
                    ca_window_handle_resize(window, state->width, state->height);
                else if (drawable_w != state->drawable_width || drawable_h != state->drawable_height)
                    ca_window_handle_framebuffer_resize(window, state->drawable_width, state->drawable_height);
            }
        }
        if (state->cancel_pending) {
            cancel_pointer(window, state);
            state->cancel_pending = false;
        }
        if (state->hide_pointer) {
            ca_window_input_leave(window);
            state->hide_pointer = false;
        }
        for (size_t j = 0; j < state->events.count; ++j) {
            const Ca_IOSPointer *event = ca_dyn_array_at_const(&state->events, j);
            if (event->phase == CA_IOS_POINTER_CANCEL) {
                cancel_pointer(window, state);
                continue;
            }
            ca_window_input_move(window, event->x, event->y);
            if (event->scroll) {
                ca_window_input_scroll(window, event->dx, event->dy);
                state->hide_pointer = true;
                continue;
            }
            state->hide_pointer = event->phase == CA_IOS_POINTER_END;
            if (event->phase == CA_IOS_POINTER_BEGIN)
                state->keyboard_requested = true;
            if (event->phase == CA_IOS_POINTER_BEGIN || event->phase == CA_IOS_POINTER_END)
                ca_window_input_button(window, 0, state->hide_pointer ? CA_RELEASE : CA_PRESS,
                                       0, event->x, event->y);
        }
        ca_dyn_array_clear(&state->events);
        if (state->return_pending) {
            state->return_pending = false;
            if (state->editor.input && state->editor.input == focused_input(window))
                ca_window_input_key(window, CA_KEY_ENTER, 0, CA_PRESS, 0);
        }
    }
}

bool ca_platform_window_create(Ca_Window *window, const Ca_WindowDesc *desc, void *host_surface)
{
    (void)desc;
    if (!host_surface) return false;
    CaIOSWindowState *state = [CaIOSWindowState new];
    if (!state || !ca_dyn_array_init(&state->events, sizeof(Ca_IOSPointer))) return false;
    state->layer = (__bridge CAMetalLayer *)host_surface;
    if (!update_geometry(state)) return false;
    window->platform_handle = (__bridge_retained void *)state;
    window->mouse_x = window->mouse_y = -1.0;
    return true;
}

void ca_platform_window_destroy(Ca_Window *window)
{
    ca_ios_window_sync_keyboard(window, nil);
    CFBridgingRelease(window->platform_handle);
    window->platform_handle = NULL;
}

Ca_Window *ca_ios_window_create(Ca_Instance *instance, CAMetalLayer *layer,
                               const Ca_WindowDesc *desc)
{
    if (!instance || !layer || !NSThread.isMainThread) return NULL;
    Ca_WindowDesc hosted = desc ? *desc : (Ca_WindowDesc){0};
    hosted.native_frame = true;
    return ca_window_create_hosted(instance, &hosted, (__bridge void *)layer);
}

bool ca_ios_window_set_layer(Ca_Window *window, CAMetalLayer *layer)
{
    if (!NSThread.isMainThread) return false;
    CaIOSWindowState *state = state_for(window);
    if (!state) return false;
    if (state->layer == layer) {
        ca_ios_window_layout_changed(window);
        return true;
    }
    Ca_Instance *instance = window->instance;
    ca_ios_window_sync_keyboard(window, nil);
    ca_renderer_window_detach(instance, window);
    cancel_pointer(window, state);
    ca_window_clear_focus(window);
    state->tracking = state->cancel_pending = false;
    ca_dyn_array_clear(&state->events);
    state->layer = layer;
    if (!layer) return true;
    if (!update_geometry(state) || !ca_renderer_window_init(instance, window)) {
        ca_renderer_window_detach(instance, window);
        state->layer = nil;
        return false;
    }
    if (window->root) window->root->dirty |= CA_DIRTY_LAYOUT | CA_DIRTY_CONTENT;
    window->needs_render = true;
    return true;
}

void ca_ios_window_layout_changed(Ca_Window *window)
{
    CaIOSWindowState *state = state_for(window);
    if (state) state->geometry_pending = true;
}

void ca_ios_window_pointer(Ca_Window *window, Ca_IOSPointerPhase phase, double x, double y)
{
    CaIOSWindowState *state = state_for(window);
    if (!state || !state->layer || phase < CA_IOS_POINTER_BEGIN || phase > CA_IOS_POINTER_CANCEL ||
        !isfinite(x) || !isfinite(y)) return;
    const bool beginning = phase == CA_IOS_POINTER_BEGIN;
    if (beginning ? state->tracking : !state->tracking) return;
    if (!ca_dyn_array_push(&state->events, &(Ca_IOSPointer){.phase = phase, .x = x, .y = y})) {
        ca_dyn_array_clear(&state->events);
        state->tracking = false;
        state->cancel_pending = true;
        return;
    }
    state->tracking = phase == CA_IOS_POINTER_BEGIN || phase == CA_IOS_POINTER_MOVE;
}

void ca_ios_window_scroll(Ca_Window *window, double x, double y, double dx, double dy)
{
    CaIOSWindowState *state = state_for(window);
    if (!state || !state->layer || !isfinite(x) || !isfinite(y) || !isfinite(dx) || !isfinite(dy))
        return;
    ca_dyn_array_push(&state->events, &(Ca_IOSPointer){
        .x = x, .y = y, .dx = dx / CA_WHEEL_SCROLL_PX, .dy = dy / CA_WHEEL_SCROLL_PX, .scroll = true});
}

bool ca_ios_window_splitter_at(Ca_Window *window, double x, double y)
{
    return state_for(window) && isfinite(x) && isfinite(y) &&
           ca_widget_splitter_hit_at(window, (float)x, (float)y);
}

void ca_ios_window_sync_keyboard(Ca_Window *window, UIView *view)
{
    CaIOSWindowState *state = state_for(window);
    if (!state) return;
    CaIOSEditor *editor = state->editor;
    Ca_TextInput *input = view.window ? focused_input(window) : NULL;
    bool requested = state->keyboard_requested || editor.isFirstResponder;
    state->keyboard_requested = false;
    if (!input || !requested) {
        if (editor.input) {
            editor.input = NULL;
            [editor resignFirstResponder];
            editor.text = @"";
        }
        if (!view) {
            editor.caWindow = NULL;
            [editor removeFromSuperview];
            state->editor = nil;
            state->return_pending = false;
        }
        return;
    }

    if (!editor) {
        editor = [[CaIOSEditor alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
        editor.caWindow = window;
        editor.delegate = editor;
        editor.textColor = UIColor.clearColor;
        editor.tintColor = UIColor.clearColor;
        editor.userInteractionEnabled = YES;
        editor.isAccessibilityElement = NO;
        editor.autocapitalizationType = UITextAutocapitalizationTypeNone;
        editor.autocorrectionType = UITextAutocorrectionTypeNo;
        editor.spellCheckingType = UITextSpellCheckingTypeNo;
        editor.returnKeyType = UIReturnKeyGo;
        [editor addTarget:editor action:@selector(editingChanged:) forControlEvents:UIControlEventEditingChanged];
        state->editor = editor;
    }
    if (editor.superview != view) [view addSubview:editor];
    bool switched = editor.input != input;
    editor.input = input;
    if (!switched && editor.markedTextRange) return;
    bool secure = input->input_mode == CA_INPUT_PASSWORD && !input->password_visible;
    NSString *text = [NSString stringWithUTF8String:ca_input_text(input)] ?: @"";
    bool replace = switched || editor.isSecureTextEntry != secure || ![editor.text isEqualToString:text];
    editor.synchronizing = YES;
    if (editor.isSecureTextEntry != secure) editor.secureTextEntry = secure;
    if (replace) editor.text = text;
    if (!editor.isFirstResponder && ![editor becomeFirstResponder])
        state->keyboard_requested = true;
    const char *utf8 = ca_input_text(input);
    int cursor = input->cursor;
    int anchor = input->sel_start < 0 ? cursor : input->sel_start;
    int start = cursor < anchor ? cursor : anchor;
    int end = cursor > anchor ? cursor : anchor;
    if (start >= 0 && (size_t)end <= strlen(utf8)) {
        NSString *prefix = [[NSString alloc] initWithBytes:utf8 length:(size_t)start encoding:NSUTF8StringEncoding];
        NSString *through = start == end ? prefix :
            [[NSString alloc] initWithBytes:utf8 length:(size_t)end encoding:NSUTF8StringEncoding];
        if (prefix && through) {
            UITextPosition *from = [editor positionFromPosition:editor.beginningOfDocument offset:(NSInteger)prefix.length];
            UITextPosition *to = [editor positionFromPosition:editor.beginningOfDocument offset:(NSInteger)through.length];
            UITextRange *current = editor.selectedTextRange;
            if (from && to && (!current || [editor comparePosition:current.start toPosition:from] != NSOrderedSame ||
                              [editor comparePosition:current.end toPosition:to] != NSOrderedSame))
                editor.selectedTextRange = [editor textRangeFromPosition:from toPosition:to];
        }
    }
    editor.synchronizing = NO;
}

void ca_platform_window_size(const Ca_Window *window, int *width, int *height)
{
    CaIOSWindowState *state = state_for(window);
    if (width) *width = state ? state->width : 0;
    if (height) *height = state ? state->height : 0;
}

void ca_platform_framebuffer_size(const Ca_Window *window, int *width, int *height)
{
    CaIOSWindowState *state = state_for(window);
    if (width) *width = state && state->layer ? state->drawable_width : 0;
    if (height) *height = state && state->layer ? state->drawable_height : 0;
}

void ca_platform_content_scale(const Ca_Window *window, float *x, float *y)
{
    CaIOSWindowState *state = state_for(window);
    float scale = state ? state->scale : 1.0f;
    if (x) *x = scale;
    if (y) *y = scale;
}

const char **ca_platform_vk_extensions(uint32_t *count)
{
    static const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
    *count = sizeof extensions / sizeof *extensions;
    return extensions;
}

bool ca_platform_vk_presentation_supported(VkInstance instance, VkPhysicalDevice device,
                                            uint32_t queue_family, VkQueueFlags flags)
{
    (void)instance; (void)device; (void)queue_family;
    return (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
}

VkResult ca_platform_vk_surface_create(VkInstance instance, const Ca_Window *window,
                                        VkSurfaceKHR *surface)
{
    CaIOSWindowState *state = state_for(window);
    if (!state || !state->layer) return VK_ERROR_INITIALIZATION_FAILED;
    return vkCreateMetalSurfaceEXT(instance, &(VkMetalSurfaceCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, .pLayer = state->layer}, NULL, surface);
}

void ca_window_get_rect(const Ca_Window *window, int *x, int *y, int *width, int *height)
{
    CaIOSWindowState *state = state_for(window);
    CGPoint origin = state && state->layer ? [state->layer convertPoint:CGPointZero toLayer:nil] : CGPointZero;
    if (x) *x = (int)origin.x;
    if (y) *y = (int)origin.y;
    ca_platform_window_size(window, width, height);
}

bool ca_window_focused(const Ca_Window *window)
{
    CaIOSWindowState *state = state_for(window);
    return state && state->layer && UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
}

bool ca_window_mouse_button_down(const Ca_Window *window, int button)
{
    return window && window->in_use && button >= 0 && button < 3 && window->mouse_buttons[button];
}

bool ca_platform_primary_button_held(Ca_Window *window)
{
    return ca_window_mouse_button_down(window, 0);
}

bool ca_window_cursor_screen_position(const Ca_Window *window, float *x, float *y)
{
    if (!window || !window->in_use || window->mouse_x < 0 || window->mouse_y < 0) return false;
    int origin_x, origin_y;
    ca_window_get_rect(window, &origin_x, &origin_y, NULL, NULL);
    if (x) *x = (float)(origin_x + window->mouse_x);
    if (y) *y = (float)(origin_y + window->mouse_y);
    return true;
}

void ca_clipboard_set_text(Ca_Window *window, const char *text)
{
    CaIOSWindowState *state = state_for(window);
    if (!state) return;
    state->clipboard_read = nil;
    UIPasteboard.generalPasteboard.string = text ? [NSString stringWithUTF8String:text] : @"";
}

const char *ca_clipboard_get_text(Ca_Window *window)
{
    CaIOSWindowState *state = state_for(window);
    if (!state) return NULL;
    state->clipboard_read = UIPasteboard.generalPasteboard.string;
    return state->clipboard_read.UTF8String;
}

void ca_window_set_opacity(Ca_Window *window, float opacity)
{
    CaIOSWindowState *state = state_for(window);
    if (state) state->layer.opacity = fminf(1.0f, fmaxf(opacity, 0.0f));
}

/* UIKit owns scene placement and chrome. Pointer input uses the hosted view. */
void ca_window_focus(Ca_Window *window) { (void)window; }
void ca_window_set_position(Ca_Window *window, int x, int y) { (void)window; (void)x; (void)y; }
void ca_window_maximize(Ca_Window *window) { (void)window; }
void ca_window_restore(Ca_Window *window) { (void)window; }
void ca_window_cancel_move(Ca_Window *window) { (void)window; }
bool ca_window_key_down(const Ca_Window *window, int key) { (void)window; (void)key; return false; }
void ca_platform_window_focus(Ca_Window *window) { (void)window; }
void ca_platform_window_set_title(Ca_Window *window, const char *title) { (void)window; (void)title; }
void ca_platform_window_minimize(Ca_Window *window) { (void)window; }
void ca_platform_window_set_drag_preview(Ca_Window *window) { (void)window; }
void ca_platform_window_resize_pass(Ca_Window *window) { (void)window; }
bool ca_platform_titlebar_drag_pass(Ca_Window *window) { (void)window; return false; }
void ca_platform_set_horizontal_drag_cursor(Ca_Window *window) { (void)window; }
void ca_platform_set_default_cursor(Ca_Window *window) { (void)window; }
