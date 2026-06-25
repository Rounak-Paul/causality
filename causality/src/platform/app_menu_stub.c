/*
 * Non-Apple stub for the platform app-menu interface.
 *
 * All platforms use the custom Causality title bar to host the menu bar.
 * Forward instance-level menus to every open window's title bar.
 */
#ifndef __APPLE__

#include "app_menu.h"
#include "../core/ca_internal.h"
#include "causality.h"

void ca_app_menu_set(Ca_Instance *instance)
{
    if (!instance || instance->app_menu_count <= 0) return;

    Ca_MenuDesc      menus[CA_MAX_APP_MENUS];
    Ca_MenuItemDesc  items[CA_MAX_APP_MENUS][CA_MAX_APP_MENU_ITEMS];

    int count = instance->app_menu_count;
    for (int mi = 0; mi < count; mi++) {
        const Ca_AppMenu *src = &instance->app_menus[mi];
        menus[mi].label      = src->label;
        menus[mi].item_count = src->item_count;
        menus[mi].items      = items[mi];
        for (int ii = 0; ii < src->item_count; ii++) {
            const Ca_AppMenuItem *si = &src->items[ii];
            items[mi][ii].label          = si->label;
            items[mi][ii].action         = si->action;
            items[mi][ii].action_data    = si->action_data;
            items[mi][ii].separator      = si->separator;
            items[mi][ii].sub_items      = NULL;
            items[mi][ii].sub_item_count = 0;
        }
    }

    for (int wi = 0; wi < CA_MAX_WINDOWS_TOTAL; wi++) {
        Ca_Window *win = &instance->windows[wi];
        if (win->in_use)
            ca_window_set_title_bar_menus(win, menus, count);
    }
}

#endif /* !__APPLE__ */
