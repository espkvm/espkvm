/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reading the characters off a text-mode screen - a BIOS setup, a boot loader,
 * a Linux console, a panic.
 *
 * This is deliberately not OCR. A text-mode screen is drawn by a character
 * generator: a fixed grid, a fixed bitmap per character, no antialiasing. At the
 * source's own resolution every letter is the same handful of bytes every time,
 * so reading the screen is cutting it into cells and looking each one up in a
 * table of bitmaps. That has two consequences worth stating plainly:
 *
 *   - It cannot be a little bit wrong. Either a cell's bitmap is in the table
 *     and the character is certain, or it is not and the cell says so - it comes
 *     back as SCREENTEXT_UNREADABLE, and enough of those turn the whole screen
 *     down. There is no "recognised with errors", which is the failure mode that
 *     makes real OCR a poor fit for a screen someone is about to act on.
 *   - It costs almost nothing. One pass over a 720x400 frame, and only when the
 *     picture has settled, which in a BIOS is a few times a minute.
 *
 * It also means the feature has a hard edge: a graphical UEFI setup, with a
 * proportional antialiased font, has no grid and will not be read at all. That
 * is the honest boundary, not a bug to fix here.
 *
 * The one thing that has to match is the font, and there are two, because the
 * two firmwares that draw text screens do not share one: a legacy BIOS draws
 * with the IBM VGA 8x16 ROM font, a UEFI console with the 8x19 narrow font. The
 * cell height says which, so the height picks the table (screentext_font.h). A
 * machine whose firmware ships its own font will simply score too low to be
 * read, rather than produce wrong text.
 *
 * A UEFI console also centres its text area rather than filling the frame -
 * 1024x768 holds 40 rows of 19 and leaves 4 pixels above and below - so the grid
 * carries the origin it was read at, not just its size.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Widest and tallest grid that can be read at all: 240 columns by 68 rows, which
 * is 1920 wide in cells of 8, and 1080 tall in cells of 16. A UEFI console fills
 * a 1080p screen with 240x56 of 8x19 and fits inside it.
 *
 * This is a size limit, not a policy: see SCREENTEXT_UNPROMPTED_MAX_COLS for the
 * narrower grid the device reads on its own.
 */
#define SCREENTEXT_MAX_COLS 240
#define SCREENTEXT_MAX_ROWS 68
#define SCREENTEXT_MAX_CELLS (SCREENTEXT_MAX_COLS * SCREENTEXT_MAX_ROWS)

/*
 * The widest grid worth reading when nobody asked.
 *
 * Reading costs one pass over the frame, and the device does it by itself on
 * every picture that has settled - so the mode it does that for has to be one
 * where text is likely. 128 columns covers a console at 1024x768 and everything
 * below; 1920 wide is 240 columns, four times the pixels of 1024x768, and is
 * overwhelmingly a desktop rather than a boot menu.
 *
 * An operator who presses Select or Copy has said the opposite - that this
 * particular screen IS text - so an asked-for reading is allowed the full
 * SCREENTEXT_MAX_COLS. A UEFI console at 1080p is real and reads perfectly; it
 * just is not worth hunting for on the off chance.
 */
#define SCREENTEXT_UNPROMPTED_MAX_COLS 128

/*
 * What a cell reads as when it has ink but no glyph in the table matches it.
 *
 * It has to be visible. A miss used to come back as a space, and a screen drawn
 * in a font we half know - the Linux console one differs from the IBM ROM font
 * in five printable characters - would then pass the confidence test while
 * quietly dropping every f and v: "de ault" for "default". Silently plausible is
 * the one thing this must never be, so an unread cell says so.
 */
#define SCREENTEXT_UNREADABLE 0xFFFDu /* U+FFFD REPLACEMENT CHARACTER */

/** How the caller's frame stores a pixel. */
typedef enum {
    SCREENTEXT_FMT_UYVY,  /**< packed 4:2:2, luma in the odd bytes */
    SCREENTEXT_FMT_RGB888, /**< three bytes per pixel; the order does not matter
                            *   here, brightness is taken symmetrically */
} screentext_fmt_t;

/** A captured frame to read, as it sits in the capture buffer. */
typedef struct {
    const uint8_t *pixels;
    screentext_fmt_t fmt;
    uint32_t width;
    uint32_t height;
    uint32_t stride; /**< bytes per row; 0 means width * bytes-per-pixel */
} screentext_frame_t;

/** One reading of the screen. */
typedef struct {
    uint16_t cols;
    uint16_t rows;
    uint8_t cell_w; /**< 8 or 9 - the 9th column is the hardware's copy of the 8th */
    uint8_t cell_h; /**< 16 (VGA) or 19 (UEFI) */
    uint16_t x0; /**< where the text area starts in the frame; a console that */
    uint16_t y0; /**< centres its text leaves a margin, and an overlay needs it */
    uint16_t width;  /**< the frame this was read from, so a caller can scale */
    uint16_t height; /**< the grid onto a picture of any displayed size */
    uint8_t confidence; /**< percent of the cells carrying ink that matched a glyph */
    uint16_t cells[SCREENTEXT_MAX_CELLS]; /**< Unicode code points, row-major */
} screentext_grid_t;

/**
 * Whether a mode can hold a character grid at all, so a caller can skip the
 * scan without touching the frame: cells of 8 pixels (9 at 720 wide) by 16 or
 * 19, and a grid no larger than SCREENTEXT_MAX_COLS by SCREENTEXT_MAX_ROWS.
 * Yes here means "worth looking at", not "is text" - only the scan can say that.
 */
bool screentext_mode_supported(uint32_t width, uint32_t height);

/**
 * Whether a mode is worth reading with nobody asking for it.
 *
 * The same test, narrowed to SCREENTEXT_UNPROMPTED_MAX_COLS. Everything this
 * says yes to, screentext_mode_supported() says yes to as well.
 */
bool screentext_mode_unprompted(uint32_t width, uint32_t height);

/**
 * Read @p frame into @p out.
 *
 * Returns false when the picture is not a character grid - a desktop, a
 * graphical setup, a photograph - which is the common case and costs only the
 * sample of cells it takes to find out.
 */
bool screentext_scan(const screentext_frame_t *frame, screentext_grid_t *out);

/**
 * The grid as UTF-8, rows joined by newlines, trailing blanks on each row
 * removed. Returns the length written, excluding the terminator.
 */
size_t screentext_to_utf8(const screentext_grid_t *grid, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
