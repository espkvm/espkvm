/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * What was pressed, as lines a person can read under a recording.
 *
 * Fed the same keyboard, mouse and media-key reports that go to the target, it
 * turns them into cues: a run of typed characters becomes one line ("Typed:
 * root"), a shortcut becomes its name ("Ctrl+Alt+Del"), a repeated key is
 * counted ("Down x5"), a click says which button and where. Plain C over a
 * callback, so it is tested on a host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEYLOG_OFF = 0,
    /** Shortcuts, named keys and clicks; typed characters show as dots. */
    KEYLOG_MASKED = 1,
    /** Everything, including what was typed - passwords too. */
    KEYLOG_FULL = 2,
} keylog_mode_t;

/** One finished cue, times in the caller's clock (microseconds). */
typedef void (*keylog_cue_cb_t)(void *user, int64_t start_us, int64_t end_us, const char *text);

typedef struct {
    keylog_mode_t mode;
    bool clicks;
    int layout; /* index into keymap_layouts */
    keylog_cue_cb_t cb;
    void *user;

    uint8_t prev_mod;
    uint8_t prev_keys[6];
    uint8_t mods_alone; /* modifiers pressed since the last key-free moment */
    bool key_since_mods;
    uint8_t prev_buttons;
    uint16_t prev_consumer;

    /* The cue being built. */
    enum { PENDING_NONE, PENDING_TEXT, PENDING_LABEL } pending;
    char text[192];
    char label[64];
    int count;
    int64_t start_us;
    int64_t last_us;
} keylog_t;

/** @p layout_id is the kbd_layout choice ("en_us", "ru_ru", ...); unknown means en_us. */
void keylog_init(keylog_t *k, keylog_mode_t mode, bool clicks, const char *layout_id,
                 keylog_cue_cb_t cb, void *user);

void keylog_keyboard(keylog_t *k, int64_t t_us, uint8_t modifier, const uint8_t keys[6]);

/** @p x, @p y in screen pixels, or negative when the pointer is relative. */
void keylog_mouse(keylog_t *k, int64_t t_us, uint8_t buttons, int x, int y);

void keylog_consumer(keylog_t *k, int64_t t_us, uint16_t usage);

/** Close a cue that nothing has added to for a while. Call now and then. */
void keylog_tick(keylog_t *k, int64_t now_us);

/** Close whatever is open, e.g. when the recording ends. */
void keylog_flush(keylog_t *k);

#ifdef __cplusplus
}
#endif
