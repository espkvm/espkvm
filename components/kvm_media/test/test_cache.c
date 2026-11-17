/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The read-ahead cache, against a fake medium whose byte at offset i is i & 0xff.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_cache.h"

static int failures;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

static unsigned source_reads;

static void source_read(uint64_t off, uint8_t *buf, uint32_t len)
{
    source_reads++;
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(off + i);
    }
}

/* What kvm_media does with the cache, minus the locking. */
static void media_read(media_cache_t *c, uint64_t off, uint8_t *dst, uint32_t len)
{
    if (media_cache_get(c, off, dst, len)) {
        return;
    }
    uint64_t foff;
    uint32_t flen;
    media_chunk_t *k = media_cache_plan(c, off, len, &foff, &flen);
    if (k) {
        source_read(foff, k->buf, flen);
        media_cache_filled(c, k, foff, flen);
        memcpy(dst, k->buf + (off - foff), len);
        return;
    }
    source_read(off, dst, len);
}

static int right(const uint8_t *buf, uint64_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != (uint8_t)(off + i)) {
            return 0;
        }
    }
    return 1;
}

static media_cache_t make(unsigned count, uint32_t chunk, uint64_t size)
{
    media_cache_t c = {0};
    c.chunks = calloc(count, sizeof(media_chunk_t));
    c.count = count;
    for (unsigned i = 0; i < count; i++) {
        c.chunks[i].buf = malloc(chunk);
    }
    media_cache_reset(&c, size, chunk);
    return c;
}

int main(void)
{
    uint8_t buf[8192];

    /* Straight on: the first read fills a chunk, the next 15 come from it. */
    media_cache_t c = make(1, 64 * 1024, 1 << 20);
    source_reads = 0;
    for (uint64_t off = 0; off < 64 * 1024; off += 4096) {
        media_read(&c, off, buf, 4096);
        CHECK(right(buf, off, 4096));
    }
    CHECK(source_reads == 1);

    /* A jump goes to the source at its own size and fills nothing. */
    source_reads = 0;
    media_read(&c, 500 * 1024, buf, 4096);
    CHECK(right(buf, 500 * 1024, 4096));
    CHECK(source_reads == 1);
    CHECK(c.chunks[0].off == 0); /* the chunk from before is kept */
    /* ...and the read after it, going straight on, fills a chunk. */
    media_read(&c, 504 * 1024, buf, 4096);
    CHECK(source_reads == 2);
    CHECK(c.chunks[0].off == 504 * 1024);

    /* Near the end the chunk is cut to the medium, and still covers the read. */
    media_cache_t e = make(1, 64 * 1024, 10000);
    e.next = 8192;
    uint64_t foff;
    uint32_t flen;
    media_chunk_t *k = media_cache_plan(&e, 8192, 1808, &foff, &flen);
    CHECK(k && foff == 8192 && flen == 1808);
    CHECK(media_cache_plan(&e, 20000, 512, &foff, &flen) == NULL); /* past the end */

    /* A read bigger than a chunk goes to the source as it is. */
    media_cache_t s = make(1, 4096, 1 << 20);
    CHECK(media_cache_plan(&s, 0, 8192, &foff, &flen) == NULL);

    /* No read-ahead: never a chunk. */
    media_cache_t z = make(1, 0, 1 << 20);
    CHECK(media_cache_plan(&z, 0, 512, &foff, &flen) == NULL);

    /* Two chunks: the one read from longest ago is replaced. */
    media_cache_t two = make(2, 8192, 1 << 20);
    source_reads = 0;
    media_read(&two, 0, buf, 4096);         /* fills A at 0 */
    media_read(&two, 4096, buf, 4096);      /* from A */
    media_read(&two, 100000, buf, 4096);    /* jump, no fill */
    media_read(&two, 104096, buf, 4096);    /* fills B at 104096 */
    media_read(&two, 0, buf, 4096);         /* from A: A is newer now */
    CHECK(source_reads == 3);
    media_read(&two, 200000, buf, 4096);    /* jump */
    media_read(&two, 204096, buf, 4096);    /* fills over B, the older */
    CHECK(two.chunks[0].off == 0 && two.chunks[1].off == 204096);
    CHECK(right(buf, 204096, 4096));

    /* A write drops everything. */
    media_cache_drop(&two);
    CHECK(!media_cache_get(&two, 0, buf, 512));

    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("media cache: all passed\n");
    return 0;
}
