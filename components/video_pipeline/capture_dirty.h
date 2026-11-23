/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which parts of the picture changed, so the rev 1.x H.264 path converts only
 * those into its YUV buffers. Plain C over a pixel buffer, tested on a host.
 *
 * The frame is cut into stripes 16 rows high (a macroblock row) and segments
 * 64 pixels wide. Each frame, every other row is hashed per segment, odd rows
 * one frame and even rows the next, and compared with that row's hash from two
 * frames ago. A change marks its stripe and segments in every YUV buffer; a
 * buffer about to be filled asks which rectangles it is missing and converts
 * only those. A change is seen one frame late at most, and never lost: a row is
 * always compared with its own last hash, not with what a buffer holds.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIRTY_STRIPE_ROWS 16u
#define DIRTY_SEG_PX 64u
#define DIRTY_MAX_SLOTS 2

typedef struct {
    uint16_t x, y, w, h;
} dirty_rect_t;

typedef struct {
    uint32_t max_w, max_h;   /* what the memory below was sized for */
    uint32_t w, h;           /* the picture now; 0 = not set */
    uint32_t segs, stripes;  /* for w x h */
    uint32_t *hash;          /* max_h rows x max segs, one hash each */
    uint8_t *lo;             /* [slot][stripe]: first dirty segment, 0xff = clean */
    uint8_t *hi;             /* [slot][stripe]: last dirty segment */
    uint8_t parity;          /* rows hashed next: 0 even, 1 odd */
    bool known[2];           /* the hashes of that parity are from a real frame */
} dirty_t;

/* Bytes of memory dirty_init() wants for a picture up to max_w x max_h. */
size_t dirty_mem_bytes(uint32_t max_w, uint32_t max_h);

/* Use @p mem (dirty_mem_bytes() long, 4-byte aligned) for pictures up to
 * max_w x max_h. Starts with no picture set. */
void dirty_init(dirty_t *d, void *mem, uint32_t max_w, uint32_t max_h);

/* A new picture size, or buffers whose content is unknown: every buffer is
 * wholly dirty and no hash is trusted. False when the size cannot be tracked
 * (too big, or odd) - then every fill is a whole one. */
bool dirty_reset(dirty_t *d, uint32_t w, uint32_t h);

/* Hash this frame's rows of the current parity and mark what changed.
 * @p px is the frame, @p stride bytes a row, @p bpp bytes a pixel. */
void dirty_scan(dirty_t *d, const uint8_t *px, uint32_t stride, uint32_t bpp);

/* Buffer @p slot needs everything. */
void dirty_mark_all(dirty_t *d, int slot);

/*
 * The rectangles buffer @p slot is missing, at most @p max of them, and forget
 * them: the caller converts them now. Returns how many (0: nothing changed),
 * or -1 when no picture is tracked and the caller must convert all of it.
 * When they would cover more than half the picture, or not fit in @p max, it
 * is one rectangle, the whole picture. Every edge is even, as YUV420 needs.
 */
int dirty_take(dirty_t *d, int slot, dirty_rect_t *out, int max);
