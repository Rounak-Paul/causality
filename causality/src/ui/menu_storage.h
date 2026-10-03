// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Causality contributors.

#pragma once

#include "ca_internal.h"

/** Resizes menu storage while preserving nested item allocations. */
bool ca_menu_storage_resize(Ca_DynArray *storage, Ca_MenuBarMenu **menus,
                            size_t count);

/** Resizes one menu's item storage while preserving nested sub-items. */
bool ca_menu_item_storage_resize(Ca_MenuBarMenu *menu, size_t count);

/** Resizes one item's sub-item storage. */
bool ca_menu_sub_item_storage_resize(Ca_MenuBarItem *item, size_t count);

/** Releases a complete menu tree and clears its direct-access alias. */
void ca_menu_storage_destroy(Ca_DynArray *storage,
                             Ca_MenuBarMenu **menus);

