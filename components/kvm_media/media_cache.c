/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "media_cache.h"

#include <string.h>

void media_cache_reset(media_cache_t *c, uint64_t size, uint32_t chunk_bytes)
{
    c->size = size;
    c->chunk_bytes = chunk_bytes;
    c->next = 0;
    c->clock = 0;
    media_cache_drop(c);
}

void media_cache_drop(media_cache_t *c)
{
    for (unsigned i = 0; i < c->count; i++) {
        c->chunks[i].len = 0;
        c->chunks[i].used = 0;
    }
}

bool media_cache_get(media_cache_t *c, uint64_t off, void *dst, uint32_t len)
{
    for (unsigned i = 0; i < c->count; i++) {
        media_chunk_t *k = &c->chunks[i];
        if (k->len && off >= k->off && off + len <= k->off + k->len) {
            memcpy(dst, k->buf + (off - k->off), len);
            k->used = ++c->clock;
            c->next = off + len;
            return true;
        }
    }
    return false;
}

media_chunk_t *media_cache_plan(media_cache_t *c, uint64_t off, uint32_t len, uint64_t *fill_off,
                                uint32_t *fill_len)
{
    const bool straight_on = off == c->next;
    c->next = off + len;
    if (!straight_on || !c->count || !c->chunk_bytes || len > c->chunk_bytes || off >= c->size) {
        return NULL;
    }
    const uint64_t left = c->size - off;
    const uint32_t n = left < c->chunk_bytes ? (uint32_t)left : c->chunk_bytes;
    if (n < len) {
        return NULL;
    }
    /* The one read from longest ago, an empty one first. */
    media_chunk_t *pick = &c->chunks[0];
    for (unsigned i = 1; i < c->count; i++) {
        media_chunk_t *k = &c->chunks[i];
        if (!pick->len) {
            break;
        }
        if (!k->len || k->used < pick->used) {
            pick = k;
        }
    }
    *fill_off = off;
    *fill_len = n;
    return pick;
}

void media_cache_filled(media_cache_t *c, media_chunk_t *k, uint64_t off, uint32_t len)
{
    k->off = off;
    k->len = len;
    k->used = ++c->clock;
}
