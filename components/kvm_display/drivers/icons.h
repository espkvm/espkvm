/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
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

/* A heart - device health. */
static const uint8_t icon_health[8] = {0x0C, 0x1E, 0x3E, 0x7C, 0x7C, 0x3E, 0x1E, 0x0C};
