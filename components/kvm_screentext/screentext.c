/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The scanner. Pure C over a pixel buffer - no ESP-IDF, no allocation, no
 * globals - so it can be built and tested on a host as easily as on the device.
 */
#include "screentext.h"
#include "screentext_font.h"

#include <string.h>

/*
 * A cell is always 8 pixels wide for hashing purposes. A 720-wide mode spaces
 * them 9 apart, but the 9th column is the character generator's copy of the 8th
 * and carries nothing, so it is stepped over rather than read.
 */
#define CELL_W 8

/** The cell heights there are fonts for, shortest first. */
static const uint8_t k_cell_heights[] = SCREENTEXT_CELL_HEIGHTS;
#define CELL_HEIGHTS_N (sizeof(k_cell_heights) / sizeof(k_cell_heights[0]))

/** Fewest columns that can be a text screen; 80 is what every one of them is. */
#define MIN_COLS 80
/** Fewest rows, so a band of noise across the top of a picture is not a screen. */
#define MIN_ROWS 20

/* A cell whose brightest and darkest pixel are this close carries no ink: it is
 * a blank, and thresholding it would turn sensor noise into a random glyph. */
#define INK_CONTRAST 40

/* Below this many cells with ink there is nothing worth calling a screen of
 * text - a mostly black picture would otherwise score 100% on three cells. */
#define MIN_INK_CELLS 20

/* The sample taken before committing to the whole screen, and the share of it
 * that has to be readable to be worth continuing. */
#define PROBE_CELLS 48
#define PROBE_MIN_PERCENT 50

/* What the whole screen must reach to be handed over as text. */
#define ACCEPT_MIN_PERCENT 90

static uint32_t fnv1a(const uint8_t *data, size_t len)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ data[i]) * 0x01000193u;
    }
    return h;
}

/** The table is sorted by hash, so this is a binary search. 0 = no such glyph. */
static uint16_t glyph_lookup(const screentext_glyph_t *font, size_t count, uint32_t hash)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (font[mid].hash == hash) {
            return font[mid].cp;
        }
        if (font[mid].hash < hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static uint32_t frame_stride(const screentext_frame_t *f)
{
    if (f->stride) {
        return f->stride;
    }
    return f->width * (f->fmt == SCREENTEXT_FMT_RGB888 ? 3 : 2);
}

/** Luma of one pixel. UYVY carries it directly; BGR888 needs the usual mix. */
static inline uint8_t luma_at(const screentext_frame_t *f, const uint8_t *row, uint32_t x)
{
    if (f->fmt == SCREENTEXT_FMT_UYVY) {
        return row[x * 2 + 1];
    }
    /* Weighted towards the middle byte, which is green either way round: the
       capture hardware's three-byte order is not worth depending on for a
       threshold, and a symmetric mix gives the same answer for RGB and BGR. */
    const uint8_t a = row[x * 3], g = row[x * 3 + 1], b = row[x * 3 + 2];
    return (uint8_t)((a + 2 * g + b) >> 2);
}

/**
 * Read one cell's 8 by @p cell_h pixels, threshold them against that cell's own
 * range and return the bitmap, or report the cell as blank.
 *
 * The threshold is per cell on purpose. A text screen picks its two colours per
 * character - grey on blue here, black on grey in the highlighted menu row there
 * - and one threshold for the whole frame gets the second case wrong.
 */
static bool cell_bitmap(const screentext_frame_t *f, uint32_t px, uint32_t py, uint8_t cell_h,
                        uint8_t out[SCREENTEXT_CELL_H_MAX])
{
    const uint32_t stride = frame_stride(f);
    uint8_t pix[SCREENTEXT_CELL_H_MAX][CELL_W];
    uint8_t lo = 255, hi = 0;

    for (uint32_t y = 0; y < cell_h; y++) {
        const uint8_t *row = f->pixels + (size_t)(py + y) * stride;
        for (uint32_t x = 0; x < CELL_W; x++) {
            const uint8_t v = luma_at(f, row, px + x);
            pix[y][x] = v;
            if (v < lo) {
                lo = v;
            }
            if (v > hi) {
                hi = v;
            }
        }
    }

    if (hi - lo < INK_CONTRAST) {
        return false; /* one flat colour: a blank cell */
    }

    const uint8_t mid = (uint8_t)((lo + hi) / 2);
    for (uint32_t y = 0; y < cell_h; y++) {
        uint8_t bits = 0;
        for (uint32_t x = 0; x < CELL_W; x++) {
            if (pix[y][x] > mid) {
                bits |= (uint8_t)(0x80u >> x);
            }
        }
        out[y] = bits;
    }
    return true;
}

/**
 * One way a frame might be cut into cells, and the font that would fit it.
 *
 * There can be more than one: 640x480 is 30 rows of 16 to a BIOS and 25 rows of
 * 19 to a UEFI console, and nothing but the glyphs themselves can say which it
 * is. So a layout is a proposal, and the probe in screentext_scan() decides.
 */
typedef struct {
    uint8_t cell_w; /**< 8, or 9 at 720 wide */
    uint8_t cell_h;
    uint16_t cols;
    uint16_t rows;
    uint16_t x0; /**< a text area that does not fill the frame is centred in it */
    uint16_t y0;
    const screentext_glyph_t *font;
    size_t font_len;
} layout_t;

/**
 * The character in a cell, or 0.
 *
 * Tried both ways round: a selected menu row is drawn dark-on-light, which
 * thresholds to the exact inverse of the glyph in the font.
 */
static uint16_t cell_char(const screentext_frame_t *f, const layout_t *lay, uint16_t col,
                          uint16_t row, bool *had_ink)
{
    const uint32_t px = (uint32_t)lay->x0 + (uint32_t)col * lay->cell_w;
    const uint32_t py = (uint32_t)lay->y0 + (uint32_t)row * lay->cell_h;

    uint8_t bits[SCREENTEXT_CELL_H_MAX];
    if (!cell_bitmap(f, px, py, lay->cell_h, bits)) {
        *had_ink = false;
        return ' ';
    }
    *had_ink = true;

    uint16_t cp = glyph_lookup(lay->font, lay->font_len, fnv1a(bits, lay->cell_h));
    if (cp) {
        return cp;
    }
    for (uint32_t i = 0; i < lay->cell_h; i++) {
        bits[i] = (uint8_t)~bits[i];
    }
    return glyph_lookup(lay->font, lay->font_len, fnv1a(bits, lay->cell_h));
}

/**
 * Every layout a mode could hold, most likely first.
 *
 * The column count is what rules a mode out cheaply: a text screen is 80 columns
 * or, on a UEFI console at 1024 wide, 128. Anything wider than the grid we keep
 * is refused here, before a pixel is read.
 *
 * @return how many were written, at most CELL_HEIGHTS_N
 */
static uint8_t layouts_for(uint32_t width, uint32_t height, layout_t out[CELL_HEIGHTS_N],
                           uint16_t max_cols)
{
    /* 720 wide is 80 columns of 9; everything else is columns of 8. */
    const uint8_t cell_w = (width == 720) ? 9 : 8;
    if (width % cell_w != 0) {
        return 0;
    }
    const uint32_t cols = width / cell_w;
    if (cols < MIN_COLS || cols > max_cols) {
        return 0;
    }

    uint8_t n = 0;
    for (size_t i = 0; i < CELL_HEIGHTS_N; i++) {
        const uint8_t cell_h = k_cell_heights[i];
        const uint32_t rows = height / cell_h;
        if (rows < MIN_ROWS || rows > SCREENTEXT_MAX_ROWS) {
            continue;
        }
        size_t font_len = 0;
        const screentext_glyph_t *font = screentext_font_for(cell_h, &font_len);
        if (!font) {
            continue;
        }
        out[n].cell_w = cell_w;
        out[n].cell_h = cell_h;
        out[n].cols = (uint16_t)cols;
        out[n].rows = (uint16_t)rows;
        out[n].x0 = (uint16_t)((width - cols * cell_w) / 2);
        out[n].y0 = (uint16_t)((height - rows * cell_h) / 2);
        out[n].font = font;
        out[n].font_len = font_len;
        n++;
    }
    return n;
}

bool screentext_mode_supported(uint32_t width, uint32_t height)
{
    layout_t discard[CELL_HEIGHTS_N];
    return layouts_for(width, height, discard, SCREENTEXT_MAX_COLS) > 0;
}

bool screentext_mode_unprompted(uint32_t width, uint32_t height)
{
    layout_t discard[CELL_HEIGHTS_N];
    return layouts_for(width, height, discard, SCREENTEXT_UNPROMPTED_MAX_COLS) > 0;
}

/**
 * The sample taken before committing to a layout: cells spread over the whole
 * screen with a stride that shares no factor with the row length, so it cannot
 * land in one column of blanks.
 *
 * A screen with no ink in the sample passes - it is blank, not wrong - and the
 * ink count on the full pass is what turns that down.
 */
static bool layout_probes_ok(const screentext_frame_t *frame, const layout_t *lay)
{
    const uint32_t cells = (uint32_t)lay->cols * lay->rows;
    uint32_t ink = 0, hit = 0;
    for (uint32_t i = 0; i < PROBE_CELLS; i++) {
        const uint32_t cell = (i * 337u) % cells;
        bool had_ink = false;
        const uint16_t cp = cell_char(frame, lay, (uint16_t)(cell % lay->cols),
                                      (uint16_t)(cell / lay->cols), &had_ink);
        if (had_ink) {
            ink++;
            if (cp) {
                hit++;
            }
        }
    }
    return ink == 0 || hit * 100 >= ink * PROBE_MIN_PERCENT;
}

/** Read the whole screen with one layout. False if it does not hold up. */
static bool scan_with(const screentext_frame_t *frame, const layout_t *lay,
                      screentext_grid_t *out)
{
    uint32_t ink = 0, hit = 0;
    for (uint16_t r = 0; r < lay->rows; r++) {
        for (uint16_t c = 0; c < lay->cols; c++) {
            bool cell_ink = false;
            const uint16_t cp = cell_char(frame, lay, c, r, &cell_ink);
            if (cell_ink) {
                ink++;
                if (cp) {
                    hit++;
                }
            }
            out->cells[(size_t)r * lay->cols + c] = cp ? cp : SCREENTEXT_UNREADABLE;
        }
    }

    if (ink < MIN_INK_CELLS) {
        return false;
    }
    const uint8_t confidence = (uint8_t)(hit * 100 / ink);
    if (confidence < ACCEPT_MIN_PERCENT) {
        return false;
    }

    out->cols = lay->cols;
    out->rows = lay->rows;
    out->cell_w = lay->cell_w;
    out->cell_h = lay->cell_h;
    out->x0 = lay->x0;
    out->y0 = lay->y0;
    out->width = (uint16_t)frame->width;
    out->height = (uint16_t)frame->height;
    out->confidence = confidence;
    return true;
}

bool screentext_scan(const screentext_frame_t *frame, screentext_grid_t *out)
{
    if (!frame || !frame->pixels || !out) {
        return false;
    }
    /* The scanner reads whatever fits; whether a mode is worth reading unasked
       is the caller's call (screentext_mode_unprompted). */
    layout_t layouts[CELL_HEIGHTS_N];
    const uint8_t n = layouts_for(frame->width, frame->height, layouts, SCREENTEXT_MAX_COLS);

    /*
     * Which cell height it is cannot be told from the resolution alone - 1024x768
     * is 48 rows of 16 to a Linux console and 40 rows of 19 to a UEFI one - so
     * let the glyphs answer. The sample rules out most layouts for the price of a
     * few dozen cells; a layout that gets past it and then fails on the whole
     * screen is not the end of it either, because the next one may still be right.
     */
    for (uint8_t i = 0; i < n; i++) {
        if (layout_probes_ok(frame, &layouts[i]) && scan_with(frame, &layouts[i], out)) {
            return true;
        }
    }
    return false;
}

/** One code point as UTF-8. Everything in code page 437 fits in three bytes. */
static size_t utf8_put(uint16_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

size_t screentext_to_utf8(const screentext_grid_t *grid, char *buf, size_t cap)
{
    if (!grid || !buf || cap == 0) {
        return 0;
    }
    size_t n = 0;
    for (uint16_t r = 0; r < grid->rows; r++) {
        const uint16_t *row = &grid->cells[(size_t)r * grid->cols];
        uint16_t end = grid->cols;
        while (end > 0 && row[end - 1] == ' ') {
            end--; /* trailing blanks are padding, not content */
        }
        for (uint16_t c = 0; c < end; c++) {
            if (n + 4 > cap) {
                buf[n] = '\0';
                return n;
            }
            n += utf8_put(row[c], buf + n);
        }
        if (n + 2 > cap) {
            break;
        }
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    return n;
}
