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

/* ---- Backgrounds (dark minimalist) ---- */
#define CA_THEME_BG_VOID     ca_color(0x0d/255.f, 0x0d/255.f, 0x0d/255.f, 1.0f)
#define CA_THEME_BG_BASE     ca_color(0x0f/255.f, 0x0f/255.f, 0x0f/255.f, 1.0f)
#define CA_THEME_BG_ELEVATED ca_color(0x12/255.f, 0x12/255.f, 0x12/255.f, 1.0f)
#define CA_THEME_BG_SURFACE  ca_color(0x1a/255.f, 0x1a/255.f, 0x1a/255.f, 1.0f)
#define CA_THEME_BG_OVERLAY  ca_color(0x33/255.f, 0x33/255.f, 0x33/255.f, 1.0f)

/* ---- Text ---- */
#define CA_THEME_TEXT_BRIGHT ca_color(0xd9/255.f, 0xd9/255.f, 0xd9/255.f, 1.0f)
#define CA_THEME_TEXT_MUTED  ca_color(0x73/255.f, 0x73/255.f, 0x73/255.f, 1.0f)
#define CA_THEME_TEXT_DIM    ca_color(0x40/255.f, 0x40/255.f, 0x40/255.f, 1.0f)

/* ---- Accent / semantic ---- */
#define CA_THEME_ACCENT   ca_color(0x99/255.f, 0x99/255.f, 0x99/255.f, 1.0f)
#define CA_THEME_SUCCESS  ca_color(0x80/255.f, 0xb3/255.f, 0x80/255.f, 1.0f)
#define CA_THEME_WARNING  ca_color(0xcc/255.f, 0xb3/255.f, 0x66/255.f, 1.0f)
#define CA_THEME_DANGER   ca_color(0xcc/255.f, 0x66/255.f, 0x66/255.f, 1.0f)

/* ---- Popover / overlay surfaces (slightly transparent) ---- */
#define CA_THEME_POPUP_BG     ca_color(0x12/255.f, 0x12/255.f, 0x12/255.f, 0.98f)
#define CA_THEME_POPUP_TEXT   CA_THEME_TEXT_BRIGHT
#define CA_THEME_POPUP_BORDER ca_color(0x26/255.f, 0x26/255.f, 0x26/255.f, 1.0f)

/* ---- Transparency / overlay ---- */
#define CA_THEME_TRANSPARENT   0u
#define CA_THEME_MODAL_OVERLAY ca_color(0.0f, 0.0f, 0.0f, 0.50f)

/* ---- Scrollbar ---- */
#define CA_THEME_SCROLLBAR_TRACK        ca_color(0x0d/255.f, 0x0d/255.f, 0x0d/255.f, 1.0f)
#define CA_THEME_SCROLLBAR_THUMB        ca_color(0x33/255.f, 0x33/255.f, 0x33/255.f, 1.0f)
#define CA_THEME_SCROLLBAR_THUMB_ACTIVE ca_color(0x73/255.f, 0x73/255.f, 0x73/255.f, 1.0f)

/* ---- Fatal (slightly warmer red than danger) ---- */
#define CA_THEME_FATAL ca_color(0xd8/255.f, 0x58/255.f, 0x64/255.f, 1.0f)
