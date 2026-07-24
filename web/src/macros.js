/*
 * Key combinations the browser will not let through.
 *
 * Ctrl+Alt+Del, Alt+F4, Alt+Tab and the Meta key are all intercepted by the
 * operator's own OS or browser long before a page sees them, so a remote
 * console has to offer them as buttons. Each one is sent as a single HID
 * report: modifiers plus one key, then an all-released report.
 *
 * Some combinations only mean something on one target OS - Task Manager is a
 * Windows idea, the SysRq sequences are a Linux one. Those carry an `os` tag
 * and the console shows them only when the target is (or is set to) that OS.
 * The rest have no tag and are always offered.
 *
 * `destructive` marks combinations that can end a session or lose unsaved work
 * on the target. The UI asks before sending those - a misclick that closes the
 * wrong window on a remote machine is expensive to undo.
 *
 * A macro with a `sysrq` string is not a chord but a timed sequence: the device
 * holds Alt+SysRq and taps each letter in turn (see InputPanel). That is how the
 * Linux magic-SysRq commands have to be delivered.
 */

import { HID_MOD_LALT, HID_MOD_LCTRL, HID_MOD_LGUI, HID_MOD_LSHIFT } from "./layouts.js";

const KEY_TAB = 0x2b;
const KEY_ESC = 0x29;
const KEY_DELETE = 0x4c;
const KEY_PRINTSCREEN = 0x46;
const KEY_F1 = 0x3a;
const KEY_F2 = 0x3b;
const KEY_F3 = 0x3c;
const KEY_F4 = 0x3d;
const KEY_F5 = 0x3e;
const KEY_F6 = 0x3f;
const KEY_L = 0x0f;

/* The Alt+SysRq key itself, and the command letters the kernel understands.
 * SysRq is Alt held together with the PrintScreen key. */
export const SYSRQ_MOD = HID_MOD_LALT;
export const SYSRQ_KEY = KEY_PRINTSCREEN;
export const SYSRQ_LETTERS = {
  r: 0x15, // unraw - take the keyboard back from X
  e: 0x08, // terminate all processes (SIGTERM)
  i: 0x0c, // kill all processes (SIGKILL)
  s: 0x16, // sync all mounted filesystems
  u: 0x18, // remount everything read-only
  b: 0x05, // reboot immediately, no sync
  o: 0x12, // power off
};

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
    /* The same physical key is Win, Command or Super depending on the target. */
    labelByOs: { macos: "Cmd", linux: "Super" },
    modifier: HID_MOD_LGUI,
    key: 0,
    hint: "Start menu, launcher or activities overview.",
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
    os: "windows",
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

  /* ---- Linux ----------------------------------------------------------- */

  {
    id: "reisub",
    label: "REISUB",
    os: "linux",
    sysrq: "reisub",
    destructive: true,
    hint:
      "Safely reboot a hung Linux machine. Holds Alt+SysRq and taps R E I S U B: " +
      "take back the keyboard, end processes, kill what is left, sync the disks, " +
      "remount them read-only, then reboot. Takes several seconds.",
  },
  {
    id: "reisuo",
    label: "REISUO",
    os: "linux",
    sysrq: "reisuo",
    destructive: true,
    hint:
      "Like REISUB but powers the machine off instead of rebooting: the last step " +
      "is O rather than B. Syncs and remounts read-only first, so the disks are safe.",
  },
  { id: "tty_f1", label: "TTY F1", os: "linux", modifier: HID_MOD_LCTRL | HID_MOD_LALT, key: KEY_F1,
    hint: "Switch to virtual terminal 1 (often the graphical session)." },
  { id: "tty_f2", label: "F2", os: "linux", modifier: HID_MOD_LCTRL | HID_MOD_LALT, key: KEY_F2,
    hint: "Switch to virtual terminal 2." },
  { id: "tty_f3", label: "F3", os: "linux", modifier: HID_MOD_LCTRL | HID_MOD_LALT, key: KEY_F3,
    hint: "Switch to virtual terminal 3 (a text console when the GUI is wedged)." },
  { id: "tty_f4", label: "F4", os: "linux", modifier: HID_MOD_LCTRL | HID_MOD_LALT, key: KEY_F4,
    hint: "Switch to virtual terminal 4." },
  { id: "tty_f5", label: "F5", os: "linux", modifier: HID_MOD_LCTRL | HID_MOD_LALT, key: KEY_F5,
    hint: "Switch to virtual terminal 5." },
  { id: "tty_f6", label: "F6", os: "linux", modifier: HID_MOD_LCTRL | HID_MOD_LALT, key: KEY_F6,
    hint: "Switch to virtual terminal 6." },
];

export function findMacro(id) {
  return MACROS.find((m) => m.id === id) || null;
}

/** The label to show for a macro on a given target OS, e.g. Win -> Cmd on macOS. */
export function macroLabel(m, os) {
  return (m.labelByOs && m.labelByOs[os]) || m.label;
}

/**
 * The macros to offer for a target OS. Untagged macros are universal; a tagged
 * one shows only when it matches. When the OS is unknown nothing is hidden, so
 * a failed guess never costs the operator a button they needed.
 */
export function macrosForOs(os) {
  return MACROS.filter((m) => !m.os || os === "unknown" || m.os === os);
}
