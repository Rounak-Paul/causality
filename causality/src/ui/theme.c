// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* theme.c — instance palette ownership */
#include "style.h"
#include "widget.h"

Ca_Theme ca_theme_default(void)
{
    return (Ca_Theme){
        .bg_void     = 0x242424ffu,
        .bg_base     = 0x0d0d0dffu,
        .bg_elevated = 0x191919ffu,
        .bg_surface  = 0x272727ffu,
        .bg_overlay  = 0x444444ffu,
        .separator   = 0x363636ffu,
        .text_bright = 0xd9d9d9ffu,
        .text_medium = 0xbebebeffu,
        .text_muted  = 0x989898ffu,
        .text_dim    = 0x7c7c7cffu,
        .accent      = 0x999999ffu,
        .on_accent   = 0x0d0d0dffu,
        .success     = 0x80b380ffu,
        .warning     = 0xccb366ffu,
        .danger      = 0xcc6666ffu,
        .on_danger   = 0x0d0d0dffu,
    };
}

const Ca_Theme *ca_instance_theme(const Ca_Instance *instance)
{
    return &instance->theme;
}

void ca_instance_set_theme(Ca_Instance *instance, const Ca_Theme *theme)
{
    if (!instance || !theme) return;
    instance->theme = *theme;
    if (instance->system_stylesheet)
        ca_style_apply_theme(instance->system_stylesheet, theme);
    ca_instance_refresh_tab_bars(instance);
    ca_instance_refresh_styles(instance);
}
