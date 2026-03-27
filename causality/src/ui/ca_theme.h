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

/* ---- Backgrounds ---- */
#define CA_THEME_BG_VOID     ca_color(0.051f, 0.051f, 0.059f, 1.0f)
#define CA_THEME_BG_BASE     ca_color(0.067f, 0.067f, 0.078f, 1.0f)
#define CA_THEME_BG_ELEVATED ca_color(0.086f, 0.086f, 0.102f, 1.0f)
#define CA_THEME_BG_SURFACE  ca_color(0.110f, 0.110f, 0.133f, 1.0f)
#define CA_THEME_BG_OVERLAY  ca_color(0.141f, 0.141f, 0.188f, 1.0f)

/* ---- Text ---- */
#define CA_THEME_TEXT_BRIGHT ca_color(0.784f, 0.816f, 1.0f,   1.0f)
#define CA_THEME_TEXT_MUTED  ca_color(0.533f, 0.565f, 0.690f, 1.0f)
#define CA_THEME_TEXT_DIM    ca_color(0.290f, 0.306f, 0.416f, 1.0f)

/* ---- Accent / semantic ---- */
#define CA_THEME_ACCENT   ca_color(0.431f, 0.541f, 1.0f,   1.0f)
#define CA_THEME_SUCCESS  ca_color(0.420f, 1.0f,   0.722f, 1.0f)
#define CA_THEME_WARNING  ca_color(1.0f,   0.820f, 0.400f, 1.0f)
#define CA_THEME_DANGER   ca_color(1.0f,   0.420f, 0.420f, 1.0f)

/* ---- Popover / overlay surfaces (slightly transparent) ---- */
#define CA_THEME_POPUP_BG   ca_color(0.086f, 0.086f, 0.102f, 0.98f)
#define CA_THEME_POPUP_TEXT CA_THEME_TEXT_BRIGHT

/* ---- Transparency / overlay ---- */
#define CA_THEME_TRANSPARENT   0u
#define CA_THEME_MODAL_OVERLAY ca_color(0.0f, 0.0f, 0.0f, 0.50f)

/* ---- Scrollbar ---- */
#define CA_THEME_SCROLLBAR_TRACK ca_color(1.0f, 1.0f, 1.0f, 0.05f)
#define CA_THEME_SCROLLBAR_THUMB ca_color(1.0f, 1.0f, 1.0f, 0.35f)

/* ---- Fatal (slightly warmer red than danger) ---- */
#define CA_THEME_FATAL ca_color(1.0f, 0.325f, 0.439f, 1.0f)
