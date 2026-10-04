// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

#include <stdint.h>

#include "ca_api.h"

/* ================================================================
   ca_theme.h — Instance-wide semantic palette

   Every color Causality draws without an explicit stylesheet or
   descriptor value comes from the instance theme. The theme also feeds
   the system stylesheet through its `--ca-*` custom properties, so
   changing it re-skins Causality-owned chrome (title bar, popups,
   hover overlays) and widget defaults in one call.

   All colors are packed RRGGBBAA (see ca_color()).

   Background depth (recessed -> raised):
     bg_void      deepest recess (inputs, scrollbar tracks)
     bg_base      primary panel surface
     bg_elevated  raised chrome (title bar, popups)
     bg_surface   section headers, hover fill
     bg_overlay   selected / pressed fill, scrollbar thumb
   ================================================================ */

typedef struct Ca_Theme {
    uint32_t bg_void;
    uint32_t bg_base;
    uint32_t bg_elevated;
    uint32_t bg_surface;
    uint32_t bg_overlay;
    uint32_t separator;
    uint32_t text_bright;
    uint32_t text_medium;
    uint32_t text_muted;
    uint32_t text_dim;
    uint32_t accent;
    uint32_t on_accent;   /* marks drawn on an accent fill (checkmarks) */
    uint32_t success;
    uint32_t warning;
    uint32_t danger;
    uint32_t on_danger;   /* text drawn on a danger fill */
} Ca_Theme;
