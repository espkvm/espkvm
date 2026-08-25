/*
 * Host test: build frames that look like the screens this has to tell apart,
 * and check which ones come back flat. No device needed - the decision is plain
 * arithmetic over a pixel buffer, which is exactly why it lives on its own.
 */
#include "capture_flat_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1280u
#define H 720u
#define PIXELS (W * H)

static int failures;
static void check(int ok, const char *what)
{
    printf("%-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/** Fill an RGB888 frame with one colour. */
static void fill_rgb(uint8_t *px, uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0; i < PIXELS; i++) {
        px[i * 3] = r;
        px[i * 3 + 1] = g;
        px[i * 3 + 2] = b;
    }
}

/** Fill a packed UYVY frame with one colour. */
static void fill_uyvy(uint8_t *px, uint8_t y, uint8_t u, uint8_t v)
{
    for (size_t i = 0; i < PIXELS / 2; i++) {
        px[i * 4] = u;
        px[i * 4 + 1] = y;
        px[i * 4 + 2] = v;
        px[i * 4 + 3] = y;
    }
}

/** Scatter `percent` of the frame with another colour, the way text sits on a
    stop screen: a small share of the pixels, spread all over it. */
static void speckle_rgb(uint8_t *px, unsigned percent, uint8_t r, uint8_t g, uint8_t b)
{
    const size_t every = 100u / (percent ? percent : 1u);
    for (size_t i = 0; i < PIXELS; i += every) {
        px[i * 3] = r;
        px[i * 3 + 1] = g;
        px[i * 3 + 2] = b;
    }
}

int main(void)
{
    uint8_t *rgb = malloc((size_t)PIXELS * 3);
    uint8_t *uyvy = malloc((size_t)PIXELS * 2);
    if (!rgb || !uyvy) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* The screens this exists for. */
    fill_rgb(rgb, 0x00, 0x78, 0xd7); /* the blue of a Windows stop screen */
    check(capture_flat_is_flat(rgb, PIXELS, 3), "a screen filled with one blue is flat");

    speckle_rgb(rgb, 4, 0xff, 0xff, 0xff); /* its text and icon */
    check(capture_flat_is_flat(rgb, PIXELS, 3), "and stays flat with text scattered over it");

    fill_rgb(rgb, 0, 0, 0);
    check(capture_flat_is_flat(rgb, PIXELS, 3), "a blanked, black screen is flat");

    fill_uyvy(uyvy, 0x20, 0xe0, 0x60); /* the same idea in the other format */
    check(capture_flat_is_flat(uyvy, PIXELS, 2), "UYVY is read too, chroma and all");

    /* And the screens it must not fire on. */
    for (size_t i = 0; i < PIXELS; i++) {
        const uint8_t v = (uint8_t)((i / W) & 0xff);
        rgb[i * 3] = v;
        rgb[i * 3 + 1] = v;
        rgb[i * 3 + 2] = v;
    }
    check(!capture_flat_is_flat(rgb, PIXELS, 3), "a gradient is not flat");

    srand(1);
    for (size_t i = 0; i < PIXELS * 3; i++) rgb[i] = (uint8_t)(rand() & 0xff);
    check(!capture_flat_is_flat(rgb, PIXELS, 3), "a busy picture is not flat");

    fill_rgb(rgb, 0x10, 0x10, 0x10);
    for (size_t i = PIXELS / 2; i < PIXELS; i++) { /* half the screen a window */
        rgb[i * 3] = 0xd0;
        rgb[i * 3 + 1] = 0xd0;
        rgb[i * 3 + 2] = 0xd0;
    }
    check(!capture_flat_is_flat(rgb, PIXELS, 3), "two colours, half and half, is not flat");

    fill_uyvy(uyvy, 0x40, 0x80, 0x80);
    for (size_t i = 0; i < PIXELS / 2; i += 3) uyvy[i * 4 + 1] = 0xf0; /* a third of it bright */
    check(!capture_flat_is_flat(uyvy, PIXELS, 2), "a third of the luma changed is not flat");

    /* A frame too small to sample says no rather than guessing. */
    check(!capture_flat_is_flat(rgb, 16, 3), "a frame smaller than the sample is refused");

    free(rgb);
    free(uyvy);
    puts(failures ? "\nFAILURES" : "\nall good");
    return failures ? 1 : 0;
}
