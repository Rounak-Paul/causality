// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

#pragma once

/* ================================================================
   ca_theme.h — Default widget-chrome color constants for Causality

   These are the fallback colors used by widget.c and paint.c when
   a stylesheet property is not (yet) CSS-driven.  All values are
   expressed as ca_color(r, g, b, a) macro calls so they pack into
   a uint32_t RGBA token and can be used wherever a color field is
   expected.

   Background depth hierarchy (darkest → lightest):
     CA_THEME_BG_VOID      — deepest recess (inputs, viewport fill)
     CA_THEME_BG_BASE      — primary panel surface
     CA_THEME_BG_ELEVATED  — raised chrome (toolbar, sidebar)
     CA_THEME_BG_SURFACE   — section headers, tab bars
     CA_THEME_BG_OVERLAY   — hover / selected state

   Text hierarchy:
     CA_THEME_TEXT_BRIGHT  — interactive / primary labels
     CA_THEME_TEXT_MUTED   — secondary labels, hints
     CA_THEME_TEXT_DIM     — disabled, placeholders

   Accent / semantic:
     CA_THEME_ACCENT       — primary interactive accent
     CA_THEME_SUCCESS      — confirm, on-state (toggle)
     CA_THEME_WARNING      — warnings
     CA_THEME_DANGER       — errors, destructive
   ================================================================ */

/* ---- Backgrounds (minimal cool glass) ---- */
#define CA_THEME_BG_VOID     ca_color(0x0b/255.f, 0x0e/255.f, 0x14/255.f, 1.0f)
#define CA_THEME_BG_BASE     ca_color(0x12/255.f, 0x16/255.f, 0x20/255.f, 0.94f)
#define CA_THEME_BG_ELEVATED ca_color(0x18/255.f, 0x1d/255.f, 0x29/255.f, 0.94f)
#define CA_THEME_BG_SURFACE  ca_color(0x20/255.f, 0x26/255.f, 0x34/255.f, 0.92f)
#define CA_THEME_BG_OVERLAY  ca_color(0x58/255.f, 0x8d/255.f, 0xc8/255.f, 0.22f)

/* ---- Text ---- */
#define CA_THEME_TEXT_BRIGHT ca_color(0xd8/255.f, 0xdd/255.f, 0xe6/255.f, 1.0f)
#define CA_THEME_TEXT_MUTED  ca_color(0x8c/255.f, 0x98/255.f, 0xaa/255.f, 1.0f)
#define CA_THEME_TEXT_DIM    ca_color(0x58/255.f, 0x63/255.f, 0x74/255.f, 1.0f)

/* ---- Accent / semantic ---- */
#define CA_THEME_ACCENT   ca_color(0x63/255.f, 0x9d/255.f, 0xd7/255.f, 1.0f)
#define CA_THEME_SUCCESS  ca_color(0x68/255.f, 0x8a/255.f, 0x70/255.f, 1.0f)
#define CA_THEME_WARNING  ca_color(0xb0/255.f, 0x90/255.f, 0x50/255.f, 1.0f)
#define CA_THEME_DANGER   ca_color(0xa8/255.f, 0x5c/255.f, 0x64/255.f, 1.0f)

/* ---- Popover / overlay surfaces (slightly transparent) ---- */
#define CA_THEME_POPUP_BG     ca_color(0x16/255.f, 0x1a/255.f, 0x24/255.f, 0.97f)
#define CA_THEME_POPUP_TEXT   CA_THEME_TEXT_BRIGHT
#define CA_THEME_POPUP_BORDER ca_color(0x78/255.f, 0x90/255.f, 0xac/255.f, 0.30f)

/* ---- Transparency / overlay ---- */
#define CA_THEME_TRANSPARENT   0u
#define CA_THEME_MODAL_OVERLAY ca_color(0.0f, 0.0f, 0.0f, 0.50f)

/* ---- Scrollbar ---- */
#define CA_THEME_SCROLLBAR_TRACK        ca_color(0x0d/255.f, 0x11/255.f, 0x18/255.f, 0.36f)
#define CA_THEME_SCROLLBAR_THUMB        ca_color(0x75/255.f, 0x86/255.f, 0x9c/255.f, 0.42f)
#define CA_THEME_SCROLLBAR_THUMB_ACTIVE ca_color(0x8e/255.f, 0xa5/255.f, 0xc0/255.f, 0.62f)

/* ---- Fatal (slightly warmer red than danger) ---- */
#define CA_THEME_FATAL ca_color(0xb8/255.f, 0x58/255.f, 0x64/255.f, 1.0f)
