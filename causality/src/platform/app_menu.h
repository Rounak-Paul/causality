#pragma once

/*
 * Internal platform interface for the application-level menu bar.
 *
 * ca_app_menu_set   — Called when ca_instance_set_app_menus() is invoked.
 *                     The instance already holds the deep-copied menu data.
 *                     On macOS this rebuilds [NSApp mainMenu].
 *                     On other platforms this is a no-op (ui.c emits the
 *                     ca_menu_bar widget from the stored instance data).
 *
 * instance          The owning Ca_Instance (menu data already updated).
 */

typedef struct Ca_Instance Ca_Instance;

void ca_app_menu_set(Ca_Instance *instance);
