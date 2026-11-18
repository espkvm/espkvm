/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The read-ahead cache in front of every source. Plain C, so it runs in the
 * host tests too.
 *
 * USB hands reads over 4 KB at a time. For a card one command per 4 KB costs
 * more than the data, and for a network one request per 4 KB costs a round
 * trip each. So a read that follows the last one takes a whole chunk from the
 * source in one go, and the next reads come from memory. A read that jumps
 * elsewhere, a file system looking at its tables, goes to the source at its
 * own size, so random access does not pay for data it never uses.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint64_t off;  /* medium offset of buf[0] */
    uint32_t len;  /* valid bytes, 0 when empty */
    uint32_t used; /* the cache's clock when last read from */
} media_chunk_t;

typedef struct {
    media_chunk_t *chunks;
    unsigned count;
    uint32_t chunk_bytes; /* 0: no read-ahead */
    uint64_t size;        /* of the medium */
    uint64_t next;        /* where a read straight on would start */
    uint32_t clock;
} media_cache_t;

/** Start empty for a medium of @p size; the chunks keep their buffers. */
void media_cache_reset(media_cache_t *c, uint64_t size, uint32_t chunk_bytes);

/** Forget what is held, after a write for one. */
void media_cache_drop(media_cache_t *c);

/** Copy [off, off + len) from the cache if one chunk holds all of it. */
bool media_cache_get(media_cache_t *c, uint64_t off, void *dst, uint32_t len);

/**
 * For a read the cache missed: the chunk to fill and the range to fill it
 * with, or NULL when the read should go to the source as it is - it jumps,
 * there is no read-ahead, or it is bigger than a chunk. Also notes the read,
 * so the next one can tell it goes straight on. The range never passes the
 * end of the medium and always covers the read.
 */
media_chunk_t *media_cache_plan(media_cache_t *c, uint64_t off, uint32_t len, uint64_t *fill_off,
                                uint32_t *fill_len);

/** The chunk now holds @p len bytes from @p off. */
void media_cache_filled(media_cache_t *c, media_chunk_t *k, uint64_t off, uint32_t len);
