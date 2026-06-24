#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "app_menu.h"
#include "../core/ca_internal.h"

/* ================================================================
   CAUSALITY — native macOS application menu bar
   ================================================================
   Builds NSMenu entries on [NSApp mainMenu] from the deep-copied
   Ca_AppMenu data stored in Ca_Instance.  Called whenever the
   app menus are updated via ca_instance_set_app_menus().
   ================================================================ */

static const NSInteger kCaMenuTag = 0xCA4E01;  /* marks causality-owned items */

/* ---- Action dispatcher ---- */

/*
 * Lightweight Obj-C target that stores a C callback and context pointer.
 * One instance per NSMenuItem that carries a C action.
 */
@interface CaMenuAction : NSObject
@property (nonatomic, assign) Ca_MenuActionFn action;
@property (nonatomic, assign) void           *data;
- (void)fire:(id)sender;
@end

@implementation CaMenuAction
- (void)fire:(id)sender {
    (void)sender;
    if (self.action) self.action(self.data);
}
@end

/* ---- Internal helpers ---- */

static NSMenuItem *make_item(const char *label,
                             const char *key,
                             Ca_MenuActionFn action,
                             void *data)
{
    CaMenuAction *target = [[CaMenuAction alloc] init];
    target.action = action;
    target.data   = data;

    NSMenuItem *item = [[NSMenuItem alloc]
        initWithTitle:@(label)
               action:@selector(fire:)
        keyEquivalent:@(key)];
    item.target = target;
    item.tag    = kCaMenuTag;
    /* Keep target alive for the lifetime of the item via associated object. */
    objc_setAssociatedObject(item, "ca_action", target,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return item;
}

/*
 * Build an NSMenu from a Ca_AppMenu descriptor.
 */
static NSMenu *build_nsmenu(const Ca_AppMenu *m)
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@(m->label)];
    for (int i = 0; i < m->item_count; i++) {
        const Ca_AppMenuItem *ci = &m->items[i];
        NSMenuItem *ni;
        if (ci->separator) {
            ni = [NSMenuItem separatorItem];
            ni.tag = kCaMenuTag;
        } else if (ci->sub_item_count > 0) {
            /* Sub-menu */
            Ca_AppMenu sub = { .item_count = ci->sub_item_count };
            snprintf(sub.label, sizeof(sub.label), "%s", ci->label);
            for (int k = 0; k < ci->sub_item_count; k++) {
                Ca_AppMenuItem *si = &sub.items[k];
                snprintf(si->label, sizeof(si->label), "%s", ci->sub_items[k].label);
                si->action        = ci->sub_items[k].action;
                si->action_data   = ci->sub_items[k].action_data;
                si->separator     = ci->sub_items[k].separator;
                si->sub_item_count = 0;
            }
            NSMenu *submenu = build_nsmenu(&sub);
            ni = [[NSMenuItem alloc] initWithTitle:@(ci->label)
                                           action:nil keyEquivalent:@""];
            ni.submenu = submenu;
            ni.tag     = kCaMenuTag;
        } else {
            ni = make_item(ci->label[0] ? ci->label : "(unnamed)",
                           "",
                           ci->action,
                           ci->action_data);
        }
        [menu addItem:ni];
    }
    return menu;
}

/*
 * Remove all causality-owned top-level menu items from [NSApp mainMenu].
 */
static void remove_ca_menus(void)
{
    NSMenu *bar = [NSApp mainMenu];
    NSArray<NSMenuItem *> *items = [bar.itemArray copy];
    for (NSInteger i = (NSInteger)items.count - 1; i >= 0; i--) {
        if (items[(NSUInteger)i].tag == kCaMenuTag)
            [bar removeItemAtIndex:i];
    }
}

/* ---- Platform interface ---- */

/*
 * Rebuilds the causality-owned top-level menus in [NSApp mainMenu].
 *
 * Removes any previously installed causality menus, then appends fresh
 * NSMenu items built from instance->app_menus[].
 *
 * instance  Ca_Instance whose app_menus[] have already been updated.
 */
void ca_app_menu_set(Ca_Instance *instance)
{
    remove_ca_menus();

    NSMenu *bar = [NSApp mainMenu];
    for (int i = 0; i < instance->app_menu_count; i++) {
        const Ca_AppMenu *m = &instance->app_menus[i];
        NSMenu     *sub = build_nsmenu(m);
        NSMenuItem *hdr = [[NSMenuItem alloc]
            initWithTitle:@(m->label) action:nil keyEquivalent:@""];
        hdr.submenu = sub;
        hdr.tag     = kCaMenuTag;
        [bar addItem:hdr];
    }
}

#endif /* __APPLE__ */
