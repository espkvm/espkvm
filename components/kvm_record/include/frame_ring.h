/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encoded frames in memory, oldest first, within a byte budget.
 *
 * One ring serves both jobs. While a recording runs it is the queue between the
 * frame reader and the card writer. While the dashcam waits for something to
 * happen it is the last minutes of the screen: when it is full the oldest
 * group of pictures goes - everything up to the next keyframe - so what is kept
 * always starts on a frame that decodes on its own.
 *
 * Plain C, no locking of its own: the caller holds a lock around every call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t off; /* start in the byte buffer */
    uint32_t len;
    int64_t at_us;
    bool keyframe;
} frame_ring_entry_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    frame_ring_entry_t *entries;
    size_t max_entries;
    size_t head;   /* oldest entry */
    size_t count;  /* entries held */
    size_t used;   /* bytes held */
    uint32_t wpos; /* where the next frame's bytes go */
} frame_ring_t;

/** @p buf and @p entries belong to the caller and outlive the ring. */
void frame_ring_init(frame_ring_t *r, uint8_t *buf, size_t cap, frame_ring_entry_t *entries,
                     size_t max_entries);

void frame_ring_clear(frame_ring_t *r);

/**
 * Add a frame. With @p evict, whole groups of pictures are dropped from the old
 * end to make room; without it a full ring refuses. False when the frame could
 * not be added - too big, or no room without evicting.
 */
bool frame_ring_push(frame_ring_t *r, const uint8_t *data, size_t len, int64_t at_us, bool keyframe,
                     bool evict);

/** The oldest frame, without removing it. False when empty. */
bool frame_ring_peek(const frame_ring_t *r, frame_ring_entry_t *out);

/** Copy the oldest frame's bytes into @p out (at least its length) and drop it. */
bool frame_ring_pop(frame_ring_t *r, uint8_t *out, size_t out_cap, frame_ring_entry_t *info);

/** Drop the oldest group of pictures (a keyframe and what follows it). */
void frame_ring_drop_group(frame_ring_t *r);

/** How far back the ring reaches: newest minus oldest frame time, in us. */
int64_t frame_ring_span_us(const frame_ring_t *r);

#ifdef __cplusplus
}
#endif
