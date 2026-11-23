/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See capture_dirty.h.
 */
#include "capture_dirty.h"

#include <string.h>

#define CLEAN 0xffu

static uint32_t segs_for(uint32_t w)
{
    return (w + DIRTY_SEG_PX - 1u) / DIRTY_SEG_PX;
}

static uint32_t stripes_for(uint32_t h)
{
    return (h + DIRTY_STRIPE_ROWS - 1u) / DIRTY_STRIPE_ROWS;
}

size_t dirty_mem_bytes(uint32_t max_w, uint32_t max_h)
{
    const size_t hashes = (size_t)max_h * segs_for(max_w) * sizeof(uint32_t);
    return hashes + 2u * DIRTY_MAX_SLOTS * stripes_for(max_h);
}

void dirty_init(dirty_t *d, void *mem, uint32_t max_w, uint32_t max_h)
{
    memset(d, 0, sizeof(*d));
    d->max_w = max_w;
    d->max_h = max_h;
    d->hash = mem;
    d->lo = (uint8_t *)mem + (size_t)max_h * segs_for(max_w) * sizeof(uint32_t);
    d->hi = d->lo + DIRTY_MAX_SLOTS * stripes_for(max_h);
}

static void mark(dirty_t *d, uint32_t stripe, uint32_t seg)
{
    for (int s = 0; s < DIRTY_MAX_SLOTS; s++) {
        uint8_t *lo = &d->lo[s * stripes_for(d->max_h) + stripe];
        uint8_t *hi = &d->hi[s * stripes_for(d->max_h) + stripe];
        if (*lo == CLEAN) {
            *lo = (uint8_t)seg;
            *hi = (uint8_t)seg;
        } else {
            if (seg < *lo) {
                *lo = (uint8_t)seg;
            }
            if (seg > *hi) {
                *hi = (uint8_t)seg;
            }
        }
    }
}

void dirty_mark_all(dirty_t *d, int slot)
{
    if (slot < 0 || slot >= DIRTY_MAX_SLOTS || !d->w) {
        return;
    }
    for (uint32_t st = 0; st < d->stripes; st++) {
        d->lo[slot * stripes_for(d->max_h) + st] = 0;
        d->hi[slot * stripes_for(d->max_h) + st] = (uint8_t)(d->segs - 1u);
    }
}

bool dirty_reset(dirty_t *d, uint32_t w, uint32_t h)
{
    d->w = 0;
    d->known[0] = d->known[1] = false;
    d->parity = 0;
    /* Segments are counted in a byte; YUV420 wants even edges. */
    if (!d->hash || w == 0 || h == 0 || w > d->max_w || h > d->max_h || (w & 1u) || (h & 1u) ||
        segs_for(w) >= CLEAN) {
        return false;
    }
    d->w = w;
    d->h = h;
    d->segs = segs_for(w);
    d->stripes = stripes_for(h);
    for (int s = 0; s < DIRTY_MAX_SLOTS; s++) {
        dirty_mark_all(d, s);
    }
    return true;
}

/* FNV-1a over a segment, four bytes a step where the row allows it. */
static uint32_t seg_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i = 0;
    if (((uintptr_t)p & 3u) == 0) {
        const uint32_t *w = (const uint32_t *)(const void *)p;
        for (; i + 4u <= n; i += 4u) {
            h = (h ^ *w++) * 16777619u;
        }
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void dirty_scan(dirty_t *d, const uint8_t *px, uint32_t stride, uint32_t bpp)
{
    if (!d->w) {
        return;
    }
    const uint32_t p = d->parity;
    const bool known = d->known[p];
    const uint32_t seg_bytes = DIRTY_SEG_PX * bpp;
    const uint32_t row_bytes = d->w * bpp;
    const uint32_t per_row = segs_for(d->max_w);
    for (uint32_t y = p; y < d->h; y += 2u) {
        const uint8_t *row = px + (size_t)y * stride;
        uint32_t *hs = &d->hash[(size_t)y * per_row];
        for (uint32_t s = 0; s < d->segs; s++) {
            const uint32_t off = s * seg_bytes;
            const uint32_t n = off + seg_bytes <= row_bytes ? seg_bytes : row_bytes - off;
            const uint32_t h = seg_hash(row + off, n);
            if (known && h != hs[s]) {
                mark(d, y / DIRTY_STRIPE_ROWS, s);
            }
            hs[s] = h;
        }
    }
    d->known[p] = true;
    d->parity = (uint8_t)(p ^ 1u);
}

int dirty_take(dirty_t *d, int slot, dirty_rect_t *out, int max)
{
    if (slot < 0 || slot >= DIRTY_MAX_SLOTS || max < 1) {
        return 0;
    }
    if (!d->w) {
        return -1; /* not tracked */
    }
    uint8_t *lo = &d->lo[slot * stripes_for(d->max_h)];
    uint8_t *hi = &d->hi[slot * stripes_for(d->max_h)];
    int n = 0;
    uint64_t area = 0;
    bool whole = false;
    for (uint32_t st = 0; st < d->stripes;) {
        if (lo[st] == CLEAN) {
            st++;
            continue;
        }
        /* Neighbouring dirty stripes become one rectangle across all of them. */
        uint32_t a = lo[st], b = hi[st], end = st + 1u;
        while (end < d->stripes && lo[end] != CLEAN) {
            if (lo[end] < a) {
                a = lo[end];
            }
            if (hi[end] > b) {
                b = hi[end];
            }
            end++;
        }
        const uint32_t x0 = a * DIRTY_SEG_PX;
        uint32_t x1 = (b + 1u) * DIRTY_SEG_PX;
        if (x1 > d->w) {
            x1 = d->w;
        }
        const uint32_t y0 = st * DIRTY_STRIPE_ROWS;
        uint32_t y1 = end * DIRTY_STRIPE_ROWS;
        if (y1 > d->h) {
            y1 = d->h;
        }
        if (n == max) {
            whole = true;
        } else {
            out[n++] = (dirty_rect_t){(uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0)};
        }
        area += (uint64_t)(x1 - x0) * (y1 - y0);
        st = end;
    }
    memset(lo, CLEAN, d->stripes);
    if (whole || area * 2u > (uint64_t)d->w * d->h) {
        out[0] = (dirty_rect_t){0, 0, (uint16_t)d->w, (uint16_t)d->h};
        return 1;
    }
    return n;
}
