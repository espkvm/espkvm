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
`node tools/check_layouts.mjs --json`.

Skips itself, rather than failing, where the xkb database is not installed.
"""

import json
import os
import re
import subprocess
import sys

XKB = "/usr/share/X11/xkb/symbols"

HID = {"TLDE":0x35,"BKSL":0x31,"SPCE":0x2c}
for i,h in enumerate([0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2d,0x2e]):
    HID["AE%02d"%(i+1)] = h
for i,h in enumerate([0x14,0x1a,0x08,0x15,0x17,0x1c,0x18,0x0c,0x12,0x13,0x2f,0x30]):
    HID["AD%02d"%(i+1)] = h
for i,h in enumerate([0x04,0x16,0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x0f,0x33,0x34]):
    HID["AC%02d"%(i+1)] = h
for i,h in enumerate([0x1d,0x1b,0x06,0x19,0x05,0x11,0x10,0x36,0x37,0x38]):
    HID["AB%02d"%(i+1)] = h

NAMES = {
 "grave":"`","asciitilde":"~","exclam":"!","at":"@","numbersign":"#","dollar":"$",
 "percent":"%","asciicircum":"^","ampersand":"&","asterisk":"*","parenleft":"(",
 "parenright":")","minus":"-","underscore":"_","equal":"=","plus":"+",
 "bracketleft":"[","braceleft":"{","bracketright":"]","braceright":"}",
 "backslash":"\\","bar":"|","semicolon":";","colon":":","apostrophe":"'",
 "quotedbl":'"',"comma":",","less":"<","period":".","greater":">","slash":"/",
 "question":"?","space":" ",
 "aogonek":"ą","Aogonek":"Ą","ccaron":"č","Ccaron":"Č","eogonek":"ę","Eogonek":"Ę",
 "eabovedot":"ė","Eabovedot":"Ė","iogonek":"į","Iogonek":"Į","scaron":"š","Scaron":"Š",
 "uogonek":"ų","Uogonek":"Ų","umacron":"ū","Umacron":"Ū","zcaron":"ž","Zcaron":"Ž",
 "ecaron":"ě","Ecaron":"Ě","rcaron":"ř","Rcaron":"Ř","yacute":"ý","Yacute":"Ý",
 "aacute":"á","Aacute":"Á","iacute":"í","Iacute":"Í","eacute":"é","Eacute":"É",
 "uring":"ů","Uring":"Ů","uacute":"ú","Uacute":"Ú",
 "doublelowquotemark":"„","leftdoublequotemark":"“",
 "Ukrainian_ghe_with_upturn":"ґ","Ukrainian_GHE_WITH_UPTURN":"Ґ",
 "Ukrainian_yi":"ї","Ukrainian_YI":"Ї","Ukrainian_i":"і","Ukrainian_I":"І",
 "Ukrainian_ie":"є","Ukrainian_IE":"Є","numerosign":"№",
}
CYR = {"a":"а","be":"б","ve":"в","ghe":"г","de":"д","ie":"е","io":"ё","zhe":"ж",
       "ze":"з","i":"и","shorti":"й","ka":"к","el":"л","em":"м","en":"н","o":"о",
       "pe":"п","er":"р","es":"с","te":"т","u":"у","ef":"ф","ha":"х","tse":"ц",
       "che":"ч","sha":"ш","shcha":"щ","hardsign":"ъ","yeru":"ы","softsign":"ь",
       "e":"э","yu":"ю","ya":"я"}
for k, v in CYR.items():
    NAMES["Cyrillic_" + k] = v
    NAMES["Cyrillic_" + k.upper()] = v.upper()

for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    NAMES[c] = c
for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    NAMES[c] = c

def section(fname, name):
    text = open(os.path.join(XKB, fname), encoding="utf-8").read()
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
    body = section(fname, name)
    for inc in re.findall(r'include\s+"([^"]+)"', body):
        if inc.startswith("level3"):
            continue
        if "(" in inc:
            f, s = inc.split("(", 1)
            s = s.rstrip(")")
        else:
            f, s = inc, "basic"
        keys.update(resolve(f, s, seen))
    for key, syms in re.findall(r'key\s*<([A-Z0-9]+)>\s*\{\s*\[(.*?)\]\s*\}', body, re.S):
        levels = [s.strip() for s in syms.split(",")]
        cur = keys.get(key, [])
        merged = []
        for i in range(max(len(levels), len(cur))):
            new = levels[i] if i < len(levels) else "NoSymbol"
            old = cur[i] if i < len(cur) else "NoSymbol"
            merged.append(old if new == "NoSymbol" else new)
        keys[key] = merged
    return keys

def expected(fname, name):
    """char -> set of (hid, mod) the real layout offers."""
    out = {}
    for key, levels in resolve(fname, name).items():
        if key not in HID:
            continue
        for lvl, sym in enumerate(levels[:4]):
            ch = NAMES.get(sym)
            if ch is None:
                continue
            mod = [0x00, 0x02, 0x40, 0x42][lvl]
            out.setdefault(ch, set()).add((HID[key], mod))
    return out

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
    if not os.path.isdir(XKB):
        msg = f"{XKB} is not here - install xkeyboard-config to run this check"
        if "--require-xkb" in sys.argv:
            sys.exit(msg)
        print(msg)
        sys.exit(0)
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    table = json.load(open(args[0])) if args else load_tables()
    bad = 0
    for layout, fname, sect in [("cs_cz","cz","basic"), ("uk_ua","ua","unicode"),
                                ("lt_lt","lt","basic"), ("ru_ru","ru","winkeys"),
                                ("en_us","us","basic")]:
        exp = expected(fname, sect)
        mine = table.get(layout, {})
        problems = []
        for ch, ks in mine.items():
            if ch in " \t\n\r":
                continue  # whitespace keys are not in the alphanumeric sections
            got = (ks["hid"], ks["mod"])
            if ch not in exp:
                problems.append("%r not on this layout at all (we send %02x/%02x)" % (ch, got[0], got[1]))
            elif got not in exp[ch]:
                want = ", ".join("%02x/%02x" % k for k in sorted(exp[ch]))
                problems.append("%r we send %02x/%02x, xkb says %s" % (ch, got[0], got[1], want))
        print("== %-6s %d chars, %d problems" % (layout, len(mine), len(problems)))
        for p in problems:
            print("   ", p)
        bad += len(problems)
    sys.exit(1 if bad else 0)
