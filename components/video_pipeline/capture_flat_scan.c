/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Is this frame one flat colour? See capture_flat.c for why that is worth
 * knowing, and capture_flat_scan.h for why it lives apart from it.
 */
#include "capture_flat_scan.h"

#include <stdlib.h>

/*
 * How many pixels to look at, and how alike they have to be.
 *
 * 512 samples spread over the frame is enough to tell "one colour" from
 * "a picture" with a wide margin: a photograph or a desktop misses by a mile,
 * and a stop screen carrying white text and an icon still leaves well over nine
 * tenths of itself on one colour. The tolerance is per channel and generous
 * enough for the noise a capture bridge adds to a flat fill.
 */
#define FLAT_SAMPLES 512
#define FLAT_TOLERANCE 12
#define FLAT_MIN_PERCENT 92
/* A flat colour this dark is a blanked screen. Black is 0, or 16 from a
   bridge sending limited range. */
#define FLAT_DARK_MAX 40

/** One pixel's three components, whatever the capture format calls them. */
static inline void sample_at(const uint8_t *px, size_t i, uint8_t bytes_per_px, uint8_t luma_off,
                             uint8_t out[3])
{
    if (bytes_per_px == 3) {
        out[0] = px[i * 3];
        out[1] = px[i * 3 + 1];
        out[2] = px[i * 3 + 2];
        return;
    }
    /* Packed 4:2:2: two pixels share a chroma pair, so step in pairs and take
       the luma of the first one. Which byte that is depends on the bridge -
       UYVY puts it second, an LT6911D first. The two chroma bytes carry the
       colour, which is what tells a blue fill from a black one; they swap
       places with the luma, and for "is this one flat colour" it does not
       matter which of them is U. */
    const size_t pair = (i / 2) * 4;
    out[0] = px[pair + luma_off];
    out[1] = px[pair + (luma_off ? 0u : 1u)];
    out[2] = px[pair + (luma_off ? 2u : 3u)];
}

bool capture_flat_is_flat(const uint8_t *px, size_t pixels, uint8_t bytes_per_px,
                          uint8_t luma_off, bool *dark)
{
    if (dark) {
        *dark = false;
    }
    /* UYVY stores two pixels in four bytes, so an odd count has a half pair at
       the end that is not there. Round it away rather than trusting every mode
       to be even: reading one byte past a frame buffer is not worth the two
       characters this costs. */
    if (bytes_per_px == 2) {
        pixels &= ~(size_t)1;
    }
    if (pixels < FLAT_SAMPLES) {
        return false;
    }
    /* Coprime with any row length, so the samples cannot all land in one
       column - the same trick the text reader's probe uses. */
    const size_t step = (pixels / FLAT_SAMPLES) | 1u;

    uint32_t sum[3] = {0, 0, 0};
    for (uint32_t n = 0; n < FLAT_SAMPLES; n++) {
        uint8_t c[3];
        sample_at(px, (n * step) % pixels, bytes_per_px, luma_off, c);
        sum[0] += c[0];
        sum[1] += c[1];
        sum[2] += c[2];
    }
    const uint8_t mean[3] = {
        (uint8_t)(sum[0] / FLAT_SAMPLES),
        (uint8_t)(sum[1] / FLAT_SAMPLES),
        (uint8_t)(sum[2] / FLAT_SAMPLES),
    };

    /* Second pass rather than a stored copy of the samples: reading 512 pixels
       again is cheaper than 1.5 KB of stack in the capture task. */
    uint32_t near = 0;
    for (uint32_t n = 0; n < FLAT_SAMPLES; n++) {
        uint8_t c[3];
        sample_at(px, (n * step) % pixels, bytes_per_px, luma_off, c);
        if (abs((int)c[0] - mean[0]) <= FLAT_TOLERANCE &&
            abs((int)c[1] - mean[1]) <= FLAT_TOLERANCE &&
            abs((int)c[2] - mean[2]) <= FLAT_TOLERANCE) {
            near++;
        }
    }
    const bool flat = near * 100 >= FLAT_SAMPLES * FLAT_MIN_PERCENT;
    if (flat && dark) {
        /* UYVY: the luma alone says how bright. RGB: the brightest channel. */
        const uint8_t bright = bytes_per_px == 2 ? mean[0]
                               : mean[0] > mean[1] ? (mean[0] > mean[2] ? mean[0] : mean[2])
                                                   : (mean[1] > mean[2] ? mean[1] : mean[2]);
        *dark = bright <= FLAT_DARK_MAX;
    }
    return flat;
}

