/*
 * All platforms use the custom Causality title bar to host the menu bar.
 * Forward instance-level menus to every open window's title bar.
 */

#include "app_menu.h"
#include "../core/ca_internal.h"
#include "../ui/menu_storage.h"
#include "causality.h"

void ca_app_menu_set(Ca_Instance *instance)
{
    if (!instance) return;

    for (size_t wi = 0; wi < ca_pool_slot_count(&instance->windows); ++wi) {
        Ca_Window *win = CA_POOL_AT(instance->windows, Ca_Window, wi);
        if (!win->in_use) continue;
        if (ca_menu_storage_copy(&win->titlebar_menu_storage,
                                 &win->titlebar_menus,
                                 instance->app_menus,
                                 (size_t)instance->app_menu_count)) {
            win->titlebar_menu_count = instance->app_menu_count;
            win->titlebar_needs_rebuild = true;
        }
    }
}
