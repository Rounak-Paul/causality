// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* theme.c — instance palette ownership */
#include "style.h"
#include "widget.h"

Ca_Theme ca_theme_default(void)
{
    return (Ca_Theme){
        .bg_void     = 0x0d0d0dffu,
        .bg_base     = 0x0d0d0dffu,
        .bg_elevated = 0x121212ffu,
        .bg_surface  = 0x1a1a1affu,
        .bg_overlay  = 0x333333ffu,
        .separator   = 0x262626ffu,
        .text_bright = 0xd9d9d9ffu,
        .text_muted  = 0x737373ffu,
        .text_dim    = 0x404040ffu,
        .accent      = 0x999999ffu,
        .on_accent   = 0x0d0d0dffu,
        .success     = 0x80b380ffu,
        .warning     = 0xccb366ffu,
        .danger      = 0xcc6666ffu,
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
