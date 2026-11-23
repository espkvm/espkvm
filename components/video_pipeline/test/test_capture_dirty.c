/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which parts of a frame changed (capture_dirty.c), on a host.
 */
#include "capture_dirty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

enum { MAXW = 1920, MAXH = 1088, BPP = 3 };

static dirty_t d;
static uint8_t *mem;
static uint8_t *frame;
static uint32_t W, H;

static void setup(uint32_t w, uint32_t h, bool want_ok)
{
    W = w;
    H = h;
    memset(frame, 0x40, (size_t)MAXW * MAXH * BPP);
    dirty_init(&d, mem, MAXW, MAXH);
    CHECK(dirty_reset(&d, w, h) == want_ok);
}

static void put(uint32_t x, uint32_t y, uint8_t v)
{
    frame[((size_t)y * W + x) * BPP + 1] = v;
}

static void scan(void)
{
    dirty_scan(&d, frame, W * BPP, BPP);
}

static int take(int slot, dirty_rect_t *r)
{
    return dirty_take(&d, slot, r, 8);
}

static bool is_whole(const dirty_rect_t *r)
{
    return r->x == 0 && r->y == 0 && r->w == W && r->h == H;
}

/* Settle: both parities hashed, both buffers filled. */
static void settle(void)
{
    dirty_rect_t r[8];
    scan();
    scan();
    take(0, r);
    take(1, r);
}

static void test_start_is_whole(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    CHECK(take(0, r) == 1 && is_whole(&r[0]));
    CHECK(take(1, r) == 1 && is_whole(&r[0]));
    CHECK(take(0, r) == 0);
    /* The first two scans only learn the hashes. */
    scan();
    scan();
    CHECK(take(0, r) == 0);
    CHECK(take(1, r) == 0);
}

static void test_one_change_both_slots(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    settle();
    put(200, 84, 0x99); /* even row: stripe 5, segment 3 */
    scan();             /* parity 0: even rows */
    CHECK(take(0, r) == 1);
    CHECK(r[0].x == 192 && r[0].w == 64 && r[0].y == 80 && r[0].h == 16);
    CHECK(take(0, r) == 0);
    CHECK(take(1, r) == 1 && r[0].x == 192 && r[0].y == 80);
    /* Nothing new: nothing to do. */
    scan();
    scan();
    CHECK(take(0, r) == 0 && take(1, r) == 0);
}

static void test_odd_row_seen_next_frame(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    settle();
    put(10, 33, 0x99); /* odd row */
    scan();            /* even rows: not seen */
    CHECK(take(0, r) == 0);
    scan();            /* odd rows: seen */
    CHECK(take(0, r) == 1 && r[0].x == 0 && r[0].y == 32 && r[0].h == 16);
}

static void test_change_and_back_is_not_lost(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    settle();
    /* Changes while buffer 1 is not being filled must still reach it. */
    put(500, 100, 0x99);
    scan();
    CHECK(take(0, r) == 1);
    put(500, 100, 0x40); /* back as it was */
    scan();              /* odd rows */
    scan();              /* even rows: back to the old hash, marks it again */
    CHECK(take(0, r) == 1 && r[0].y == 96);
    CHECK(take(1, r) == 1 && r[0].y == 96);
}

static void test_neighbours_merge(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    settle();
    put(70, 16, 0x99);   /* stripe 1, segment 1 */
    put(900, 32, 0x99);  /* stripe 2, segment 14 */
    put(100, 200, 0x99); /* stripe 12, segment 1 */
    scan();
    CHECK(take(0, r) == 2);
    CHECK(r[0].y == 16 && r[0].h == 32 && r[0].x == 64 && r[0].w == 15 * 64 - 64);
    CHECK(r[1].y == 192 && r[1].h == 16 && r[1].x == 64 && r[1].w == 64);
}

static void test_big_change_is_whole(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    settle();
    for (uint32_t y = 0; y < 700; y += 2) {
        put(5, y, 0x99);
        put(1900, y, 0x99);
    }
    scan();
    CHECK(take(0, r) == 1 && is_whole(&r[0]));
}

static void test_too_many_is_whole(void)
{
    dirty_rect_t r[8];
    setup(1920, 1080, true);
    settle();
    for (uint32_t s = 0; s < 20; s++) {
        put(0, s * 32, 0x99); /* every other stripe: 20 rectangles */
    }
    scan();
    CHECK(take(0, r) == 1 && is_whole(&r[0]));
}

static void test_last_stripe_and_segment(void)
{
    dirty_rect_t r[8];
    setup(1366, 768, true); /* 4098-byte rows: not word aligned */
    settle();
    put(1365, 766, 0x99);
    scan();
    CHECK(take(0, r) == 1);
    CHECK(r[0].x == 1344 && r[0].w == 22 && r[0].y == 752 && r[0].h == 16);
    setup(1920, 1080, true);
    settle();
    put(0, 1078, 0x99);
    scan();
    CHECK(take(0, r) == 1 && r[0].y == 1072 && r[0].h == 8);
}

static void test_untracked(void)
{
    dirty_rect_t r[8];
    setup(1365, 768, false); /* odd width */
    CHECK(take(0, r) == -1);
    setup(1920, 1200, false); /* taller than the memory */
    CHECK(take(0, r) == -1);
    setup(1920, 1080, true);
    settle();
    dirty_mark_all(&d, 1);
    CHECK(take(0, r) == 0);
    CHECK(take(1, r) == 1 && is_whole(&r[0]));
}

static void test_even_edges(void)
{
    dirty_rect_t r[8];
    setup(1280, 720, true);
    settle();
    for (uint32_t k = 0; k < 6; k++) {
        put(k * 211 % 1280, k * 97 % 720, 0x99);
    }
    scan();
    scan();
    int n = take(0, r);
    CHECK(n >= 1);
    for (int i = 0; i < n; i++) {
        CHECK(r[i].x % 2 == 0 && r[i].y % 2 == 0 && r[i].w % 2 == 0 && r[i].h % 2 == 0);
        CHECK(r[i].x + r[i].w <= W && r[i].y + r[i].h <= H);
    }
}

int main(void)
{
    mem = malloc(dirty_mem_bytes(MAXW, MAXH));
    frame = malloc((size_t)MAXW * MAXH * BPP);
    if (!mem || !frame) {
        return 1;
    }
    test_start_is_whole();
    test_one_change_both_slots();
    test_odd_row_seen_next_frame();
    test_change_and_back_is_not_lost();
    test_neighbours_merge();
    test_big_change_is_whole();
    test_too_many_is_whole();
    test_last_stripe_and_segment();
    test_untracked();
    test_even_edges();
    free(mem);
    free(frame);
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("changed stripes: all passed\n");
    return 0;
}
