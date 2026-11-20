#!/usr/bin/env python3
"""
Check the paste tables against the real keyboard layouts.

check_layouts.mjs proves the tables are self-consistent - no two characters on
one key. This proves they are *right*: every character we send is compared with
where the actual layout puts it, taken from the X keyboard database in
/usr/share/X11/xkb/symbols (xkeyboard-config), includes and all.

That is the failure this catches: a table copied from a neighbouring layout and
left one key wrong. It happened - the Ukrainian table inherited the backslash
from the Russian one, where Ukrainian has ґ on that key, so pasting a Windows
path would have typed a Cyrillic letter into it.

    python3 tools/check_layouts_xkb.py

It reads the tables from the console itself, through
`node tools/check_layouts.mjs --json`. For the generated layouts it checks the
Linux table and the shared one, dead keys included: the first press of a pair
has to be a dead key there, and the pair has to give that character. Their
Windows side is checked by `tools/gen_layouts.py --check`.

Skips itself, rather than failing, where the xkb database is not installed.
"""

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xkbdb  # noqa: E402

# Hand-written tables and the xkb section each one follows.
HAND = [("cs_cz", "cz", "basic"), ("uk_ua", "ua", "unicode"), ("lt_lt", "lt", "basic"),
        ("ru_ru", "ru", "winkeys"), ("en_us", "us", "basic")]


def generated_sections():
    """id -> (xkb file, section), from the generator's own list."""
    import gen_layouts
    return {lid: (xf, xs) for lid, _, _, xf, xs in gen_layouts.LAYOUTS}


def check(label, table, fname, sect):
    slots = xkbdb.slots(fname, sect)
    problems = []

    def typed(e):
        return xkbdb.keysym_char(slots.get((e["hid"], e["mod"]), "NoSymbol"))

    for ch, e in table["map"].items():
        if ch in " \t\n\r":
            continue  # whitespace keys are not in the alphanumeric sections
        got = typed(e)
        if got != ch:
            problems.append("%r we send %02x/%02x, xkb types %r there" % (ch, e["hid"], e["mod"], got))
    for ch, seq in table.get("seq", {}).items():
        first, second = seq
        sym = slots.get((first["hid"], first["mod"]), "NoSymbol")
        if not sym.startswith("dead_"):
            problems.append("%r starts with %02x/%02x, which is %s, not a dead key" % (ch, first["hid"], first["mod"], sym))
            continue
        base = " " if second["hid"] == 0x2c and second["mod"] == 0 else typed(second)
        got = xkbdb.dead_result(sym, base) if base else None
        if got != ch:
            problems.append("%r = %s then %r, which gives %r" % (ch, sym, base, got))
    print("== %-14s %d chars, %d problems" % (label, len(table["map"]) + len(table.get("seq", {})), len(problems)))
    for p in problems:
        print("   ", p)
    return len(problems)


def load_tables() -> dict:
    """The layout tables, straight from the console's own source."""
    here = os.path.dirname(os.path.abspath(__file__))
    out = subprocess.run(
        ["node", os.path.join(here, "check_layouts.mjs"), "--json"],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.exit(out.stderr.strip() or "could not read the layout tables")
    return json.loads(out.stdout)


if __name__ == "__main__":
    if not xkbdb.available():
        msg = f"{xkbdb.XKB} is not here - install xkb-data to run this check"
        if "--require-xkb" in sys.argv:
            sys.exit(msg)
        print(msg)
        sys.exit(0)
    tables = load_tables()
    bad = 0
    for lid, fname, sect in HAND:
        bad += check(lid, tables[lid], fname, sect)
    for lid, (fname, sect) in generated_sections().items():
        t = tables.get(lid)
        if not t:
            print("== %s is missing from the console" % lid)
            bad += 1
            continue
        bad += check(lid, t, fname, sect)
        bad += check(lid + " linux", t["os"]["linux"], fname, sect)
    sys.exit(1 if bad else 0)
