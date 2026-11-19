/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small 8x8 status icons, shared by every driver that wants them. Same layout as
 * the font (column-major, five... no - eight bytes, one per column, bit 0 = top
 * pixel), so a driver blits them exactly like a glyph: the mono OLED 1:1, a
 * colour panel scaled and tinted. Header-only static, one copy per translation
 * unit (a few bytes each).
 */
#pragma once

#include <stdint.h>

/* A monitor on a stand - the video screen. (The old side-nub version read as a
 * battery, so the stand now hangs off the bottom instead.) */
static const uint8_t icon_video[8] = {0x00, 0x1E, 0x52, 0x72, 0x72, 0x52, 0x1E, 0x00};

/* Concentric arcs - network / link. */
static const uint8_t icon_net[8] = {0x08, 0x04, 0x02, 0x5A, 0x5A, 0x02, 0x04, 0x08};

/* A disc with a hole - the virtual media page. */
static const uint8_t icon_media[8] = {0x3C, 0x7E, 0xFF, 0xE7, 0xE7, 0xFF, 0x7E, 0x3C};

/* A heart - device health. */
static const uint8_t icon_health[8] = {0x0C, 0x1E, 0x3E, 0x7C, 0x7C, 0x3E, 0x1E, 0x0C};

/* What the target is, by name: "Linux" for "linux" and so on, NULL for "" or
   anything unknown. The marks are icons16.h, for the colour panels. */
static inline const char *os_name(const char *os)
{
    static const char *const k[][2] = {
        {"windows", "Windows"}, {"macos", "macOS"},   {"linux", "Linux"},
        {"android", "Android"}, {"switch", "Switch"}, {"steamdeck", "Steam Deck"},
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (os && __builtin_strcmp(os, k[i][0]) == 0) {
            return k[i][1];
        }
    }
    return NULL;
}

/* The size of a medium for a panel line: "700 MB", "4.7 GB". */
static inline void media_size_text(char *out, unsigned cap, uint32_t mb)
{
    if (mb >= 1024) {
        __builtin_snprintf(out, cap, "%u.%u GB", (unsigned)(mb / 1024), (unsigned)(mb % 1024 * 10 / 1024));
    } else {
        __builtin_snprintf(out, cap, "%u MB", (unsigned)mb);
    }
}

/* A long file name cut in the middle to @p max characters, so both its start
   and its end (the version, the extension) stay readable. */
static inline void media_name_fit(char *out, unsigned cap, const char *name, unsigned max)
{
    const unsigned len = (unsigned)__builtin_strlen(name);
    if (len <= max || max < 5) {
        __builtin_snprintf(out, cap, "%s", name);
        return;
    }
    const unsigned head = (max - 2) / 2, tail = max - 2 - head;
    __builtin_snprintf(out, cap, "%.*s..%s", (int)head, name, name + len - tail);
}

