# Character generator fonts

One table per cell height, and a table may hold several fonts - what is looked
up is a bitmap, not "which font is this".

`ibm_vga_8x16.bin` is the IBM VGA text-mode font: 256 glyphs of 16 rows, one
byte per row, most significant bit leftmost - the layout a VGA BIOS keeps in
ROM and the one nearly every legacy PC firmware draws its setup screens with.
It is a dump of that ROM; the bitmaps carry no copyright of their own. FreeBSD's
console font is byte-identical to it.

`pcdos_cp437_8x16.bin` is the PC-DOS code page 437 font, and it is **not** the
same font: 28 glyphs differ, five of them printable ASCII - `` ` `` f v | ~.
This is the font the Linux console carries (`lib/fonts/font_8x16.c`, "generated
by cpi2fnt"), so without it a Linux console reads at about 94% with every f and
v turned into a hole. Taken from viler-int10h's collection of raw VGA text-mode
fonts (`FONTS/SYSTEM/PCDOS2K/CP437.F16`), which is where to look first if some
machine's firmware turns out to draw with a font we do not have:
<https://github.com/viler-int10h/vga-text-mode-fonts>. Not from the kernel
itself, whose copy of it is GPL-2.0.

`uefi_hii_8x19.txt` is the UEFI narrow font: one glyph per line, the Unicode
code point in hex followed by 19 rows. This is what a firmware's own console
draws with, so it is what a UEFI boot menu or setup screen is written in - and
it is a different font, not a taller VGA. It was extracted from EDK2's standard
narrow glyph table (`gUsStdNarrowGlyphData`, Intel, BSD-2-Clause-Patent):

```sh
python3 tools/mkfont.py --from-edk2 LaffStd.c > fonts/uefi_hii_8x19.txt
```

The tables the scanner searches are generated from these:

```sh
python3 tools/mkfont.py fonts/ibm_vga_8x16.bin fonts/pcdos_cp437_8x16.bin \
    --height 16 > screentext_font_h16.h
python3 tools/mkfont.py fonts/uefi_hii_8x19.txt --height 19 > screentext_font_h19.h
```

Only hashes are compiled in, never the bitmaps, so a table costs about 2 KB
whatever the font, and a second font of the same height costs only the glyphs it
draws differently - the two 16-tall fonts together come to 282 entries against
254 for one, or 224 bytes for the pair of them.

Merging has one rule: where two fonts draw the same bitmap for different
characters there is no honest answer, so the bitmap is dropped and a cell that
hits it reads as unreadable. The two here disagree about nothing. Do not merge
indiscriminately for the same reason - all 401 hardware fonts in that collection
come to 16112 entries with 2069 of them ambiguous, which would trade the whole
guarantee for coverage nobody asked for.

A machine whose firmware draws with a font that is in neither table will not be
read - the scanner reports too few matches rather than guessing - so the way to
support one is to dump its font, drop it in here and regenerate the table.
