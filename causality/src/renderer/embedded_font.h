// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol/Causality contributors.

/* embedded_font.h - bundled default font data.
   Roboto Mono Nerd Font Mono provides regular/bold text faces.
   Symbols Nerd Font Mono provides the icon/symbol layer.
   Causality Mono Symbols (a renamed JuliaMono subset) provides monospace
   glyphs for the Unicode symbol blocks the text faces do not cover.
   DejaVu Sans provides the Unicode fallback layer for text blocks the
   Nerd Font faces do not cover (arrows, Braille, geometric shapes,
   dingbats, math operators).
   Noto Emoji (monochrome) provides the final fallback layer for emoji
   pictograph blocks that DejaVu does not cover. */
#pragma once

extern const unsigned int  ca_embedded_font_size;
extern const unsigned char ca_embedded_font_data[];

extern const unsigned int  ca_embedded_font_bold_size;
extern const unsigned char ca_embedded_font_bold_data[];

extern const unsigned int  ca_embedded_symbols_font_size;
extern const unsigned char ca_embedded_symbols_font_data[];

extern const unsigned int  ca_embedded_mono_symbols_font_size;
extern const unsigned char ca_embedded_mono_symbols_font_data[];

extern const unsigned int  ca_embedded_fallback_font_size;
extern const unsigned char ca_embedded_fallback_font_data[];

extern const unsigned int  ca_embedded_emoji_font_size;
extern const unsigned char ca_embedded_emoji_font_data[];
