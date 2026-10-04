// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* palette.c — resolves the `--ca-*` CSS variables Causality paints with */
#include "style.h"

#include <stddef.h>

static const struct { const char *name; size_t offset; } k_palette_vars[] = {
    { "--ca-bg-void",     offsetof(Ca_Palette, bg_void) },
    { "--ca-bg-base",     offsetof(Ca_Palette, bg_base) },
    { "--ca-bg-elevated", offsetof(Ca_Palette, bg_elevated) },
    { "--ca-bg-surface",  offsetof(Ca_Palette, bg_surface) },
    { "--ca-bg-overlay",  offsetof(Ca_Palette, bg_overlay) },
    { "--ca-separator",   offsetof(Ca_Palette, separator) },
    { "--ca-text-bright", offsetof(Ca_Palette, text_bright) },
    { "--ca-text-medium", offsetof(Ca_Palette, text_medium) },
    { "--ca-text-muted",  offsetof(Ca_Palette, text_muted) },
    { "--ca-text-dim",    offsetof(Ca_Palette, text_dim) },
    { "--ca-accent",      offsetof(Ca_Palette, accent) },
    { "--ca-on-accent",   offsetof(Ca_Palette, on_accent) },
    { "--ca-success",     offsetof(Ca_Palette, success) },
    { "--ca-warning",     offsetof(Ca_Palette, warning) },
    { "--ca-danger",      offsetof(Ca_Palette, danger) },
    { "--ca-on-danger",   offsetof(Ca_Palette, on_danger) },
};

void ca_instance_resolve_palette(Ca_Instance *instance)
{
    if (!instance) return;
    const Ca_VarScope scope = { { instance->stylesheet, instance->system_stylesheet }, 2 };
    const Ca_VarScope defaults = { { instance->system_stylesheet }, 1 };
    for (size_t i = 0; i < sizeof(k_palette_vars) / sizeof(k_palette_vars[0]); ++i) {
        Ca_CssValue value = ca_style_lookup_var(&scope, k_palette_vars[i].name);
        if (value.type != CA_CSS_VAL_COLOR)
            value = ca_style_lookup_var(&defaults, k_palette_vars[i].name);
        uint32_t *slot = (uint32_t *)((char *)&instance->palette + k_palette_vars[i].offset);
        *slot = value.type == CA_CSS_VAL_COLOR ? value.color : 0u;
    }
}
