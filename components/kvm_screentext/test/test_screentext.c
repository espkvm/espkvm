/* Host test: render a text screen with the same font a firmware would, read it
 * back. Both fonts, both cell heights, and a text area that does not fill the
 * frame - which is what a UEFI console gives us. */
#include "screentext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_H_MAX 19

/* A font as the test needs it: rows indexed by code point, for code points that
 * fit in a byte. Enough for ASCII, and for the code page 437 box drawing the VGA
 * font carries - a UEFI console has neither box drawing nor a code page. */
typedef struct {
    uint8_t height;
    uint8_t rows[256][GLYPH_H_MAX];
} font_t;

static font_t vga, pcdos, uefi;

/** The flat 256-glyph ROM dump, indexed by its code page 437 byte. */
static int load_rom(const char *path, uint8_t height, font_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    out->height = height;
    for (int i = 0; i < 256; i++) {
        if (fread(out->rows[i], 1, height, f) != height) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return 1;
}

/** The `XXXX: hh hh ...` list; entries above U+00FF are not needed here. */
static int load_list(const char *path, uint8_t height, font_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    out->height = height;
    char line[512];
    int seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        unsigned cp = 0;
        int off = 0;
        if (sscanf(line, "%4x:%n", &cp, &off) != 1) {
            continue;
        }
        if (cp > 0xFF) {
            continue;
        }
        const char *p = line + off;
        for (int y = 0; y < height; y++) {
            unsigned b = 0;
            int used = 0;
            if (sscanf(p, "%2x%n", &b, &used) != 1) {
                fclose(f);
                return 0;
            }
            out->rows[cp][y] = (uint8_t)b;
            p += used;
        }
        seen++;
    }
    fclose(f);
    return seen > 0;
}

/* One VGA colour pair, as luma values. */
typedef struct { uint8_t fg, bg; } pair_t;

static void put_px(uint8_t *buf, screentext_fmt_t fmt, uint32_t stride,
                   uint32_t x, uint32_t y, uint8_t v)
{
    if (fmt == SCREENTEXT_FMT_UYVY) {
        uint8_t *p = buf + (size_t)y * stride + x * 2;
        p[0] = 128;  /* U/V: grey, we only read luma */
        p[1] = v;
    } else {
        uint8_t *p = buf + (size_t)y * stride + x * 3;
        p[0] = p[1] = p[2] = v;
    }
}

/*
 * Render `text` (one byte per cell, rows of `cols`) into a frame.
 *
 * `pad_top` and `pad_bot` are the margins a console leaves around its text area.
 * They are given separately because they are not always equal: 25 rows of 19 in
 * a 480-tall frame leave 5 pixels over, and a console cannot split those evenly.
 */
static uint8_t *render(const uint8_t *text, uint32_t cols, uint32_t rows,
                       uint32_t cell_w, screentext_fmt_t fmt, const pair_t *rowpair,
                       const font_t *font, uint32_t pad_top, uint32_t pad_bot,
                       uint32_t *w, uint32_t *h, uint32_t *stride_out)
{
    *w = cols * cell_w;
    *h = rows * font->height + pad_top + pad_bot;
    const uint32_t stride = *w * (fmt == SCREENTEXT_FMT_RGB888 ? 3 : 2);
    uint8_t *buf = calloc(1, (size_t)stride * *h);
    *stride_out = stride;

    /* The margin is background, not black: a console paints the whole screen. */
    for (uint32_t y = 0; y < *h; y++) {
        for (uint32_t x = 0; x < *w; x++) {
            put_px(buf, fmt, stride, x, y, rowpair[0].bg);
        }
    }

    for (uint32_t r = 0; r < rows; r++) {
        const pair_t p = rowpair[r];
        for (uint32_t c = 0; c < cols; c++) {
            const uint8_t ch = text[r * cols + c];
            for (uint32_t y = 0; y < font->height; y++) {
                const uint8_t bits = font->rows[ch][y];
                for (uint32_t x = 0; x < cell_w; x++) {
                    const int on = (x < 8) && (bits & (0x80 >> x));
                    put_px(buf, fmt, stride, c * cell_w + x,
                           pad_top + r * font->height + y, on ? p.fg : p.bg);
                }
            }
        }
    }
    return buf;
}

/** How many cells on a row came back marked as drawn the other way round. */
static uint32_t marks_in_row(const screentext_grid_t *g, uint16_t row)
{
    uint32_t n = 0;
    for (uint16_t c = 0; c < g->cols; c++) {
        if (screentext_marked(g, (size_t)row * g->cols + c)) {
            n++;
        }
    }
    return n;
}

static int failures;
static void check(int ok, const char *what)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(int argc, char **argv)
{
    const char *vga_path = argc > 1 ? argv[1] : "fonts/ibm_vga_8x16.bin";
    const char *dos_path = argc > 2 ? argv[2] : "fonts/pcdos_cp437_8x16.bin";
    const char *uefi_path = argc > 3 ? argv[3] : "fonts/uefi_hii_8x19.txt";
    if (!load_rom(vga_path, 16, &vga) || !load_rom(dos_path, 16, &pcdos) ||
        !load_list(uefi_path, 19, &uefi)) {
        fprintf(stderr, "cannot read the fonts\n");
        return 2;
    }

    static screentext_grid_t grid;
    static char out[32768];

    /* --- a plausible BIOS screen: 80x25, 720x400, one highlighted row --- */
    const uint32_t cols = 80, rows = 25;
    uint8_t *text = malloc(cols * rows);
    memset(text, ' ', cols * rows);
    const char *l0 = "Aptio Setup Utility - Copyright (C) 2026 American Megatrends, Inc.";
    const char *l1 = "  Boot Option #1     [UEFI: Samsung SSD 990 PRO 2TB]";
    const char *l2 = "  Serial Number: 5CG9382KJ7   IP: 192.168.1.14   MAC: e8:f6:0a:e0:cb:87";
    const char *l3 = "  Symbols: !@#$%^&*()_+-=[]{}|;':\",./<>?`~";
    memcpy(text + 0 * cols, l0, strlen(l0));
    memcpy(text + 5 * cols, l1, strlen(l1));
    memcpy(text + 7 * cols, l2, strlen(l2));
    memcpy(text + 9 * cols, l3, strlen(l3));
    /* a box-drawing run, which is what a BIOS frames its panes with */
    for (uint32_t c = 2; c < 60; c++) text[12 * cols + c] = 0xC4; /* ─ */
    text[12 * cols + 1] = 0xDA; /* ┌ */

    pair_t pairs[SCREENTEXT_MAX_ROWS];
    for (uint32_t r = 0; r < SCREENTEXT_MAX_ROWS; r++) pairs[r] = (pair_t){200, 30};
    pairs[5] = (pair_t){20, 220};  /* the selected row: dark on light */

    uint32_t w, h, stride;
    uint8_t *buf = render(text, cols, rows, 9, SCREENTEXT_FMT_UYVY, pairs, &vga, 0, 0,
                          &w, &h, &stride);
    screentext_frame_t frame = {buf, SCREENTEXT_FMT_UYVY, w, h, stride};

    check(screentext_mode_supported(w, h), "720x400 is a supported mode");
    const bool got = screentext_scan(&frame, &grid);
    check(got, "scan reads the screen");
    if (got) {
        check(grid.cols == 80 && grid.rows == 25, "grid is 80x25");
        check(grid.cell_w == 9, "cell is 9 wide at 720");
        check(grid.cell_h == 16, "cell is 16 tall, so the VGA font was chosen");
        check(grid.x0 == 0 && grid.y0 == 0, "the text area fills the frame");
        check(grid.width == 720 && grid.height == 400, "the frame size came back");
        check(grid.confidence == 100, "confidence is 100%");
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, l0) != NULL, "header line came back exactly");
        check(strstr(out, "[UEFI: Samsung SSD 990 PRO 2TB]") != NULL,
              "the inverted (selected) row came back");
        check(strstr(out, "e8:f6:0a:e0:cb:87") != NULL, "the MAC came back");
        check(strstr(out, l3) != NULL, "every ASCII symbol came back");
        check(strstr(out, "───") != NULL, "box drawing came back as Unicode");
        check(strstr(out, "   \n") == NULL, "trailing blanks are trimmed");

        /* The selected row is the one drawn dark on light, and that is the only
           thing a text screen says about a menu's selection. */
        const size_t sel = (size_t)5 * cols;
        check(screentext_marked(&grid, sel + 2), "the selected row's first letter is marked");
        check(screentext_marked(&grid, sel + 17),
              "a blank inside the selected label is marked with it");
        check(!screentext_marked(&grid, sel + 0),
              "padding before the selection is not marked");
        check(!screentext_marked(&grid, sel + strlen(l1)),
              "padding after the selection is not marked");
        check(marks_in_row(&grid, 5) == strlen(l1) - 2,
              "the mark covers the label and nothing else");
        uint32_t elsewhere = 0;
        for (uint16_t r = 0; r < grid.rows; r++) {
            if (r != 5) elsewhere += marks_in_row(&grid, r);
        }
        check(elsewhere == 0, "no other row is marked");
    }
    free(buf);

    /* --- a screen drawn the other way round: black on white, one light row ---
       Inversion is relative to the screen, so here it is the ordinary-looking
       row that is the selection, and the rest of the page is not marked. */
    uint8_t *inv = malloc(cols * rows);
    memset(inv, ' ', cols * rows);
    memcpy(inv + 2 * cols, l2, strlen(l2));
    memcpy(inv + 6 * cols, l1, strlen(l1));
    for (uint32_t r = 0; r < SCREENTEXT_MAX_ROWS; r++) pairs[r] = (pair_t){20, 220};
    pairs[6] = (pair_t){200, 30}; /* the selected row, light on dark this time */
    buf = render(inv, cols, rows, 9, SCREENTEXT_FMT_UYVY, pairs, &vga, 0, 0, &w, &h, &stride);
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_UYVY, w, h, stride};
    const bool got_inv = screentext_scan(&frame, &grid);
    check(got_inv, "a black-on-white screen still reads");
    if (got_inv) {
        check(marks_in_row(&grid, 6) == strlen(l1) - 2,
              "on an inverted page the odd row out is the marked one");
        check(marks_in_row(&grid, 2) == 0, "the page itself is not marked");
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, "5CG9382KJ7") != NULL, "text still came back from the inverted page");
    }
    free(buf);
    free(inv);

    /* --- the same screen in BGR888 at 640x480, 8-wide cells --- */
    uint8_t *text2 = malloc(cols * 30);
    memset(text2, ' ', cols * 30);
    memcpy(text2 + 3 * cols, l2, strlen(l2));
    for (uint32_t r = 0; r < 30; r++) pairs[r] = (pair_t){230, 10};
    buf = render(text2, cols, 30, 8, SCREENTEXT_FMT_RGB888, pairs, &vga, 0, 0, &w, &h, &stride);
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_RGB888, w, h, stride};
    const bool got2 = screentext_scan(&frame, &grid);
    check(got2 && grid.rows == 30 && grid.cell_w == 8, "640x480 BGR888 reads as 80x30");
    if (got2) {
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, "5CG9382KJ7") != NULL, "serial came back from the BGR frame");
    }
    free(buf);
    free(text2);

    /*
     * --- a UEFI console: 1024x768, 128x40 cells of 8x19, text area centred ---
     *
     * This is what a real boot menu looks like: 768 is 40 rows of 19 with 8
     * pixels left over, which the firmware splits above and below.
     */
    const uint32_t ucols = 128, urows = 40;
    uint8_t *utext = malloc(ucols * urows);
    memset(utext, ' ', ucols * urows);
    const char *u0 = "NixOS (Generation 21 NixOS Yarara 26.05.20260505.549bd84 (Linux 7.0.3))";
    const char *u1 = "Reboot Into Firmware Interface";
    memcpy(utext + 4 * ucols + 17, u0, strlen(u0));
    memcpy(utext + 30 * ucols + 48, u1, strlen(u1));
    for (uint32_t r = 0; r < urows; r++) pairs[r] = (pair_t){190, 20};
    pairs[4] = (pair_t){20, 200}; /* the highlighted entry */

    buf = render(utext, ucols, urows, 8, SCREENTEXT_FMT_UYVY, pairs, &uefi, 4, 4,
                 &w, &h, &stride);
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_UYVY, w, h, stride};
    check(w == 1024 && h == 768, "the UEFI frame is 1024x768");
    check(screentext_mode_supported(w, h), "1024x768 is a supported mode");
    const bool got3 = screentext_scan(&frame, &grid);
    check(got3, "a UEFI console reads");
    if (got3) {
        check(grid.cols == 128 && grid.rows == 40, "grid is 128x40");
        check(grid.cell_h == 19, "cell is 19 tall, so the UEFI font was chosen");
        check(grid.y0 == 4, "the centred text area was found (4 pixels down)");
        check(grid.confidence == 100, "confidence is 100%");
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, u0) != NULL, "the highlighted boot entry came back");
        check(strstr(out, u1) != NULL, "the last line came back");
    }
    free(buf);
    free(utext);

    /* --- 640x480 could be either font, so the glyphs have to decide --- */
    uint8_t *u480 = malloc(cols * 25);
    memset(u480, ' ', cols * 25);
    memcpy(u480 + 6 * cols, l2, strlen(l2));
    for (uint32_t r = 0; r < 25; r++) pairs[r] = (pair_t){210, 15};
    buf = render(u480, cols, 25, 8, SCREENTEXT_FMT_UYVY, pairs, &uefi, 2, 3, &w, &h, &stride);
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_UYVY, w, h, stride};
    check(w == 640 && h == 480, "a UEFI console at 640x480 is 80x25 of 8x19");
    /* 5 pixels over, so the margins differ: the reading must still land. */
    const bool got4 = screentext_scan(&frame, &grid);
    check(got4 && grid.cell_h == 19 && grid.rows == 25,
          "the 16-tall reading is rejected and the 19-tall one taken");
    if (got4) {
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, "5CG9382KJ7") != NULL, "serial came back from the UEFI frame");
    }
    free(buf);
    free(u480);
    free(text);

    /*
     * --- a Linux console, which draws with the PC-DOS font, not the IBM one ---
     *
     * They differ in 28 glyphs, five of them printable: ` f v | ~. Read with the
     * IBM table alone this screen scored 94% - accepted, with every f and v
     * quietly replaced by a space. Both fonts are in the 16-tall table now.
     */
    uint8_t *ltext = malloc(cols * 30);
    memset(ltext, ' ', cols * 30);
    const char *lx = "vova@nixos:~$ df -h /var | grep -v tmpfs";
    memcpy(ltext + 4 * cols, lx, strlen(lx));
    for (uint32_t r = 0; r < 30; r++) pairs[r] = (pair_t){210, 15};
    buf = render(ltext, cols, 30, 8, SCREENTEXT_FMT_UYVY, pairs, &pcdos, 0, 0, &w, &h, &stride);
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_UYVY, w, h, stride};
    const bool got5 = screentext_scan(&frame, &grid);
    check(got5 && grid.confidence == 100, "a Linux console reads at 100%");
    if (got5) {
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, lx) != NULL, "its f, v and | came back, not spaces");
    }
    free(buf);
    free(ltext);

    /*
     * --- a glyph in no table must be visible, not silently a space ---
     *
     * One character drawn in a font we do not have. The screen still reads - one
     * bad cell in hundreds is well above the threshold - but that cell has to say
     * it was not read, or the text is plausible and wrong.
     */
    font_t odd = vga;
    for (int y = 0; y < 16; y++) odd.rows['X'][y] = (uint8_t)(0xA5 ^ y); /* not any letter */
    uint8_t *xtext = malloc(cols * 30);
    memset(xtext, ' ', cols * 30);
    const char *xs = "Serial: 5CG9382KJ7X";
    memcpy(xtext + 4 * cols, xs, strlen(xs));
    memcpy(xtext + 6 * cols, l2, strlen(l2));
    buf = render(xtext, cols, 30, 8, SCREENTEXT_FMT_UYVY, pairs, &odd, 0, 0, &w, &h, &stride);
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_UYVY, w, h, stride};
    const bool got6 = screentext_scan(&frame, &grid);
    check(got6, "one unknown glyph does not lose the screen");
    if (got6) {
        screentext_to_utf8(&grid, out, sizeof out);
        check(strstr(out, "\xEF\xBF\xBD") != NULL, "the unread cell came back as U+FFFD");
        check(strstr(out, "Serial: 5CG9382KJ7 ") == NULL, "and not as a space");
    }
    free(buf);
    free(xtext);

    /* --- things that are not a text screen must be refused --- */
    const uint32_t nw = 720, nh = 400, nstride = nw * 2;
    buf = malloc((size_t)nstride * nh);
    srand(1);
    for (size_t i = 0; i < (size_t)nstride * nh; i++) buf[i] = rand() & 0xFF;
    frame = (screentext_frame_t){buf, SCREENTEXT_FMT_UYVY, nw, nh, nstride};
    check(!screentext_scan(&frame, &grid), "noise is refused");

    for (uint32_t y = 0; y < nh; y++)
        for (uint32_t x = 0; x < nw; x++)
            put_px(buf, SCREENTEXT_FMT_UYVY, nstride, x, y, (x * 255) / nw);
    check(!screentext_scan(&frame, &grid), "a gradient is refused");

    memset(buf, 0, (size_t)nstride * nh);
    check(!screentext_scan(&frame, &grid), "a blank screen is refused");
    free(buf);

    check(screentext_mode_supported(1024, 768), "1024x768 fits both a 48-row and a 40-row grid");
    check(screentext_mode_supported(800, 600), "800x600 is a supported mode");

    /* The two ceilings. A wide mode can be read - a UEFI console fills a 1080p
       screen with 240x56 cells - but is not read unless somebody asks. */
    check(screentext_mode_supported(1920, 1080), "1080p can be read when asked for");
    check(!screentext_mode_unprompted(1920, 1080), "1080p is not read unasked");
    check(screentext_mode_supported(1280, 720), "720p can be read when asked for");
    check(!screentext_mode_unprompted(1280, 720), "720p is not read unasked");
    check(screentext_mode_supported(1280, 1024), "1280x1024 is 160 columns: readable");
    check(!screentext_mode_unprompted(1280, 1024), "1280x1024 is not read unasked");

    /* Anything narrow enough to be read unasked must also be readable at all. */
    check(screentext_mode_unprompted(1024, 768), "1024x768 is read unasked");
    check(screentext_mode_unprompted(720, 400), "720x400 is read unasked");

    /* Past the size of the grid, asking changes nothing: 2560 is 320 columns. */
    check(!screentext_mode_supported(2560, 1440), "2560 wide is 320 columns: refused");

    printf("\n%s\n", failures ? "FAILURES" : "all good");
    return failures ? 1 : 0;
}
