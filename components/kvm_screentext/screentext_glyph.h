/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One entry of a font lookup table, shared by every generated table.
 *
 * The bitmaps never reach the firmware: a table is hashes and code points, so
 * it costs the same few kilobytes whatever the glyphs look like or how tall
 * they are.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t hash; /**< FNV-1a over the glyph's rows, one byte per row */
    uint16_t cp;   /**< the character it draws, as a Unicode code point */
} screentext_glyph_t;
