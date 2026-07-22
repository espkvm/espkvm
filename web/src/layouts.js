/*
 * Character -> HID scancode tables for pasting text into the target.
 *
 * A KVM cannot type characters, only key positions. What appears on the target
 * depends on the layout the target itself has active, so the table has to match
 * it: sending the "A" key position produces "a" on a US layout and "ф" on a
 * Russian one. That is what the kbd_layout setting selects.
 *
 * Characters absent from a table are skipped rather than guessed, because a
 * wrong guess types the wrong character into whatever has focus.
 */

export const HID_MOD_LCTRL = 0x01;
export const HID_MOD_LSHIFT = 0x02;
export const HID_MOD_LALT = 0x04;
export const HID_MOD_LGUI = 0x08;

/* US key positions, named for what they produce on a US layout. */
const K = {
  a: 0x04, b: 0x05, c: 0x06, d: 0x07, e: 0x08, f: 0x09, g: 0x0a, h: 0x0b,
  i: 0x0c, j: 0x0d, k: 0x0e, l: 0x0f, m: 0x10, n: 0x11, o: 0x12, p: 0x13,
  q: 0x14, r: 0x15, s: 0x16, t: 0x17, u: 0x18, v: 0x19, w: 0x1a, x: 0x1b,
  y: 0x1c, z: 0x1d,
  d1: 0x1e, d2: 0x1f, d3: 0x20, d4: 0x21, d5: 0x22,
  d6: 0x23, d7: 0x24, d8: 0x25, d9: 0x26, d0: 0x27,
  enter: 0x28, esc: 0x29, backspace: 0x2a, tab: 0x2b, space: 0x2c,
  minus: 0x2d, equal: 0x2e, lbracket: 0x2f, rbracket: 0x30, backslash: 0x31,
  semicolon: 0x33, quote: 0x34, grave: 0x35, comma: 0x36, period: 0x37, slash: 0x38,
};

const SH = HID_MOD_LSHIFT;

/** Keys every layout shares: whitespace and the digit row without shift. */
function addCommon(map) {
  map[" "] = { mod: 0, hid: K.space };
  map["\n"] = { mod: 0, hid: K.enter };
  map["\r"] = { mod: 0, hid: K.enter };
  map["\t"] = { mod: 0, hid: K.tab };
  const digits = "1234567890";
  const digitKeys = [K.d1, K.d2, K.d3, K.d4, K.d5, K.d6, K.d7, K.d8, K.d9, K.d0];
  for (let i = 0; i < digits.length; i++) {
    map[digits[i]] = { mod: 0, hid: digitKeys[i] };
  }
  return map;
}

function buildUs() {
  const m = addCommon({});
  for (let i = 0; i < 26; i++) {
    const lower = String.fromCharCode(97 + i);
    const upper = String.fromCharCode(65 + i);
    m[lower] = { mod: 0, hid: K.a + i };
    m[upper] = { mod: SH, hid: K.a + i };
  }
  const shiftedDigits = "!@#$%^&*()";
  const digitKeys = [K.d1, K.d2, K.d3, K.d4, K.d5, K.d6, K.d7, K.d8, K.d9, K.d0];
  for (let i = 0; i < shiftedDigits.length; i++) {
    m[shiftedDigits[i]] = { mod: SH, hid: digitKeys[i] };
  }
  const pairs = [
    ["-", "_", K.minus], ["=", "+", K.equal], ["[", "{", K.lbracket],
    ["]", "}", K.rbracket], ["\\", "|", K.backslash], [";", ":", K.semicolon],
    ["'", '"', K.quote], ["`", "~", K.grave], [",", "<", K.comma],
    [".", ">", K.period], ["/", "?", K.slash],
  ];
  for (const [plain, shifted, hid] of pairs) {
    m[plain] = { mod: 0, hid };
    m[shifted] = { mod: SH, hid };
  }
  return m;
}

/*
 * Russian ЙЦУКЕН as standardised on Windows and used by every desktop Linux
 * layout named "ru". Latin letters are deliberately absent: with a Russian
 * layout active on the target there is no key position that produces them.
 */
function buildRu() {
  const m = addCommon({});

  const letters = [
    ["й", K.q], ["ц", K.w], ["у", K.e], ["к", K.r], ["е", K.t], ["н", K.y],
    ["г", K.u], ["ш", K.i], ["щ", K.o], ["з", K.p], ["х", K.lbracket], ["ъ", K.rbracket],
    ["ф", K.a], ["ы", K.s], ["в", K.d], ["а", K.f], ["п", K.g], ["р", K.h],
    ["о", K.j], ["л", K.k], ["д", K.l], ["ж", K.semicolon], ["э", K.quote],
    ["я", K.z], ["ч", K.x], ["с", K.c], ["м", K.v], ["и", K.b], ["т", K.n],
    ["ь", K.m], ["б", K.comma], ["ю", K.period], ["ё", K.grave],
  ];
  for (const [ch, hid] of letters) {
    m[ch] = { mod: 0, hid };
    m[ch.toUpperCase()] = { mod: SH, hid };
  }

  /* Punctuation sits differently than on US: the slash key carries the full
     stop, and most marks live on the shifted digit row. */
  m["."] = { mod: 0, hid: K.slash };
  m[","] = { mod: SH, hid: K.slash };
  m["\\"] = { mod: 0, hid: K.backslash };
  m["/"] = { mod: SH, hid: K.backslash };
  m["-"] = { mod: 0, hid: K.minus };
  m["_"] = { mod: SH, hid: K.minus };
  m["="] = { mod: 0, hid: K.equal };
  m["+"] = { mod: SH, hid: K.equal };
  m["!"] = { mod: SH, hid: K.d1 };
  m['"'] = { mod: SH, hid: K.d2 };
  m["\u2116"] = { mod: SH, hid: K.d3 };
  m[";"] = { mod: SH, hid: K.d4 };
  m["%"] = { mod: SH, hid: K.d5 };
  m[":"] = { mod: SH, hid: K.d6 };
  m["?"] = { mod: SH, hid: K.d7 };
  m["*"] = { mod: SH, hid: K.d8 };
  m["("] = { mod: SH, hid: K.d9 };
  m[")"] = { mod: SH, hid: K.d0 };
  return m;
}

export const LAYOUTS = {
  en_us: { label: "English (US)", map: buildUs() },
  ru_ru: { label: "Русская", map: buildRu() },
};

export const DEFAULT_LAYOUT = "en_us";

/** @return {{mod:number,hid:number}|null} null when the layout cannot type it. */
export function charToHid(layoutId, ch) {
  const layout = LAYOUTS[layoutId] || LAYOUTS[DEFAULT_LAYOUT];
  return layout.map[ch] || null;
}

/** Characters in @p text that the layout cannot produce, for warning the user. */
export function untypeableChars(layoutId, text) {
  const missing = new Set();
  for (const ch of text) {
    if (ch === "\r") continue;
    if (!charToHid(layoutId, ch)) missing.add(ch);
  }
  return [...missing];
}
