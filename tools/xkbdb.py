"""
The X keyboard database (xkeyboard-config), read the way the layout tools need it.

resolve() gives, for one xkb_symbols section with its includes, the keysym names
on each key and level. keysym_char() turns a keysym name into the character it
types, through libxkbcommon. dead_result() says what a dead key followed by a
base character gives on Linux.

Shared by gen_layouts.py and check_layouts_xkb.py.
"""

import ctypes
import ctypes.util
import os
import re
import sys
import unicodedata

XKB = "/usr/share/X11/xkb/symbols"

# xkb key name -> HID usage. BKSL is the backslash key on ANSI boards and the
# key left of Enter on ISO ones: Windows and Linux read both HID codes as the
# same key, so it is 0x31 either way. LSGT is the ISO key left of Z, AB11 the
# extra key of the Brazilian ABNT2 board.
HID = {"TLDE": 0x35, "BKSL": 0x31, "SPCE": 0x2c, "LSGT": 0x64, "AB11": 0x87}
for _i, _h in enumerate([0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e]):
    HID["AE%02d" % (_i + 1)] = _h
for _i, _h in enumerate([0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12, 0x13, 0x2f, 0x30]):
    HID["AD%02d" % (_i + 1)] = _h
for _i, _h in enumerate([0x04, 0x16, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33, 0x34]):
    HID["AC%02d" % (_i + 1)] = _h
for _i, _h in enumerate([0x1d, 0x1b, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36, 0x37, 0x38]):
    HID["AB%02d" % (_i + 1)] = _h

# Level -> the modifiers a HID report carries for it: none, Shift, AltGr, both.
LEVEL_MOD = [0x00, 0x02, 0x40, 0x42]

# Dead keysym -> the combining mark it puts on the next letter.
DEAD_MARK = {
    "dead_grave": "̀", "dead_acute": "́", "dead_circumflex": "̂",
    "dead_tilde": "̃", "dead_macron": "̄", "dead_breve": "̆",
    "dead_abovedot": "̇", "dead_diaeresis": "̈", "dead_abovering": "̊",
    "dead_doubleacute": "̋", "dead_caron": "̌", "dead_cedilla": "̧",
    "dead_ogonek": "̨",
}
# Dead key then Space on Linux (the Compose file). Two differ from Windows,
# which gives the spacing accent itself: ´ and ¨ come out as ' and " here.
DEAD_SPACE = {
    "dead_grave": "`", "dead_acute": "'", "dead_circumflex": "^", "dead_tilde": "~",
    "dead_macron": "¯", "dead_breve": "˘", "dead_abovedot": "˙",
    "dead_diaeresis": '"', "dead_abovering": "°", "dead_doubleacute": "˝",
    "dead_caron": "ˇ", "dead_cedilla": "¸", "dead_ogonek": "˛",
}

_lib = None


def _xkbcommon():
    global _lib
    if _lib is None:
        name = ctypes.util.find_library("xkbcommon") or "libxkbcommon.so.0"
        try:
            _lib = ctypes.CDLL(name)
        except OSError:
            sys.exit("libxkbcommon is not installed (apt install libxkbcommon0)")
        _lib.xkb_keysym_from_name.argtypes = [ctypes.c_char_p, ctypes.c_int]
        _lib.xkb_keysym_from_name.restype = ctypes.c_uint32
        _lib.xkb_keysym_to_utf32.argtypes = [ctypes.c_uint32]
        _lib.xkb_keysym_to_utf32.restype = ctypes.c_uint32
    return _lib


def keysym_char(name):
    """The character a keysym name types, or None (dead keys, NoSymbol, ...)."""
    if name in ("NoSymbol", "VoidSymbol") or name.startswith("dead_"):
        return None
    lib = _xkbcommon()
    sym = lib.xkb_keysym_from_name(name.encode(), 0)
    if not sym:
        return None
    cp = lib.xkb_keysym_to_utf32(sym)
    if cp < 0x20 or cp == 0x7f:
        return None
    return chr(cp)


def dead_result(dead, base):
    """What dead keysym @p dead then character @p base types on Linux, or None."""
    if base == " ":
        return DEAD_SPACE.get(dead)
    mark = DEAD_MARK.get(dead)
    if not mark:
        return None
    out = unicodedata.normalize("NFC", base + mark)
    return out if len(out) == 1 else None


def available():
    return os.path.isdir(XKB)


def _section(fname, name):
    text = open(os.path.join(XKB, fname), encoding="utf-8").read()
    if name is None:
        m = re.search(r'default[^\n]*\n\s*xkb_symbols\s+"([^"]+)"', text)
        name = m.group(1) if m else "basic"
    pat = re.compile(r'xkb_symbols\s+"%s"\s*\{(.*?)\n\};' % re.escape(name), re.S)
    m = pat.search(text)
    if not m:
        sys.exit("no section %s in %s" % (name, fname))
    return m.group(1)


def resolve(fname, name, seen=None):
    """key -> [level1..level4 keysym names], following includes (base first)."""
    seen = seen if seen is not None else set()
    if (fname, name) in seen:
        return {}  # an include cycle; the first visit already carried it
    seen.add((fname, name))
    keys = {}
    body = re.sub(r"//[^\n]*", "", _section(fname, name))
    for line in body.split("\n"):
        line = line.strip()
        inc = re.match(r'include\s+"([^"]+)"', line)
        if inc:
            spec = inc.group(1)
            if spec.startswith("level3") or spec.startswith("kpdl") or spec.startswith("nbsp"):
                continue
            f, s = (spec.split("(", 1)[0], spec.split("(", 1)[1].rstrip(")")) if "(" in spec else (spec, None)
            keys.update(resolve(f, s, seen))
    for key, inner in re.findall(r'key\s*<([A-Z0-9]+)>\s*\{(.*?)\}\s*;', body, re.S):
        syms = re.search(r'(?:symbols\[[^\]]*\]\s*=\s*)?\[([^\]]*)\]', inner.replace("type[group1]", "").replace("type[Group1]", ""))
        if not syms:
            continue
        levels = [s.strip() for s in syms.group(1).split(",")]
        cur = keys.get(key, [])
        merged = []
        for i in range(max(len(levels), len(cur))):
            new = levels[i] if i < len(levels) else "NoSymbol"
            old = cur[i] if i < len(cur) else "NoSymbol"
            merged.append(old if new == "NoSymbol" else new)
        keys[key] = merged
    return keys


def slots(fname, name):
    """(hid, mod) -> keysym name, for the keys a KVM can press.

    A desktop builds its keymap as pc+<layout>, so the pc section goes first:
    it is where most layouts get the ISO key left of Z from."""
    keys = resolve("pc", "pc105")
    keys.update(resolve(fname, name))
    out = {}
    for key, levels in keys.items():
        if key not in HID:
            continue
        for lvl, sym in enumerate(levels[:4]):
            out[(HID[key], LEVEL_MOD[lvl])] = sym
    return out
