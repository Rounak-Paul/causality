// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* embedded_font.h - bundled default font data.
   Roboto Mono Nerd Font Mono provides regular/bold text faces.
   Symbols Nerd Font Mono provides the icon/symbol layer.
   DejaVu Sans provides the Unicode fallback layer for text blocks the
   Nerd Font faces do not cover (arrows, Braille, geometric shapes,
   dingbats, math operators). */
#pragma once

extern const unsigned int  ca_embedded_font_size;
extern const unsigned char ca_embedded_font_data[];

extern const unsigned int  ca_embedded_font_bold_size;
extern const unsigned char ca_embedded_font_bold_data[];

extern const unsigned int  ca_embedded_symbols_font_size;
extern const unsigned char ca_embedded_symbols_font_data[];

extern const unsigned int  ca_embedded_fallback_font_size;
extern const unsigned char ca_embedded_fallback_font_data[];
