#!/usr/bin/env python3
"""
Turn a character generator font into the lookup table screentext.c searches.

A text-mode screen is drawn by a character generator: every glyph is the same
bitmap every time it appears, so recognising one is a table lookup, not image
recognition. This script hashes each glyph and pairs it with the character it
stands for, which is all the scanner needs. The bitmaps themselves are never
compiled in - only the hashes - so a table costs about 2 KB whatever the font.

One table per cell height, and a table may merge several fonts: the firmwares
that draw 16-tall text do not all draw it the same way, and a bitmap is looked
up by its shape, not by which font it came from. A bitmap that means different
characters in different fonts is dropped rather than guessed at.

Two input formats, picked by extension:

  .bin  a raw ROM dump: 256 glyphs of `--height` rows, one byte per row, most
        significant bit leftmost, indexed by code page 437. What every VGA BIOS
        keeps in ROM.
  .txt  one glyph per line, `XXXX: hh hh ...`, the code point in hex followed by
        `--height` bytes. For fonts that are a list of Unicode glyphs rather
        than a code page - a UEFI firmware's font is one.

    python3 tools/mkfont.py fonts/ibm_vga_8x16.bin fonts/pcdos_cp437_8x16.bin \\
        --height 16 > screentext_font_h16.h
    python3 tools/mkfont.py fonts/uefi_hii_8x19.txt --height 19 \\
        > screentext_font_h19.h

The .txt for the UEFI font is itself generated, so the chain stays reproducible:

    python3 tools/mkfont.py --from-edk2 LaffStd.c > fonts/uefi_hii_8x19.txt
"""
import argparse
import re
import sys

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193

# What the generated header says about where its bitmaps came from.
PROVENANCE = {
    16: (
        "The 16-tall fonts, which is what a legacy BIOS and a Linux console draw\n"
        "with. Two of them, because they are not the same font: the IBM VGA ROM\n"
        "font (a dump of the ROM, no copyright of its own) and the PC-DOS code page\n"
        "437 font, which is the one the Linux console carries and which differs in\n"
        "28 glyphs - five of them printable ASCII, including f and v. Reading a\n"
        "Linux console with the IBM table alone loses those letters."
    ),
    19: (
        "The 19-tall font: the UEFI narrow font, what a firmware's own console\n"
        "draws with, and so what a UEFI boot menu or setup screen is written in.\n"
        "The bitmaps come from EDK2's standard narrow glyph table\n"
        "(gUsStdNarrowGlyphData, Intel, BSD-2-Clause-Patent)."
    ),
}


def fnv1a(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def read_rom(path, height):
    """A flat code page 437 ROM dump: 256 glyphs, `height` rows each."""
    blob = open(path, "rb").read()
    want = 256 * height
    if len(blob) != want:
        sys.exit("%s: expected %d bytes (256 glyphs x %d rows), got %d"
                 % (path, want, height, len(blob)))
    glyphs = {}
    for idx in range(256):
        cp = ord(bytes([idx]).decode("cp437"))
        glyphs[cp] = blob[idx * height:(idx + 1) * height]
    return glyphs


def read_list(path, height):
    """`XXXX: hh hh ...` per line, the code point followed by `height` bytes."""
    glyphs = {}
    for lineno, line in enumerate(open(path), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^([0-9a-fA-F]{4})\s*:\s*(.*)$", line)
        if not m:
            sys.exit("%s:%d: expected 'XXXX: hh hh ...'" % (path, lineno))
        rows = [int(b, 16) for b in m.group(2).split()]
        if len(rows) != height:
            sys.exit("%s:%d: expected %d bytes, got %d" % (path, lineno, height, len(rows)))
        glyphs[int(m.group(1), 16)] = bytes(rows)
    if not glyphs:
        sys.exit("%s: no glyphs" % path)
    return glyphs


def from_edk2(path):
    """
    Extract the narrow glyphs from EDK2's LaffStd.c.

    Each entry is `{ 0xNNNN, 0xNN, { 19 bytes } }`: the Unicode code point, the
    width attribute, and the bitmap. Wide glyphs carry a second bitmap and are
    not a fixed 19 bytes, so they are skipped - the console draws text with the
    narrow set.
    """
    src = open(path).read()
    entry = re.compile(r"\{\s*(0x[0-9a-fA-F]{4})\s*,\s*0x[0-9a-fA-F]{2}\s*,\s*\{([^}]*)\}", re.S)
    out = []
    for m in entry.finditer(src):
        rows = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2))]
        if len(rows) != 19:
            continue
        out.append((int(m.group(1), 16), rows))
    if not out:
        sys.exit("%s: found no 19-row narrow glyphs - is this LaffStd.c?" % path)
    w = sys.stdout.write
    w("# The UEFI narrow font, 8 wide by 19 tall: one glyph per line, the Unicode\n")
    w("# code point in hex followed by 19 rows, one byte each, MSB leftmost.\n")
    w("#\n")
    w("# Extracted from EDK2's gUsStdNarrowGlyphData\n")
    w("# (MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c,\n")
    w("# Copyright (c) 2006-2008 Intel Corporation, SPDX-License-Identifier:\n")
    w("# BSD-2-Clause-Patent) with:\n")
    w("#\n")
    w("#     python3 tools/mkfont.py --from-edk2 LaffStd.c > fonts/uefi_hii_8x19.txt\n")
    w("#\n")
    w("# %d glyphs.\n" % len(out))
    for cp, rows in sorted(out):
        w("%04X: %s\n" % (cp, " ".join("%02X" % b for b in rows)))


def font_table(glyphs):
    """
    One font as hash -> code point.

    A bitmap can stand for more than one character, so keep the lowest code
    point - the one a reader means. An empty bitmap is the exception: a font
    draws nothing for the blank at 0x00, the space, and the non-breaking space
    alike, and "space" is the only one of those worth putting on a screen.
    """
    out = {}
    for cp, glyph in glyphs.items():
        if not any(glyph):
            cp = 0x20
        h = fnv1a(glyph)
        out[h] = min(out[h], cp) if h in out else cp
    return out


def emit(tables, height, sources):
    """
    Merge the fonts of one cell height into a table and print it.

    Where two fonts draw the same bitmap for different characters there is no
    honest answer, so the bitmap is dropped: a cell that hits it reads as
    unreadable, which is the whole promise of doing it this way.
    """
    merged = {}
    dropped = set()
    for t in tables:
        for h, cp in t.items():
            if h in merged and merged[h] != cp:
                dropped.add(h)
            merged.setdefault(h, cp)
    for h in dropped:
        del merged[h]
    if dropped:
        sys.stderr.write("%d bitmaps mean different characters in different fonts; dropped\n"
                         % len(dropped))

    entries = sorted(merged.items())
    w = sys.stdout.write
    w("/*\n")
    w(" * SPDX-FileCopyrightText: 2026 ESP-KVM contributors\n")
    w(" * SPDX-License-Identifier: Apache-2.0\n")
    w(" *\n")
    w(" * GENERATED by tools/mkfont.py - do not edit.\n")
    w(" *\n")
    for line in PROVENANCE[height].split("\n"):
        w((" * %s\n" % line) if line else " *\n")
    w(" *\n")
    w(" * Built from: %s\n" % ", ".join(sources))
    w(" * %d distinct bitmaps%s, sorted by hash for a binary search.\n"
      % (len(entries), ", %d dropped as ambiguous" % len(dropped) if dropped else ""))
    w(" */\n")
    w("#pragma once\n\n")
    w('#include "screentext_glyph.h"\n\n')
    w("#define SCREENTEXT_FONT_H%d_HEIGHT %d\n" % (height, height))
    w("#define SCREENTEXT_FONT_H%d_GLYPHS %d\n\n" % (height, len(entries)))
    w("static const screentext_glyph_t screentext_font_h%d[SCREENTEXT_FONT_H%d_GLYPHS] = {\n"
      % (height, height))
    for h, cp in entries:
        note = chr(cp) if 0x20 < cp < 0x7F else "U+%04X" % cp
        w("    {0x%08x, 0x%04x}, /* %s */\n" % (h, cp, note))
    w("};\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("font", nargs="*", help="the .bin or .txt fonts to merge into one table")
    ap.add_argument("--height", type=int, default=16, help="rows per glyph (default 16)")
    ap.add_argument("--from-edk2", metavar="LaffStd.c",
                    help="convert EDK2's narrow glyph table to the .txt format instead")
    args = ap.parse_args()

    if args.from_edk2:
        from_edk2(args.from_edk2)
        return
    if not args.font:
        ap.error("at least one font file is required")
    if args.height not in PROVENANCE:
        ap.error("no provenance text for a %d-tall table; add one to PROVENANCE" % args.height)

    tables = []
    for path in args.font:
        reader = read_rom if path.endswith(".bin") else read_list
        tables.append(font_table(reader(path, args.height)))
    emit(tables, args.height, [p.split("/")[-1] for p in args.font])


if __name__ == "__main__":
    main()
