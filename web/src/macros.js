/*
 * Key combinations the browser will not let through.
 *
 * Ctrl+Alt+Del, Alt+F4, Alt+Tab and the Meta key are all intercepted by the
 * operator's own OS or browser long before a page sees them, so a remote
 * console has to offer them as buttons. Each one is sent as a single HID
 * report: modifiers plus one key, then an all-released report.
 *
 * `destructive` marks combinations that can end a session or lose unsaved work
 * on the target. The UI asks before sending those - a misclick that closes the
 * wrong window on a remote machine is expensive to undo.
 */

import { HID_MOD_LALT, HID_MOD_LCTRL, HID_MOD_LGUI, HID_MOD_LSHIFT } from "./layouts.js";

const KEY_TAB = 0x2b;
const KEY_ESC = 0x29;
const KEY_DELETE = 0x4c;
const KEY_PRINTSCREEN = 0x46;
const KEY_F4 = 0x3d;
const KEY_L = 0x0f;

export const MACROS = [
  {
    id: "ctrl_alt_del",
    label: "Ctrl+Alt+Del",
    modifier: HID_MOD_LCTRL | HID_MOD_LALT,
    key: KEY_DELETE,
    destructive: true,
    hint: "Security screen on Windows, logout prompt on most Linux desktops.",
  },
  {
    id: "alt_tab",
    label: "Alt+Tab",
    modifier: HID_MOD_LALT,
    key: KEY_TAB,
    hint: "Switch to the previously focused window.",
  },
  {
    id: "meta",
    label: "Win",
    modifier: HID_MOD_LGUI,
    key: 0,
    hint: "Start menu or activities overview.",
  },
  {
    id: "prtsc",
    label: "PrtSc",
    modifier: 0,
    key: KEY_PRINTSCREEN,
    hint: "Screenshot on the target.",
  },
  {
    id: "esc",
    label: "Esc",
    modifier: 0,
    key: KEY_ESC,
  },
  {
    id: "ctrl_shift_esc",
    label: "Ctrl+Shift+Esc",
    modifier: HID_MOD_LCTRL | HID_MOD_LSHIFT,
    key: KEY_ESC,
    hint: "Task Manager on Windows.",
  },
  {
    id: "meta_l",
    label: "Win+L",
    modifier: HID_MOD_LGUI,
    key: KEY_L,
    destructive: true,
    hint: "Locks the target. You will need its password to get back in.",
  },
  {
    id: "alt_f4",
    label: "Alt+F4",
    modifier: HID_MOD_LALT,
    key: KEY_F4,
    destructive: true,
    hint: "Closes the focused window, discarding unsaved work.",
  },
];

export function findMacro(id) {
  return MACROS.find((m) => m.id === id) || null;
}
