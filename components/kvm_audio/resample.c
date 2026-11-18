/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_priv.h"

void resample_reset(resampler_t *rs, uint32_t rate)
{
    *rs = (resampler_t){.rate = rate};
}

size_t resample_block(resampler_t *rs, const kvm_audio_block_t *b, int16_t *out, int64_t *first_us)
{
    const uint32_t step = (uint32_t)(((uint64_t)rs->rate << 16) / OUT_RATE);
    const uint32_t end = (uint32_t)b->frames << 16;
    *first_us = b->start_us + (int64_t)(rs->pos >> 16) * 1000000 / rs->rate;
    size_t n = 0;
    while (rs->pos < end && n < RESAMPLE_MAX) {
        const uint32_t i = rs->pos >> 16;
        const int32_t frac = (int32_t)(rs->pos & 0xffff);
        for (int c = 0; c < KVM_AUDIO_CHANNELS; c++) {
            const int32_t b1 = b->pcm[i * KVM_AUDIO_CHANNELS + c];
            const int32_t a = i ? b->pcm[(i - 1) * KVM_AUDIO_CHANNELS + c]
                                : (rs->have_prev ? rs->prev[c] : b1);
            /* Between the frame before i and frame i: one input frame late,
             * which spares waiting for the next block. */
            out[n * KVM_AUDIO_CHANNELS + c] = (int16_t)(a + (int32_t)(((int64_t)(b1 - a) * frac) >> 16));
        }
        rs->pos += step;
        n++;
    }
    rs->pos = rs->pos >= end ? rs->pos - end : 0;
    rs->prev[0] = b->pcm[(b->frames - 1) * KVM_AUDIO_CHANNELS];
    rs->prev[1] = b->pcm[(b->frames - 1) * KVM_AUDIO_CHANNELS + 1];
    rs->have_prev = true;
    return n;
}
