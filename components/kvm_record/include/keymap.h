/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which character a key position types, on the target's keyboard layout.
 * The table is generated from the console's paste tables; see
 * tools/gen_keymap.mjs.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t hid;
    uint8_t mod; /* 0, 0x02 shift, 0x40 AltGr, or both */
    const char *utf8;
} keymap_entry_t;

/* A character typed as a dead key and then another key: ´ then e for é. */
typedef struct {
    uint8_t dead_hid, dead_mod;
    uint8_t hid, mod;
    const char *utf8;
} keymap_pair_t;

typedef struct {
    const char *id; /* the kbd_layout choice, e.g. "ru_ru" */
    const keymap_entry_t *entries;
    size_t count;
    const keymap_entry_t *dead; /* dead keys, utf8 = the accent alone */
    size_t dead_count;
    const keymap_pair_t *pairs;
    size_t pair_count;
} keymap_layout_t;

extern const keymap_layout_t keymap_layouts[];
extern const int keymap_layout_count;
