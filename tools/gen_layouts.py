#!/usr/bin/env python3
"""
Generate web/src/layoutsGen.ts, the paste tables for the Western layouts.

    python3 tools/gen_layouts.py           write it
    python3 tools/gen_layouts.py --check   fail if it is out of date

Each layout is written out twice, because Windows and Linux do not always
agree on it (French ~ is a dead key on Windows and a plain one on Linux):

- the Windows layout, from Unicode CLDR (keyboards/windows, release 42, the last
  release that carried them), downloaded when this runs;
- the Linux layout of the same name, from the X keyboard database.

The console picks the one for the target's OS and, when the OS is not known,
uses only what the two agree on (layouts.ts).

Needs network (CLDR), xkb-data and libxkbcommon.
"""

import os
import re
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xkbdb  # noqa: E402

CLDR = "https://raw.githubusercontent.com/unicode-org/cldr/release-42/keyboards/windows/"

# id, label (in the layout's language), CLDR file, xkb file, xkb section.
# New ids go on the end of s_layout_choices in kvm_settings_table.c as well.
LAYOUTS = [
    ("de_de", "Deutsch", "de-t-k0-windows", "de", "basic"),
    ("fr_fr", "Français", "fr-t-k0-windows", "fr", "basic"),
    ("es_es", "Español", "es-t-k0-windows", "es", "basic"),
    ("en_gb", "English (UK)", "en-GB-t-k0-windows", "gb", "basic"),
    ("it_it", "Italiano", "it-t-k0-windows", "it", "basic"),
    ("en_intl", "English (US, international)", "en-t-k0-windows-extended", "us", "intl"),
    ("pt_pt", "Português", "pt-PT-t-k0-windows", "pt", "basic"),
    ("pt_br", "Português (Brasil)", "pt-t-k0-windows", "br", "abnt2"),
    ("es_latam", "Español (Latinoamérica)", "es-419-t-k0-windows", "latam", "basic"),
    ("sv_se", "Svenska / Suomi", "sv-t-k0-windows", "se", "basic"),
    ("nb_no", "Norsk", "nb-t-k0-windows", "no", "basic"),
    ("da_dk", "Dansk", "da-t-k0-windows", "dk", "basic"),
    ("pl_pl", "Polski", "pl-t-k0-windows-extended", "pl", "basic"),
    ("fr_be", "Belge", "fr-BE-t-k0-windows", "be", "basic"),
    ("de_ch", "Schweiz (Deutsch)", "de-CH-t-k0-windows", "ch", "basic"),
    ("fr_ch", "Suisse (Français)", "fr-CH-t-k0-windows", "ch", "fr"),
]

# CLDR's ISO key positions -> HID. D13 (ANSI) and C12 (ISO) are the same key
# to Windows and Linux, see xkbdb.HID.
ISO = {"E00": 0x35, "D13": 0x31, "C12": 0x31, "B00": 0x64, "B11": 0x87}
for _i in range(1, 13):
    ISO["E%02d" % _i] = xkbdb.HID["AE%02d" % _i]
    ISO["D%02d" % _i] = xkbdb.HID["AD%02d" % _i]
for _i in range(1, 12):
    ISO["C%02d" % _i] = xkbdb.HID["AC%02d" % _i]
for _i in range(1, 11):
    ISO["B%02d" % _i] = xkbdb.HID["AB%02d" % _i]

# The key order of the generated strings: one character per key and level.
ORDER = sorted(set(ISO.values()), key=lambda h: [0x35, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
                                                  0x26, 0x27, 0x2d, 0x2e, 0x14, 0x1a, 0x08, 0x15, 0x17,
                                                  0x1c, 0x18, 0x0c, 0x12, 0x13, 0x2f, 0x30, 0x31, 0x04,
                                                  0x16, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33,
                                                  0x34, 0x64, 0x1d, 0x1b, 0x06, 0x19, 0x05, 0x11, 0x10,
                                                  0x36, 0x37, 0x38, 0x87].index(h))
NONE = "\u0000"  # written out as a space: no key in KEYS types one
MODS = [0x00, 0x02, 0x40, 0x42]

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "..", "web", "src", "layoutsGen.ts")


def unescape(s):
    return re.sub(r"\\u\{([0-9A-Fa-f]+)\}", lambda m: chr(int(m.group(1), 16)), s)


def modifier_level(attr):
    """CLDR keyMap modifiers -> HID modifier byte, or None for maps we skip."""
    if attr is None:
        return 0x00
    for alt in attr.split():
        need = {p for p in alt.split("+") if not p.endswith("?")}
        if need == {"shift"}:
            return 0x02
        if need == {"altR"}:
            return 0x40
        if need == {"altR", "shift"}:
            return 0x42
    return None


def read_cldr(name, cache):
    path = os.path.join(cache, name + ".xml")
    if not os.path.exists(path):
        with urllib.request.urlopen(CLDR + name + ".xml", timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    root = ET.parse(path).getroot()
    keys = {}  # (hid, mod) -> char
    for km in root.findall("keyMap"):
        mod = modifier_level(km.get("modifiers"))
        if mod is None:
            continue
        for m in km.findall("map"):
            ch = unescape(m.get("to", ""))
            hid = ISO.get(m.get("iso"))
            if hid is not None and len(ch) == 1 and ord(ch) >= 0x20:
                keys[(hid, mod)] = ch
    compose = {}  # (dead char, base char) -> result
    for tr in root.iter("transform"):
        frm, to = unescape(tr.get("from")), unescape(tr.get("to"))
        if len(frm) == 2 and len(to) == 1:
            compose[(frm[0], frm[1])] = to
    return keys, compose


# Dead keysym -> the accent it is shown as in the Linux table.
DEAD_CHAR = {
    "dead_grave": "`", "dead_acute": "\u00b4", "dead_circumflex": "^", "dead_tilde": "~",
    "dead_macron": "\u00af", "dead_breve": "\u02d8", "dead_abovedot": "\u02d9",
    "dead_diaeresis": "\u00a8", "dead_abovering": "\u00b0", "dead_doubleacute": "\u02dd",
    "dead_caron": "\u02c7", "dead_cedilla": "\u00b8", "dead_ogonek": "\u02db",
}
# The letters a dead key is tried on for the Linux pairs.
BASES = "aceinouyszrlgAECINOUYSZRLG "


def windows_table(cldr_name, cache):
    keys, compose = read_cldr(cldr_name, cache)
    dead_chars = {d for d, _ in compose}
    direct = {s: ch for s, ch in keys.items() if ch not in dead_chars}
    dead = {s: ch for s, ch in keys.items() if ch in dead_chars}
    pairs = [(d, b, o) for (d, b), o in sorted(compose.items())]
    return direct, dead, pairs


def linux_table(xkb_file, xkb_section):
    direct, dead = {}, {}
    for slot, sym in xkbdb.slots(xkb_file, xkb_section).items():
        if slot[0] not in ORDER:
            continue
        if sym == "space":
            sys.exit("%s(%s) has a space on a key: the tables use space for none" % (xkb_file, xkb_section))
        if sym in DEAD_CHAR:
            dead[slot] = DEAD_CHAR[sym]
            continue
        ch = xkbdb.keysym_char(sym)
        if ch is not None:
            direct[slot] = ch
    typed = set(direct.values())
    sym_of = {v: k for k, v in DEAD_CHAR.items()}
    pairs = []
    for d in sorted(set(dead.values())):
        for b in BASES:
            if b != " " and b not in typed:
                continue
            out = xkbdb.dead_result(sym_of[d], b)
            if out and out != b and (b == " " or ord(out) < 0x250):
                pairs.append((d, b, out))
    return direct, dead, pairs


def levels_of(direct):
    rows = []
    for mod in MODS:
        rows.append("".join(direct.get((hid, mod), NONE) for hid in ORDER))
    return rows


def ts_string(s):
    out = '"'
    for ch in s:
        if ch == NONE:
            out += " "
        elif ch in '"\\':
            out += "\\" + ch
        else:
            out += ch
    return out + '"'


def emit_table(lines, name, table):
    direct, dead, pairs = table
    lines.append("    %s: {" % name)
    lines.append("      levels: [")
    for row in levels_of(direct):
        lines.append("        %s," % ts_string(row))
    lines.append("      ],")
    order = sorted(dead.items(), key=lambda kv: (MODS.index(kv[0][1]), ORDER.index(kv[0][0])))
    dead_items = ", ".join("[%s, 0x%02x, 0x%02x]" % (ts_string(ch), mod, hid)
                           for (hid, mod), ch in order)
    lines.append("      dead: [%s]," % dead_items)
    lines.append("      compose: %s," % ts_string("".join(d + b + o for d, b, o in pairs)))
    lines.append("    },")


def generate(cache):
    lines = [
        "/*",
        " * GENERATED by tools/gen_layouts.py - do not edit.",
        " *",
        " * Each layout twice: as Windows has it (Unicode CLDR, release 42) and as",
        " * Linux has it (the X keyboard database). levels: the character on each key",
        " * of KEYS with no modifier, Shift, AltGr and Shift+AltGr, a space for none. dead:",
        " * the dead keys as [accent, modifiers, key]. compose: what a dead key does",
        " * to the next character, as runs of accent, base, result.",
        " */",
        "",
        "export const KEYS = [" + ", ".join("0x%02x" % h for h in ORDER) + "];",
        "",
        "export interface GenTable {",
        "  levels: string[];",
        "  dead: [string, number, number][];",
        "  compose: string;",
        "}",
        "",
        "export interface GenLayout {",
        "  id: string;",
        "  label: string;",
        "  windows: GenTable;",
        "  linux: GenTable;",
        "}",
        "",
        "export const GENERATED: GenLayout[] = [",
    ]
    for lid, label, cldr_name, xf, xs in LAYOUTS:
        lines.append("  {")
        lines.append("    id: %s," % ts_string(lid))
        lines.append("    label: %s," % ts_string(label))
        emit_table(lines, "windows", windows_table(cldr_name, cache))
        emit_table(lines, "linux", linux_table(xf, xs))
        lines.append("  },")
    lines.append("];")
    return "\n".join(lines) + "\n"


def main():
    if not xkbdb.available():
        sys.exit("%s is not here - install xkb-data" % xkbdb.XKB)
    cache = os.environ.get("CLDR_CACHE") or tempfile.mkdtemp(prefix="cldr-kbd-")
    os.makedirs(cache, exist_ok=True)
    text = generate(cache)
    if "--check" in sys.argv:
        current = open(TARGET, encoding="utf-8").read() if os.path.exists(TARGET) else ""
        if current != text:
            sys.exit("layoutsGen.ts is out of date: run python3 tools/gen_layouts.py")
        return
    with open(TARGET, "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    main()
