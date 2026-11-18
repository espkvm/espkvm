/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Frame rate limit: true when the frame that came at @p now_us should be
 * encoded. @p next_us is when the next one is due, 0 at the start.
 *
 * Frames come with jitter, so a frame up to a quarter interval early is taken.
 * The due time moves by a whole interval, so the average stays at the limit.
 * After a stall longer than an interval the missed slots are skipped and the
 * count starts again from now.
 */
static inline bool fps_gate_take(int64_t *next_us, int64_t now_us, int64_t interval_us)
{
    if (now_us < *next_us - interval_us / 4) {
        return false;
    }
    *next_us = now_us - *next_us > interval_us ? now_us + interval_us : *next_us + interval_us;
    return true;
}

/**
 * Which share of the source's frames the encoder keeps up with: 1 = every
 * frame, 2 = every other one (1080p50 on rev 3.x). @p made is the measured
 * rate, @p div the current answer. The measured rate wanders by several
 * frames with the picture, so the answer moves only when the rate is clearly
 * nearer another share.
 */
static inline int32_t fps_plan_div(int32_t div, int32_t input_hz, int32_t made)
{
    if (input_hz <= 0 || made <= 0) {
        return div;
    }
    const int32_t r100 = input_hz * 100 / made; /* input_hz / made, x100 */
    if (r100 > div * 100 + 70 || r100 < div * 100 - 70) {
        div = (r100 + 50) / 100;
    }
    return div < 1 ? 1 : div;
}
