/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "frame_ring.h"

#include <string.h>

void frame_ring_init(frame_ring_t *r, uint8_t *buf, size_t cap, frame_ring_entry_t *entries,
                     size_t max_entries)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
    r->entries = entries;
    r->max_entries = max_entries;
}

void frame_ring_clear(frame_ring_t *r)
{
    r->head = 0;
    r->count = 0;
    r->used = 0;
    r->wpos = 0;
}

static frame_ring_entry_t *at(const frame_ring_t *r, size_t i)
{
    return &r->entries[(r->head + i) % r->max_entries];
}

static void drop_oldest(frame_ring_t *r)
{
    r->used -= at(r, 0)->len;
    r->head = (r->head + 1) % r->max_entries;
    r->count--;
    if (!r->count) {
        r->wpos = 0; /* empty: start from the beginning again */
    }
}

/* Drop the oldest group of pictures: its keyframe and everything up to the
 * next one, so the ring still starts on a keyframe. */
static void drop_oldest_group(frame_ring_t *r)
{
    drop_oldest(r);
    while (r->count && !at(r, 0)->keyframe) {
        drop_oldest(r);
    }
}

void frame_ring_drop_group(frame_ring_t *r)
{
    if (r->count) {
        drop_oldest_group(r);
    }
}

static void copy_in(frame_ring_t *r, uint32_t off, const uint8_t *data, size_t len)
{
    const size_t first = (off + len <= r->cap) ? len : r->cap - off;
    memcpy(r->buf + off, data, first);
    if (first < len) {
        memcpy(r->buf, data + first, len - first);
    }
}

bool frame_ring_push(frame_ring_t *r, const uint8_t *data, size_t len, int64_t at_us, bool keyframe,
                     bool evict)
{
    if (!len || len > r->cap) {
        return false;
    }
    while (r->count && (r->used + len > r->cap || r->count == r->max_entries)) {
        if (!evict) {
            return false;
        }
        drop_oldest_group(r);
    }
    /* Keeping the past: an empty ring may only begin on a keyframe. As a queue it
     * is emptied by the reader all the time, and any frame may follow. */
    if (evict && !r->count && !keyframe) {
        return false;
    }
    const size_t idx = (r->head + r->count) % r->max_entries;
    r->entries[idx] = (frame_ring_entry_t){.off = r->wpos, .len = (uint32_t)len, .at_us = at_us,
                                           .keyframe = keyframe};
    copy_in(r, r->wpos, data, len);
    r->wpos = (uint32_t)((r->wpos + len) % r->cap);
    r->count++;
    r->used += len;
    return true;
}

bool frame_ring_peek(const frame_ring_t *r, frame_ring_entry_t *out)
{
    if (!r->count) {
        return false;
    }
    *out = *at(r, 0);
    return true;
}

bool frame_ring_pop(frame_ring_t *r, uint8_t *out, size_t out_cap, frame_ring_entry_t *info)
{
    if (!r->count) {
        return false;
    }
    const frame_ring_entry_t e = *at(r, 0);
    if (e.len > out_cap) {
        drop_oldest(r); /* cannot be delivered; do not jam the ring on it */
        return false;
    }
    const size_t first = (e.off + e.len <= r->cap) ? e.len : r->cap - e.off;
    memcpy(out, r->buf + e.off, first);
    if (first < e.len) {
        memcpy(out + first, r->buf, e.len - first);
    }
    if (info) {
        *info = e;
    }
    drop_oldest(r);
    return true;
}

int64_t frame_ring_span_us(const frame_ring_t *r)
{
    if (r->count < 2) {
        return 0;
    }
    return at(r, r->count - 1)->at_us - at(r, 0)->at_us;
}
