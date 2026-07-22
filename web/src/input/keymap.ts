/*
 * Browser KeyboardEvent.code -> HID usage.
 *
 * `code` is deliberate: it names the physical key position, which is what a
 * HID report carries. Using `key` would send whatever the operator's own
 * layout produces, and the target would then apply its layout on top - press
 * "ф" on a Russian keyboard and the target would receive the letter twice
 * translated.
 *
 * Ported unchanged from the client that was verified against real hardware.
 */

export const HID_MOD_LCTRL = 0x01;
export const HID_MOD_LSHIFT = 0x02;
export const HID_MOD_LALT = 0x04;
export const HID_MOD_LGUI = 0x08;
export const HID_MOD_RCTRL = 0x10;
export const HID_MOD_RSHIFT = 0x20;
export const HID_MOD_RALT = 0x40;
export const HID_MOD_RGUI = 0x80;

const MODIFIER_CODES: Record<string, number> = {
  ControlLeft: HID_MOD_LCTRL,
  ShiftLeft: HID_MOD_LSHIFT,
  AltLeft: HID_MOD_LALT,
  MetaLeft: HID_MOD_LGUI,
  ControlRight: HID_MOD_RCTRL,
  ShiftRight: HID_MOD_RSHIFT,
  AltRight: HID_MOD_RALT,
  MetaRight: HID_MOD_RGUI,
};

function buildCodeMap(): Record<string, number> {
  const m: Record<string, number> = {};

  for (let i = 0; i < 26; i++) {
    m[`Key${String.fromCharCode(65 + i)}`] = 0x04 + i;
  }
  const digits = ["Digit1", "Digit2", "Digit3", "Digit4", "Digit5",
                  "Digit6", "Digit7", "Digit8", "Digit9", "Digit0"];
  digits.forEach((code, i) => {
    m[code] = 0x1e + i;
  });

  const rest: Array<[string, number]> = [
    ["Enter", 0x28], ["Escape", 0x29], ["Backspace", 0x2a], ["Tab", 0x2b],
    ["Space", 0x2c], ["Minus", 0x2d], ["Equal", 0x2e], ["BracketLeft", 0x2f],
    ["BracketRight", 0x30], ["Backslash", 0x31], ["Semicolon", 0x33],
    ["Quote", 0x34], ["Backquote", 0x35], ["Comma", 0x36], ["Period", 0x37],
    ["Slash", 0x38], ["CapsLock", 0x39],
    ["F1", 0x3a], ["F2", 0x3b], ["F3", 0x3c], ["F4", 0x3d], ["F5", 0x3e],
    ["F6", 0x3f], ["F7", 0x40], ["F8", 0x41], ["F9", 0x42], ["F10", 0x43],
    ["F11", 0x44], ["F12", 0x45],
    ["PrintScreen", 0x46], ["ScrollLock", 0x47], ["Pause", 0x48],
    ["Insert", 0x49], ["Home", 0x4a], ["PageUp", 0x4b], ["Delete", 0x4c],
    ["End", 0x4d], ["PageDown", 0x4e],
    ["ArrowRight", 0x4f], ["ArrowLeft", 0x50], ["ArrowDown", 0x51], ["ArrowUp", 0x52],
    ["NumLock", 0x53], ["NumpadDivide", 0x54], ["NumpadMultiply", 0x55],
    ["NumpadSubtract", 0x56], ["NumpadAdd", 0x57], ["NumpadEnter", 0x58],
    ["Numpad1", 0x59], ["Numpad2", 0x5a], ["Numpad3", 0x5b], ["Numpad4", 0x5c],
    ["Numpad5", 0x5d], ["Numpad6", 0x5e], ["Numpad7", 0x5f], ["Numpad8", 0x60],
    ["Numpad9", 0x61], ["Numpad0", 0x62], ["NumpadDecimal", 0x63],
    ["ContextMenu", 0x65],
  ];
  for (const [code, usage] of rest) m[code] = usage;
  return m;
}

const CODE_TO_HID = buildCodeMap();

/** HID usage for a physical key, or 0 when we have no mapping for it. */
export function usageForCode(code: string): number {
  return CODE_TO_HID[code] ?? 0;
}

export function isModifierCode(code: string): boolean {
  return code in MODIFIER_CODES;
}

/** Modifier bitmap from the event's own modifier state. */
export function modifierMask(e: KeyboardEvent): number {
  let mask = 0;
  if (e.ctrlKey) mask |= HID_MOD_LCTRL;
  if (e.shiftKey) mask |= HID_MOD_LSHIFT;
  if (e.altKey) mask |= HID_MOD_LALT;
  if (e.metaKey) mask |= HID_MOD_LGUI;
  return mask;
}

export function modifierForCode(code: string): number {
  return MODIFIER_CODES[code] ?? 0;
}
