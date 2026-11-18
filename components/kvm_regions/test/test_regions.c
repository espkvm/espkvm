/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the region arithmetic: sh components/kvm_regions/test/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "region_core.h"

static int fails;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);          \
            fails++;                                                     \
        }                                                                \
    } while (0)

enum { W = 320, H = 240 };
static uint8_t rgb[W * H * 3];
static uint8_t uyvy[W * H * 2];

/* Paint a rectangle in both frames: RGB stored as BGR, and UYVY. */
static void paint(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    const int Y = (77 * r + 150 * g + 29 * b) >> 8;
    const int U = 128 + ((-43 * r - 85 * g + 128 * b) >> 8);
    const int V = 128 + ((128 * r - 107 * g - 21 * b) >> 8);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint8_t *p = rgb + (y * W + x) * 3;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            uint8_t *q = uyvy + (y * W + (x & ~1)) * 2;
            q[0] = (uint8_t)U;
            q[2] = (uint8_t)V;
            q[1 + (x & 1) * 2] = (uint8_t)Y;
        }
    }
}

static const region_frame_t F_RGB = {rgb, W, H, 3, 0, true};
static const region_frame_t F_YUV = {uyvy, W, H, 2, 1, false};
/* The right half of the frame. */
static const region_rect_t HALF = {5000, 0, 5000, 10000};

static void look_and_colour(void)
{
    paint(0, 0, W, H, 0, 0, 0);
    paint(W / 2, 0, W, H, 220, 30, 30);
    region_look_t a, b;
    CHECK(region_look(&F_RGB, &HALF, &a));
    CHECK(region_look(&F_YUV, &HALF, &b));
    CHECK(a.colour == REGION_COL_RED);
    CHECK(b.colour == REGION_COL_RED);
    CHECK(a.colour_pct == 100);
    CHECK(region_diff(a.sig, b.sig) == 0); /* both formats see the same picture */

    const region_rect_t left = {0, 0, 5000, 10000};
    CHECK(region_look(&F_RGB, &left, &a));
    CHECK(a.colour == REGION_COL_BLACK);

    CHECK(region_classify(30, 200, 40) == REGION_COL_GREEN);
    CHECK(region_classify(40, 60, 220) == REGION_COL_BLUE);
    CHECK(region_classify(240, 240, 240) == REGION_COL_WHITE);
    CHECK(region_classify(128, 128, 128) == REGION_COL_GREY);
    CHECK(region_classify(240, 220, 30) == REGION_COL_YELLOW);

    const region_rect_t off = {9000, 0, 2000, 1000};
    CHECK(!region_look(&F_RGB, &off, &a));
}

static region_look_t look_now(const region_rect_t *r)
{
    region_look_t l;
    region_look(&F_RGB, r, &l);
    return l;
}

static void change_mode(void)
{
    region_cfg_t c = {.id = 1, .mode = REGION_CHANGE, .threshold = 5, .hold_s = 0, .rect = HALF};
    region_state_t s = {0};
    paint(0, 0, W, H, 10, 10, 10);
    region_look_t l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 1000)); /* the first look is the base */
    CHECK(!region_step(&c, &s, &l, 1200));
    /* A bar a tenth of the region tall: well past 5% of the cells. */
    paint(W / 2, 0, W, H / 10, 250, 250, 250);
    l = look_now(&c.rect);
    CHECK(region_step(&c, &s, &l, 1400));
    CHECK(s.active && s.events == 1);
    /* An animation: the bar keeps growing on the next look, the same change. */
    paint(W / 2, 0, W, H / 5, 250, 250, 250);
    l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 1600));
    CHECK(!region_step(&c, &s, &l, 1800)); /* settled, and the new picture is the base */
    CHECK(s.active && s.events == 1);      /* still within the pulse */
    paint(0, 0, W, H, 10, 10, 10);
    l = look_now(&c.rect);
    CHECK(region_step(&c, &s, &l, 2000)); /* after a calm look: a new change */
    CHECK(s.events == 2);
    CHECK(!region_step(&c, &s, &l, 2200));
    CHECK(!region_step(&c, &s, &l, 2000 + REGION_PULSE_MS + 1));
    CHECK(!s.active);

    /* With a hold, a flicker shorter than it is not an event. */
    c.hold_s = 2;
    region_reset(&s);
    paint(0, 0, W, H, 10, 10, 10);
    l = look_now(&c.rect);
    region_step(&c, &s, &l, 1000);
    paint(W / 2, 0, W, H / 4, 250, 250, 250);
    l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 1200));
    paint(0, 0, W, H, 10, 10, 10);
    l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 1400));
    paint(W / 2, 0, W, H / 4, 250, 250, 250);
    l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 2000));
    CHECK(region_step(&c, &s, &l, 4100));
}

static void still_mode(void)
{
    /* A thin bar moves two cells of 256 at a time: the threshold for a zone
       like this is the lowest one, 1%. At 5% it would add up slower than the
       hold and look still. */
    region_cfg_t c = {.id = 2, .mode = REGION_STILL, .threshold = 1, .hold_s = 10, .rect = HALF};
    region_state_t s = {0};
    uint32_t t = 1000;
    /* A progress bar creeping one column at a time: each step is tiny, the
       sum is not, so it must never count as still. */
    paint(0, 0, W, H, 0, 0, 0);
    region_look_t l = look_now(&c.rect);
    region_step(&c, &s, &l, t);
    for (int x = W / 2; x < W; x += 2) {
        paint(W / 2, H / 2 - 10, x, H / 2 + 10, 0, 200, 0);
        l = look_now(&c.rect);
        t += 1000;
        CHECK(!region_step(&c, &s, &l, t));
    }
    /* Then it stops: still exactly hold_s after the last move it made. */
    CHECK(t - s.moved_ms < 5000);
    const uint32_t due = s.moved_ms + 10000;
    CHECK(!region_step(&c, &s, &l, due - 1));
    t = due;
    CHECK(region_step(&c, &s, &l, t));
    CHECK(s.active);
    CHECK(!region_step(&c, &s, &l, t + 1000)); /* once */
}

static void like_and_colour(void)
{
    region_cfg_t c = {.id = 3, .mode = REGION_LIKE, .threshold = 10, .hold_s = 1, .rect = HALF};
    region_state_t s = {0};
    paint(0, 0, W, H, 0, 0, 0);
    paint(W / 2, H / 3, W, H / 2, 200, 200, 200); /* "the login box" */
    region_look_t l = look_now(&c.rect);
    memcpy(c.ref, l.sig, REGION_CELLS);
    c.has_ref = true;
    paint(0, 0, W, H, 0, 0, 80); /* something else */
    l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 1000));
    CHECK(!s.active);
    paint(0, 0, W, H, 0, 0, 0);
    paint(W / 2, H / 3, W, H / 2, 200, 200, 200);
    l = look_now(&c.rect);
    CHECK(!region_step(&c, &s, &l, 2000)); /* holding */
    CHECK(region_step(&c, &s, &l, 3000));
    CHECK(s.active);

    c.mode = REGION_UNLIKE;
    region_reset(&s);
    CHECK(!region_step(&c, &s, &l, 4000));
    CHECK(!s.active);

    region_cfg_t k = {.id = 4, .mode = REGION_COLOUR, .colour = REGION_COL_GREEN, .threshold = 10, .rect = HALF};
    region_state_t ks = {0};
    paint(0, 0, W, H, 200, 20, 20);
    l = look_now(&k.rect);
    CHECK(!region_step(&k, &ks, &l, 1000));
    CHECK(ks.colour == REGION_COL_RED);
    paint(0, 0, W, H, 20, 200, 20);
    l = look_now(&k.rect);
    CHECK(region_step(&k, &ks, &l, 2000));
    CHECK(ks.active);

    k.colour = REGION_COL_ANY;
    region_reset(&ks);
    CHECK(!region_step(&k, &ks, &l, 1000));
    paint(0, 0, W, H, 220, 210, 20);
    l = look_now(&k.rect);
    CHECK(region_step(&k, &ks, &l, 2000));
    CHECK(!region_step(&k, &ks, &l, 2200));
}

static void names(void)
{
    CHECK(region_mode_parse("Still") == REGION_STILL);
    CHECK(region_mode_parse("color") == REGION_COLOUR);
    CHECK(region_mode_parse("nope") == -1);
    CHECK(region_colour_parse("gray") == REGION_COL_GREY);
    CHECK(strcmp(region_colour_name(REGION_COL_MAGENTA), "magenta") == 0);
}

int main(void)
{
    look_and_colour();
    change_mode();
    still_mode();
    like_and_colour();
    names();
    if (fails) {
        printf("regions: %d failed\n", fails);
        return 1;
    }
    printf("regions: all passed\n");
    return 0;
}
