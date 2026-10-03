// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#include "menu_storage.h"

#include <limits.h>
#include <string.h>

/** Releases nested sub-item storage owned by one menu item. */
static void menu_item_destroy(Ca_MenuBarItem *item)
{
    if (!item) return;
    ca_dyn_array_destroy(&item->sub_item_storage);
    item->sub_items = NULL;
    item->sub_item_count = 0;
}

/** Releases nested item storage owned by one menu. */
static void menu_destroy(Ca_MenuBarMenu *menu)
{
    if (!menu) return;
    for (size_t i = 0; i < menu->item_storage.count; ++i)
        menu_item_destroy(ca_dyn_array_at(&menu->item_storage, i));
    ca_dyn_array_destroy(&menu->item_storage);
    menu->items = NULL;
    menu->item_count = 0;
}

bool ca_menu_sub_item_storage_resize(Ca_MenuBarItem *item, size_t count)
{
    if (!item || count > INT_MAX) return false;
    if (item->sub_item_storage.element_size == 0 &&
        !ca_dyn_array_init(&item->sub_item_storage,
                           sizeof(Ca_MenuBarSubItem)))
        return false;
    if (!ca_dyn_array_resize(&item->sub_item_storage, count)) return false;
    item->sub_items = item->sub_item_storage.data;
    item->sub_item_count = (int)count;
    return true;
}

bool ca_menu_item_storage_resize(Ca_MenuBarMenu *menu, size_t count)
{
    if (!menu || count > INT_MAX) return false;
    if (menu->item_storage.element_size == 0 &&
        !ca_dyn_array_init(&menu->item_storage, sizeof(Ca_MenuBarItem)))
        return false;
    for (size_t i = count; i < menu->item_storage.count; ++i)
        menu_item_destroy(ca_dyn_array_at(&menu->item_storage, i));
    if (!ca_dyn_array_resize(&menu->item_storage, count)) return false;
    menu->items = menu->item_storage.data;
    menu->item_count = (int)count;
    return true;
}

bool ca_menu_storage_resize(Ca_DynArray *storage, Ca_MenuBarMenu **menus,
                            size_t count)
{
    if (!storage || !menus || count > INT_MAX) return false;
    if (storage->element_size == 0 &&
        !ca_dyn_array_init(storage, sizeof(Ca_MenuBarMenu)))
        return false;
    for (size_t i = count; i < storage->count; ++i)
        menu_destroy(ca_dyn_array_at(storage, i));
    if (!ca_dyn_array_resize(storage, count)) return false;
    *menus = storage->data;
    return true;
}

void ca_menu_storage_destroy(Ca_DynArray *storage, Ca_MenuBarMenu **menus)
{
    if (!storage) return;
    if (ca_dyn_array_valid(storage)) {
        for (size_t i = 0; i < storage->count; ++i)
            menu_destroy(ca_dyn_array_at(storage, i));
    }
    ca_dyn_array_destroy(storage);
    if (menus) *menus = NULL;
}
