"""Bake a small-caps font's default substitutions into its character map.

The Spectral SC files map lowercase letters to plain lowercase glyphs and switch them to small caps through the
always-on 'ccmp' feature. Slate shapes Latin text kerning-only, so 'ccmp' never runs and the HUD would draw plain
lowercase. This script points the cmap straight at the substituted glyphs (e -> e.sc and so on), so the file draws
real small caps with no shaping. Figures keep their lining forms. Nothing else in the font changes. Spectral is OFL 1.1 with no Reserved Font Name;
the derived file ships beside the unchanged licence.

Usage: python -I tools/bake_small_caps.py <official SC .ttf> <output .ttf>
Needs fontTools. Deterministic: the same input gives the same output bytes.
"""

import hashlib
import sys
import unicodedata

from fontTools.ttLib import TTFont


def single_subs(gsub, tag):
    """All single substitutions reachable from the given feature tag."""
    mapping = {}
    table = gsub.table
    for record in table.FeatureList.FeatureRecord:
        if record.FeatureTag != tag:
            continue
        for index in record.Feature.LookupListIndex:
            lookup = table.LookupList.Lookup[index]
            for sub in lookup.SubTable:
                inner = getattr(sub, "ExtSubTable", sub)
                found = getattr(inner, "mapping", None)
                if found:
                    mapping.update(found)
    return mapping


def main(src, dst):
    font = TTFont(src, recalcTimestamp=False)  # keep the head date: same input, same bytes
    subs = single_subs(font["GSUB"], "ccmp")
    if not subs:
        sys.exit("no ccmp single substitutions in " + src)
    changed = 0
    for cmap in font["cmap"].tables:
        if not cmap.isUnicode():
            continue
        for code, glyph in list(cmap.cmap.items()):
            # Figures stay lining: the small-cap figures read as letters at HUD sizes (108 looks like 1o8).
            if glyph in subs and not unicodedata.category(chr(code)).startswith("N"):
                cmap.cmap[code] = subs[glyph]
                changed += 1
    font.save(dst, reorderTables=True)
    with open(src, "rb") as f:
        src_hash = hashlib.sha256(f.read()).hexdigest()
    with open(dst, "rb") as f:
        dst_hash = hashlib.sha256(f.read()).hexdigest()
    print(f"{src} ({src_hash}) -> {dst} ({dst_hash}): {changed} cmap entries")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
