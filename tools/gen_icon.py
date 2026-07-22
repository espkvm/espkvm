#!/usr/bin/env python3
"""Generate the ESP-KVM icon set.

The mark is the project's name in two rows, set in a blocky monospaced grid and
drawn as cells rather than as type. A favicon is rasterised at sixteen pixels,
where the choice of font, its hinting and the renderer decide what the letters
look like; cells decide it here.

Two letterforms, for a reason that is about pixels rather than taste:

  5x5 glyphs  the mark proper: the scalable icon, and anything shown large.
              K, V and M have room to be themselves.
  3x5 glyphs  drawn straight onto the pixel grid for every raster size, whole
              cells on whole pixels, so nothing is antialiased into mush.
              Scaling the 5x5 mark down to sixteen pixels produces texture,
              not a word - which was the whole objection.

Requires inkscape (rendering) and ImageMagick (assembling the .ico).

    python3 tools/gen_icon.py
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUBLIC = ROOT / "web" / "public"

BG = "#0e1116"
FG = "#e6edf3"

WORDS = ("ESP", "KVM")

# One cell per character of the pattern; a "1" is a filled cell.
GLYPHS_3x5 = {
    "E": ["111", "100", "110", "100", "111"],
    "S": ["111", "100", "111", "001", "111"],
    "P": ["111", "101", "111", "100", "100"],
    "K": ["101", "110", "100", "110", "101"],
    "V": ["101", "101", "101", "101", "010"],
    "M": ["101", "111", "101", "101", "101"],
}

GLYPHS_5x5 = {
    "E": ["11111", "10000", "11110", "10000", "11111"],
    "S": ["11111", "10000", "11111", "00001", "11111"],
    "P": ["11111", "10001", "11111", "10000", "10000"],
    "K": ["10001", "10010", "11100", "10010", "10001"],
    "V": ["10001", "10001", "10001", "01010", "00100"],
    "M": ["10001", "11011", "10101", "10001", "10001"],
}


def cells(glyphs: dict[str, list[str]], word: str, cell: int, ox: int, oy: int) -> list[str]:
    """Rects for one row of text, one cell apart."""
    out: list[str] = []
    width = len(next(iter(glyphs.values()))[0])
    for i, ch in enumerate(word):
        gx = ox + i * (width + 1) * cell
        for r, line in enumerate(glyphs[ch]):
            for c, v in enumerate(line):
                if v == "1":
                    out.append(
                        f'<rect x="{gx + c * cell}" y="{oy + r * cell}" '
                        f'width="{cell}" height="{cell}"/>'
                    )
    return out


def build_svg(glyphs: dict[str, list[str]], size: int, cell: int, radius: int) -> str:
    gw = len(next(iter(glyphs.values()))[0])
    block_w = (len(WORDS[0]) * (gw + 1) - 1) * cell
    block_h = (5 * 2 + 1) * cell
    ox = (size - block_w) // 2
    oy = (size - block_h) // 2
    if ox < 0 or oy < 0:
        raise SystemExit(
            f"{len(WORDS[0])} glyphs of {gw}x5 at cell {cell} need "
            f"{block_w}x{block_h}, which does not fit a {size}px tile"
        )
    rects = cells(glyphs, WORDS[0], cell, ox, oy)
    rects += cells(glyphs, WORDS[1], cell, ox, oy + 6 * cell)
    body = "\n    ".join(rects)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" \
width="{size}" height="{size}" role="img" aria-label="ESP-KVM">
  <title>ESP-KVM</title>
  <rect width="{size}" height="{size}" rx="{radius}" fill="{BG}"/>
  <g fill="{FG}">
    {body}
  </g>
</svg>
"""


def render(svg: str, png: Path, size: int) -> None:
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg)
        tmp = Path(f.name)
    try:
        subprocess.run(
            ["inkscape", str(tmp), "-w", str(size), "-h", str(size), "-o", str(png)],
            check=True,
            capture_output=True,
        )
    finally:
        tmp.unlink(missing_ok=True)


def main() -> int:
    # The scalable mark, and the file a human should edit if the design changes
    # (then re-run this script).
    master = build_svg(GLYPHS_5x5, size=64, cell=3, radius=12)
    (PUBLIC / "icon.svg").write_text(master)

    with tempfile.TemporaryDirectory() as tmpdir:
        pngs = []
        # Every raster size gets the narrow glyphs on its own grid: one cell
        # per pixel, so the letters stay letters.
        for size, glyphs, cell, radius in (
            (16, GLYPHS_3x5, 1, 2),
            (32, GLYPHS_3x5, 2, 5),
            (48, GLYPHS_3x5, 3, 8),
        ):
            png = Path(tmpdir) / f"icon{size}.png"
            render(build_svg(glyphs, size, cell, radius), png, size)
            pngs.append(str(png))
        subprocess.run(
            ["magick", *pngs, str(PUBLIC / "favicon.ico")], check=True, capture_output=True
        )

    print(f"wrote {PUBLIC / 'icon.svg'} and {PUBLIC / 'favicon.ico'}")
    print("run `npm run build` in web/ to embed the new icon in the firmware")
    return 0


if __name__ == "__main__":
    sys.exit(main())
