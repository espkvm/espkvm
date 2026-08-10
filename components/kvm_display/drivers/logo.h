/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ESP-KVM wordmark, lifted pixel-for-pixel from web/public/icon.svg: the
 * light "ESP" (top) over "KVM" (bottom), each glyph a 5x5 pixel block. One byte
 * per row, bit 4 (0x10) = leftmost column. Laid out on a 17x11 grid: three 5-wide
 * glyphs with a 1-column gap, two rows with a 1-row gap.
 *
 * A driver draws it by testing logo_px(): the mono OLED plots lit pixels, a
 * colour panel fills the tile dark and the pixels light, matching the icon.
 */
#pragma once

#include <stdint.h>

static const uint8_t logo_E[5] = {0x1F, 0x10, 0x1E, 0x10, 0x1F};
static const uint8_t logo_S[5] = {0x1F, 0x10, 0x1F, 0x01, 0x1F};
static const uint8_t logo_P[5] = {0x1F, 0x11, 0x1F, 0x10, 0x10};
static const uint8_t logo_K[5] = {0x11, 0x12, 0x1C, 0x12, 0x11};
static const uint8_t logo_V[5] = {0x11, 0x11, 0x11, 0x0A, 0x04};
static const uint8_t logo_M[5] = {0x11, 0x1B, 0x15, 0x11, 0x11};

static const uint8_t *const logo_grid[2][3] = {
    {logo_E, logo_S, logo_P},
    {logo_K, logo_V, logo_M},
};

#define LOGO_COLS 17 /* 5 + gap + 5 + gap + 5 */
#define LOGO_ROWS 11 /* 5 + gap + 5 */

/* Is the logo pixel at grid (col, row) lit? */
static inline int logo_px(int col, int row)
{
    if (col < 0 || col >= LOGO_COLS || row < 0 || row >= LOGO_ROWS) {
        return 0;
    }
    const int li = col / 6, lc = col % 6; /* which glyph, column within it */
    const int lr = row / 6, rr = row % 6; /* which glyph row, row within it */
    if (lc > 4 || rr > 4) {
        return 0; /* in a gap column/row */
    }
    return (logo_grid[lr][li][rr] >> (4 - lc)) & 1;
}
