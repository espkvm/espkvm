/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "keylog.h"

#include <stdio.h>
#include <string.h>

#include "keymap.h"

/* Presses closer together than this belong to one cue. */
#define WINDOW_US (1500 * 1000LL)
/* How long a cue stays up after its last press. */
#define LINGER_US (1500 * 1000LL)

#define MOD_LCTRL 0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT 0x04
#define MOD_LGUI 0x08
#define MOD_RCTRL 0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT 0x40
#define MOD_RGUI 0x80
#define MOD_CTRL (MOD_LCTRL | MOD_RCTRL)
#define MOD_SHIFT (MOD_LSHIFT | MOD_RSHIFT)
#define MOD_GUI (MOD_LGUI | MOD_RGUI)

/* Symbols a player is likely to have a glyph for: the return and erase signs of
 * the keyboard world (U+23CE, U+232B) are missing from common subtitle fonts and
 * came out as empty boxes on screen. An arrow is in every font there is. */
#define DOT "\xe2\x80\xa2"       /* • */
#define ENTER "\xe2\x86\xb5"     /* ↵ */
#define BACKSPACE "\xe2\x86\x90" /* ← */

static const char *key_name(uint8_t k, char *buf, size_t cap)
{
    static const char *const f_keys[] = {"F1", "F2", "F3", "F4",  "F5",  "F6",
                                         "F7", "F8", "F9", "F10", "F11", "F12"};
    if (k >= 0x04 && k <= 0x1d) {
        snprintf(buf, cap, "%c", 'A' + (k - 0x04));
        return buf;
    }
    if (k >= 0x1e && k <= 0x27) {
        snprintf(buf, cap, "%c", k == 0x27 ? '0' : '1' + (k - 0x1e));
        return buf;
    }
    if (k >= 0x3a && k <= 0x45) {
        return f_keys[k - 0x3a];
    }
    if (k >= 0x68 && k <= 0x73) {
        snprintf(buf, cap, "F%d", 13 + (k - 0x68));
        return buf;
    }
    if (k >= 0x59 && k <= 0x62) {
        snprintf(buf, cap, "Num %c", k == 0x62 ? '0' : '1' + (k - 0x59));
        return buf;
    }
    switch (k) {
    case 0x28: return "Enter";
    case 0x29: return "Esc";
    case 0x2a: return "Backspace";
    case 0x2b: return "Tab";
    case 0x2c: return "Space";
    case 0x2d: return "-";
    case 0x2e: return "=";
    case 0x2f: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "CapsLock";
    case 0x46: return "PrintScreen";
    case 0x47: return "ScrollLock";
    case 0x48: return "Pause";
    case 0x49: return "Insert";
    case 0x4a: return "Home";
    case 0x4b: return "PageUp";
    case 0x4c: return "Delete";
    case 0x4d: return "End";
    case 0x4e: return "PageDown";
    case 0x4f: return "Right";
    case 0x50: return "Left";
    case 0x51: return "Down";
    case 0x52: return "Up";
    case 0x53: return "NumLock";
    case 0x54: return "Num /";
    case 0x55: return "Num *";
    case 0x56: return "Num -";
    case 0x57: return "Num +";
    case 0x58: return "Num Enter";
    case 0x63: return "Num .";
    case 0x65: return "Menu";
    default: break;
    }
    snprintf(buf, cap, "Key 0x%02x", k);
    return buf;
}

/* The keypad types the same on every layout - with NumLock on, which is the
 * only state a KVM can assume. */
static const char *keypad_char(uint8_t k)
{
    static const char *const digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    if (k >= 0x59 && k <= 0x62) {
        return digits[k - 0x59];
    }
    switch (k) {
    case 0x54: return "/";
    case 0x55: return "*";
    case 0x56: return "-";
    case 0x57: return "+";
    case 0x63: return ".";
    default: return NULL;
    }
}

static const char *typed_char(const keylog_t *k, uint8_t key, uint8_t mod)
{
    const char *kp = keypad_char(key);
    if (kp) {
        return kp;
    }
    const uint8_t want = ((mod & MOD_SHIFT) ? MOD_LSHIFT : 0) | (mod & MOD_RALT);
    const keymap_layout_t *l = &keymap_layouts[k->layout];
    for (size_t i = 0; i < l->count; i++) {
        if (l->entries[i].hid == key && l->entries[i].mod == want) {
            return l->entries[i].utf8;
        }
    }
    return NULL;
}

static void flush(keylog_t *k)
{
    if (k->pending == PENDING_NONE) {
        return;
    }
    char line[224];
    if (k->pending == PENDING_TEXT) {
        snprintf(line, sizeof(line), "Typed: %s", k->text);
    } else if (k->count > 1) {
        snprintf(line, sizeof(line), "%s x%d", k->label, k->count);
    } else {
        snprintf(line, sizeof(line), "%s", k->label);
    }
    k->pending = PENDING_NONE;
    if (k->cb) {
        k->cb(k->user, k->start_us, k->last_us + LINGER_US, line);
    }
}

static void add_text(keylog_t *k, int64_t t, const char *s, bool ends_line)
{
    if (k->pending != PENDING_TEXT || t - k->last_us > WINDOW_US ||
        strlen(k->text) + strlen(s) + 1 >= sizeof(k->text)) {
        flush(k);
        k->pending = PENDING_TEXT;
        k->text[0] = '\0';
        k->start_us = t;
    }
    strncat(k->text, s, sizeof(k->text) - strlen(k->text) - 1);
    k->last_us = t;
    if (ends_line) {
        flush(k);
    }
}

static void add_label(keylog_t *k, int64_t t, const char *label)
{
    if (k->pending == PENDING_LABEL && t - k->last_us <= WINDOW_US &&
        strcmp(k->label, label) == 0) {
        k->count++;
        k->last_us = t;
        return;
    }
    flush(k);
    k->pending = PENDING_LABEL;
    snprintf(k->label, sizeof(k->label), "%s", label);
    k->count = 1;
    k->start_us = t;
    k->last_us = t;
}

static void mod_prefix(char *out, size_t cap, uint8_t mod, bool with_shift)
{
    out[0] = '\0';
    if (mod & MOD_CTRL) {
        strncat(out, "Ctrl+", cap - strlen(out) - 1);
    }
    if (mod & MOD_LALT) {
        strncat(out, "Alt+", cap - strlen(out) - 1);
    }
    if (mod & MOD_RALT) {
        strncat(out, "AltGr+", cap - strlen(out) - 1);
    }
    if (mod & MOD_GUI) {
        strncat(out, "Win+", cap - strlen(out) - 1);
    }
    if (with_shift && (mod & MOD_SHIFT)) {
        strncat(out, "Shift+", cap - strlen(out) - 1);
    }
}

void keylog_init(keylog_t *k, keylog_mode_t mode, bool clicks, const char *layout_id,
                 keylog_cue_cb_t cb, void *user)
{
    memset(k, 0, sizeof(*k));
    k->mode = mode;
    k->clicks = clicks;
    k->cb = cb;
    k->user = user;
    for (int i = 0; i < keymap_layout_count; i++) {
        if (layout_id && strcmp(keymap_layouts[i].id, layout_id) == 0) {
            k->layout = i;
        }
    }
}

static bool was_down(const keylog_t *k, uint8_t key)
{
    for (int i = 0; i < 6; i++) {
        if (k->prev_keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void press(keylog_t *k, int64_t t, uint8_t key, uint8_t mod)
{
    const bool command = mod & (MOD_CTRL | MOD_LALT | MOD_GUI);
    if (!command) {
        if (key == 0x28 || key == 0x58) {
            if (k->pending == PENDING_TEXT && t - k->last_us <= WINDOW_US) {
                add_text(k, t, ENTER, true);
            } else {
                add_label(k, t, "Enter");
            }
            return;
        }
        if (key == 0x2a && k->pending == PENDING_TEXT && t - k->last_us <= WINDOW_US) {
            add_text(k, t, BACKSPACE, false);
            return;
        }
        const char *ch = typed_char(k, key, mod);
        if (ch) {
            add_text(k, t, k->mode == KEYLOG_FULL ? ch : DOT, false);
            return;
        }
    }
    char prefix[40], buf[16], label[64];
    const bool shift_shown = command || !typed_char(k, key, 0);
    mod_prefix(prefix, sizeof(prefix), mod, shift_shown);
    snprintf(label, sizeof(label), "%s%s", prefix, key_name(key, buf, sizeof(buf)));
    add_label(k, t, label);
}

void keylog_keyboard(keylog_t *k, int64_t t_us, uint8_t modifier, const uint8_t keys[6])
{
    if (k->mode == KEYLOG_OFF || !keys) {
        return;
    }
    bool any_key = false;
    for (int i = 0; i < 6; i++) {
        const uint8_t key = keys[i];
        if (key < 0x04) {
            continue; /* empty slot, or the keyboard's error codes */
        }
        any_key = true;
        if (!was_down(k, key)) {
            press(k, t_us, key, modifier);
            k->key_since_mods = true;
        }
    }
    /* A modifier pressed and let go on its own - Win opening the start menu,
     * Alt reaching a menu bar. Shift alone means nothing on screen. */
    k->mods_alone |= modifier & ~k->prev_mod;
    if (!modifier && !any_key) {
        const uint8_t alone = k->mods_alone & (MOD_CTRL | MOD_LALT | MOD_RALT | MOD_GUI);
        if (alone && !k->key_since_mods) {
            char name[40];
            mod_prefix(name, sizeof(name), alone, false);
            name[strlen(name) - 1] = '\0'; /* the trailing "+" */
            add_label(k, t_us, name);
        }
        k->mods_alone = 0;
        k->key_since_mods = false;
    }
    k->prev_mod = modifier;
    memcpy(k->prev_keys, keys, 6);
}

void keylog_mouse(keylog_t *k, int64_t t_us, uint8_t buttons, int x, int y)
{
    if (k->mode == KEYLOG_OFF) {
        return;
    }
    const uint8_t down = buttons & ~k->prev_buttons;
    k->prev_buttons = buttons;
    if (!k->clicks || !down) {
        return;
    }
    static const char *const names[] = {"Left", "Right", "Middle", "Back", "Forward"};
    for (int b = 0; b < 5; b++) {
        if (!(down & (1u << b))) {
            continue;
        }
        char label[64];
        if (x >= 0 && y >= 0) {
            snprintf(label, sizeof(label), "%s click (%d, %d)", names[b], x, y);
        } else {
            snprintf(label, sizeof(label), "%s click", names[b]);
        }
        add_label(k, t_us, label);
    }
}

void keylog_consumer(keylog_t *k, int64_t t_us, uint16_t usage)
{
    if (k->mode == KEYLOG_OFF) {
        return;
    }
    const uint16_t prev = k->prev_consumer;
    k->prev_consumer = usage;
    if (!usage || usage == prev) {
        return;
    }
    const char *name = NULL;
    switch (usage) {
    case 0xe9: name = "Volume up"; break;
    case 0xea: name = "Volume down"; break;
    case 0xe2: name = "Mute"; break;
    case 0xcd: name = "Play/Pause"; break;
    case 0xb5: name = "Next track"; break;
    case 0xb6: name = "Previous track"; break;
    case 0xb7: name = "Stop"; break;
    default: break;
    }
    char label[32];
    if (!name) {
        snprintf(label, sizeof(label), "Media key 0x%03x", usage);
        name = label;
    }
    add_label(k, t_us, name);
}

void keylog_tick(keylog_t *k, int64_t now_us)
{
    if (k->pending != PENDING_NONE && now_us - k->last_us > WINDOW_US) {
        flush(k);
    }
}

void keylog_flush(keylog_t *k)
{
    flush(k);
}
