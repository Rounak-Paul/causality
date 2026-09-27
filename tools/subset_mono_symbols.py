#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Causality contributors.
#
# subset_mono_symbols.py - build the embedded monospace symbol face
# ("Causality Mono Symbols") from upstream JuliaMono.
#
# Usage (requires fontTools):
#   python3 tools/subset_mono_symbols.py <JuliaMono-Regular.ttf> \
#       <emoji-data.txt> <CausalityMonoSymbols-Regular.ttf>
#   python3 tools/embed_font.py <CausalityMonoSymbols-Regular.ttf> \
#       ca_embedded_mono_symbols_font \
#       causality/src/renderer/embedded_mono_symbols_font.c
#
# Keeps the Unicode symbol blocks terminal/TUI output relies on (arrows,
# math, technical, box/block, geometric, dingbats, Braille, legacy
# computing) and drops:
#   - codepoints the primary Roboto Mono face already maps, so that face
#     keeps rendering them;
#   - Emoji_Presentation=Yes codepoints (emoji-data.txt), which must resolve
#     to the double-width emoji face instead of a single-width text glyph.
# JuliaMono's Reserved Font Name forbids reuse by modified versions (SIL OFL
# 1.1 section 3), so the subset is renamed.

import re
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

PRIMARY_FONT_SOURCE = (Path(__file__).resolve().parent.parent
                       / "causality/src/renderer/embedded_font.c")

BLOCKS = [
    (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2100, 0x214F), (0x2150, 0x218F),
    (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x2400, 0x243F),
    (0x2440, 0x245F), (0x2460, 0x24FF), (0x2500, 0x257F), (0x2580, 0x259F),
    (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF), (0x27C0, 0x27EF),
    (0x27F0, 0x27FF), (0x2800, 0x28FF), (0x2900, 0x297F), (0x2980, 0x29FF),
    (0x2A00, 0x2AFF), (0x2B00, 0x2BFF), (0x1FB00, 0x1FBFF),
]

FAMILY = "Causality Mono Symbols"
POSTSCRIPT = "CausalityMonoSymbols-Regular"


def emoji_presentation(path):
    """Return the Emoji_Presentation=Yes codepoint set from emoji-data.txt."""
    result = set()
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        span, prop = (part.strip() for part in line.split(";"))
        if prop != "Emoji_Presentation":
            continue
        first, _, last = span.partition("..")
        result.update(range(int(first, 16), int(last or first, 16) + 1))
    return result


def primary_cmap():
    """Return the codepoints mapped by the embedded primary text face."""
    source = PRIMARY_FONT_SOURCE.read_text(encoding="utf-8")
    body = source[source.index("_data[] = {"):]
    data = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body))
    tmp = Path(sys.argv[3]).with_suffix(".primary.ttf")
    tmp.write_bytes(data)
    try:
        return set(TTFont(str(tmp)).getBestCmap())
    finally:
        tmp.unlink()


def rename(font):
    """Replace every family/full/unique/PostScript name with the new name."""
    names = {1: FAMILY, 3: f"{FAMILY} Regular; derived from JuliaMono",
             4: f"{FAMILY} Regular", 6: POSTSCRIPT, 16: FAMILY, 21: FAMILY}
    for record in font["name"].names:
        if record.nameID in names:
            record.string = names[record.nameID]


def main():
    """Subset and rename JuliaMono into the embedded symbol face."""
    if len(sys.argv) != 4:
        sys.exit("usage: subset_mono_symbols.py <JuliaMono-Regular.ttf> "
                 "<emoji-data.txt> <out.ttf>")
    font = TTFont(sys.argv[1])
    wanted = {cp for first, last in BLOCKS for cp in range(first, last + 1)}
    wanted &= set(font.getBestCmap())
    wanted -= emoji_presentation(sys.argv[2])
    wanted -= primary_cmap()

    options = subset.Options()
    options.layout_features = []
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.glyph_names = False
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=sorted(wanted))
    subsetter.subset(font)
    rename(font)
    font.save(sys.argv[3])
    print(f"wrote {sys.argv[3]} ({len(wanted)} codepoints)")


if __name__ == "__main__":
    main()
