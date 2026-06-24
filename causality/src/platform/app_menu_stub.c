/*
 * Non-Apple stub for the platform app-menu interface.
 *
 * On non-Apple platforms the menu bar is rendered as a ca_menu_bar widget
 * emitted automatically by ca_ui_begin; no native OS action is required here.
 */
#ifndef __APPLE__

#include "app_menu.h"

void ca_app_menu_set(Ca_Instance *instance)
{
    (void)instance;
}

#endif /* !__APPLE__ */
