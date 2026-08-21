/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fonts the scanner knows, and how a cell height picks one.
 *
 * One table per cell height, because the height is the only thing about a cell
 * that can be measured before anything is recognised:
 *
 *   16  a legacy BIOS text mode, and a Linux console
 *   19  a UEFI firmware console - a boot menu, a setup screen
 *
 * A table can hold more than one font. Nothing looks up "which font is this",
 * only "what does this bitmap mean", so fonts of the same height are merged and
 * any bitmap they disagree about is left out - see tools/mkfont.py. That is what
 * makes a Linux console readable without a second table and without a guess:
 * its font differs from the IBM ROM one in 28 glyphs, five of them printable.
 *
 * A firmware that ships a font we have neither is not read at all, which is the
 * designed answer: see the component header on refusing rather than guessing.
 */
#pragma once

#include "screentext_font_h16.h"
#include "screentext_font_h19.h"

#include <stddef.h>

/** The tallest cell any table holds; sizes the per-cell bitmap buffers. */
#define SCREENTEXT_CELL_H_MAX SCREENTEXT_FONT_H19_HEIGHT

/** Every cell height there is a font for, shortest first. */
#define SCREENTEXT_CELL_HEIGHTS \
    { SCREENTEXT_FONT_H16_HEIGHT, SCREENTEXT_FONT_H19_HEIGHT }

/**
 * The table for a cell height, or NULL if no font is that tall.
 * @param[out] count how many entries it has
 */
static inline const screentext_glyph_t *screentext_font_for(uint8_t cell_h, size_t *count)
{
    if (cell_h == SCREENTEXT_FONT_H16_HEIGHT) {
        *count = SCREENTEXT_FONT_H16_GLYPHS;
        return screentext_font_h16;
    }
    if (cell_h == SCREENTEXT_FONT_H19_HEIGHT) {
        *count = SCREENTEXT_FONT_H19_GLYPHS;
        return screentext_font_h19;
    }
    *count = 0;
    return NULL;
}
