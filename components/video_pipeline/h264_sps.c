/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The SPS fix described in h264_sps.h. Plain C, so it runs on a host test too.
 */
#include "h264_sps.h"

#include <stdbool.h>
#include <string.h>

#define SPS_MAX 96 /* this encoder's SPS is about 25 bytes */

typedef struct {
    const uint8_t *b;
    size_t len;
    size_t pos; /* in bits */
    bool bad;
} reader_t;

static uint32_t rd(reader_t *r, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if ((r->pos >> 3) >= r->len) {
            r->bad = true;
            return 0;
        }
        v = (v << 1) | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

static uint32_t rd_ue(reader_t *r)
{
    int zeros = 0;
    while (!r->bad && rd(r, 1) == 0) {
        if (++zeros > 31) {
            r->bad = true;
            return 0;
        }
    }
    return zeros ? ((1u << zeros) - 1u + rd(r, zeros)) : 0;
}

typedef struct {
    uint8_t b[SPS_MAX];
    size_t pos; /* in bits */
} writer_t;

static void wr(writer_t *w, int n, uint32_t v)
{
    for (int i = n - 1; i >= 0; i--) {
        if ((w->pos >> 3) < sizeof(w->b) && ((v >> i) & 1u)) {
            w->b[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7));
        }
        w->pos++;
    }
}

static void wr_ue(writer_t *w, uint32_t v)
{
    int len = 0;
    while ((v + 1u) >> (len + 1)) {
        len++;
    }
    wr(w, len, 0);
    wr(w, len + 1, v + 1u);
}

/* Emulation prevention off: wire bytes -> RBSP. */
static size_t unescape(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n && o < cap; i++) {
        if (zeros >= 2 && in[i] == 3) {
            zeros = 0;
            continue;
        }
        out[o++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

/* And back on. */
static size_t escape(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            if (o >= cap) {
                return 0;
            }
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) {
            return 0;
        }
        out[o++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

/* The patched SPS payload (after the NAL header byte), escaped, into @p out;
 * 0 when it should be left alone. */
static size_t patch(const uint8_t *nal, size_t n, uint8_t *out, size_t cap)
{
    uint8_t rbsp[SPS_MAX];
    const size_t rlen = unescape(nal, n, rbsp, sizeof(rbsp));
    reader_t r = {.b = rbsp, .len = rlen};
    const uint32_t profile = rd(&r, 8);
    rd(&r, 16);
    rd_ue(&r);
    if (profile != 66 && profile != 77 && profile != 88) {
        return 0; /* High profiles carry chroma fields this does not read */
    }
    rd_ue(&r);
    const uint32_t poc = rd_ue(&r);
    if (poc == 0) {
        rd_ue(&r);
    } else if (poc == 1) {
        return 0;
    }
    rd_ue(&r);
    rd(&r, 1);
    rd_ue(&r);
    rd_ue(&r);
    if (!rd(&r, 1)) {
        rd(&r, 1);
    }
    rd(&r, 1);
    if (rd(&r, 1)) {
        rd_ue(&r);
        rd_ue(&r);
        rd_ue(&r);
        rd_ue(&r);
    }
    if (!rd(&r, 1)) {
        return 0; /* no VUI: not this encoder's layout */
    }
    if (rd(&r, 1) && rd(&r, 8) == 255) {
        rd(&r, 32);
    }
    if (rd(&r, 1)) {
        rd(&r, 1);
    }
    if (rd(&r, 1)) {
        rd(&r, 4);
        if (rd(&r, 1)) {
            rd(&r, 24);
        }
    }
    if (rd(&r, 1)) {
        rd_ue(&r);
        rd_ue(&r);
    }
    if (rd(&r, 1)) {
        rd(&r, 32);
        rd(&r, 32);
        rd(&r, 1);
    }
    if (rd(&r, 1) || rd(&r, 1)) {
        return 0; /* HRD parameters */
    }
    rd(&r, 1);
    const size_t flag_at = r.pos;
    if (rd(&r, 1) || r.bad) {
        return 0; /* already there, or a short read */
    }

    writer_t w = {0};
    reader_t c = {.b = rbsp, .len = rlen};
    for (size_t i = 0; i < flag_at; i++) {
        wr(&w, 1, rd(&c, 1));
    }
    wr(&w, 1, 1);   /* bitstream_restriction_flag */
    wr(&w, 1, 1);   /* motion_vectors_over_pic_boundaries */
    wr_ue(&w, 0);   /* max_bytes_per_pic_denom */
    wr_ue(&w, 0);   /* max_bits_per_mb_denom */
    wr_ue(&w, 16);  /* log2_max_mv_length_horizontal */
    wr_ue(&w, 16);  /* log2_max_mv_length_vertical */
    wr_ue(&w, 0);   /* max_num_reorder_frames */
    wr_ue(&w, 1);   /* max_dec_frame_buffering */
    wr(&w, 1, 1);   /* stop bit */
    if ((w.pos + 7) / 8 > sizeof(w.b)) {
        return 0;
    }
    return escape(w.b, (w.pos + 7) / 8, out, cap);
}

size_t h264_sps_add_restriction(uint8_t *buf, size_t len, size_t cap)
{
    /* Find the SPS NAL: after a start code, type 7, up to the next start code. */
    size_t begin = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && (buf[i + 3] & 0x1f) == 7) {
            begin = i + 4; /* past the NAL header byte */
            break;
        }
    }
    if (!begin) {
        return len;
    }
    size_t end = len;
    for (size_t i = begin; i + 2 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && (buf[i + 2] == 1 || (buf[i + 2] == 0 && i + 3 < len && buf[i + 3] == 1))) {
            end = i;
            break;
        }
    }
    uint8_t fixed[SPS_MAX];
    const size_t n = patch(buf + begin, end - begin, fixed, sizeof(fixed));
    if (!n) {
        return len;
    }
    const size_t old = end - begin;
    if (len - old + n > cap) {
        return len;
    }
    memmove(buf + begin + n, buf + end, len - end);
    memcpy(buf + begin, fixed, n);
    return len - old + n;
}
