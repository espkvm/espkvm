/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "frame_ring.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                            \
        }                                                                                          \
    } while (0)

int main(void)
{
    static uint8_t buf[1000];
    static frame_ring_entry_t entries[16];
    static uint8_t frame[400], out[400];
    frame_ring_t r;
    frame_ring_init(&r, buf, sizeof(buf), entries, 16);

    /* Keeping the past, it starts only on a keyframe; as a queue, on anything. */
    CHECK(!frame_ring_push(&r, frame, 10, 0, false, true));
    CHECK(frame_ring_push(&r, frame, 10, 0, true, true));
    frame_ring_clear(&r);
    CHECK(frame_ring_push(&r, frame, 10, 0, false, false));

    /* The dashcam: a keyframe every 4 frames, 100 bytes each, 1000 bytes of room.
       Whatever is kept starts on a keyframe and the bytes come out as they went in. */
    frame_ring_clear(&r);
    for (int i = 0; i < 40; i++) {
        memset(frame, i, 100);
        CHECK(frame_ring_push(&r, frame, 100, i * 1000, i % 4 == 0, true));
        frame_ring_entry_t e;
        CHECK(frame_ring_peek(&r, &e) && e.keyframe);
        CHECK(r.used <= sizeof(buf));
    }
    CHECK(r.count >= 7 && r.count <= 10);
    CHECK(frame_ring_span_us(&r) == (int64_t)(r.count - 1) * 1000);
    int expect = 40 - (int)r.count;
    frame_ring_entry_t info;
    while (frame_ring_pop(&r, out, sizeof(out), &info)) {
        CHECK(info.at_us == expect * 1000);
        CHECK(out[0] == expect && out[99] == expect);
        expect++;
    }
    CHECK(expect == 40 && r.count == 0 && r.used == 0);

    /* The recording queue: a full ring refuses rather than dropping the past. */
    frame_ring_clear(&r);
    int pushed = 0;
    while (frame_ring_push(&r, frame, 150, pushed * 1000, true, false)) {
        pushed++;
    }
    CHECK(pushed == 6 && r.used == 900);
    CHECK(frame_ring_pop(&r, out, sizeof(out), &info) && info.at_us == 0);
    CHECK(frame_ring_push(&r, frame, 150, 99000, true, false));

    /* Wrap-around keeps the bytes whole. */
    frame_ring_clear(&r);
    for (int i = 0; i < 25; i++) {
        for (int k = 0; k < 330; k++) {
            frame[k] = (uint8_t)(i * 7 + k);
        }
        CHECK(frame_ring_push(&r, frame, 330, i, true, true));
        CHECK(frame_ring_pop(&r, out, sizeof(out), &info));
        int same = 1;
        for (int k = 0; k < 330; k++) {
            same &= out[k] == (uint8_t)(i * 7 + k);
        }
        CHECK(same);
    }

    /* A frame bigger than the ring is refused. */
    CHECK(!frame_ring_push(&r, frame, 2000, 0, true, true));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("frame_ring: dashcam eviction, recording queue, wrap-around - all as expected\n");
    return 0;
}
