/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See sps_patch.h. A line-for-line port of web/src/video/spsPatch.ts; the host
 * test checks both give the same bytes.
 */
#include "sps_patch.h"

#include <stdbool.h>
#include <string.h>

/* An SPS is a few dozen bytes; this is plenty. */
#define SPS_MAX 128

typedef struct {
    const uint8_t *b;
    size_t len; /* bytes */
    size_t pos; /* bits */
    bool bad;
} bits_t;

static uint32_t rd_u(bits_t *r, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if ((r->pos >> 3) >= r->len) {
            r->bad = true;
            return 0;
        }
        v = v << 1 | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

static uint32_t rd_ue(bits_t *r)
{
    int zeros = 0;
    while (!r->bad && rd_u(r, 1) == 0) {
        if (++zeros > 31) {
            r->bad = true;
            return 0;
        }
    }
    return zeros ? ((1u << zeros) - 1u + rd_u(r, zeros)) : 0;
}

typedef struct {
    uint8_t b[SPS_MAX];
    size_t pos; /* bits */
    bool bad;
} writer_t;

static void wr_u(writer_t *w, int n, uint32_t v)
{
    for (int i = n - 1; i >= 0; i--) {
        if ((w->pos >> 3) >= sizeof(w->b)) {
            w->bad = true;
            return;
        }
        if ((v >> i) & 1u) {
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
    wr_u(w, len, 0);
    wr_u(w, len + 1, v + 1u);
}

/* Emulation prevention bytes out (unescape) and back in (escape). */
static size_t unescape(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int zeros = 0;
    for (size_t i = 0; i < len; i++) {
        if (zeros >= 2 && in[i] == 3) {
            zeros = 0;
            continue;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return n;
}

static size_t escape(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int zeros = 0;
    for (size_t i = 0; i < len; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            if (n >= cap) {
                return 0;
            }
            out[n++] = 3;
            zeros = 0;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return n;
}

size_t sps_patch(const uint8_t *nal, size_t len, uint8_t *out, size_t cap)
{
    if (len < 2) {
        return 0;
    }
    uint8_t rbsp[SPS_MAX];
    const size_t rlen = unescape(nal + 1, len - 1, rbsp, sizeof(rbsp));
    if (!rlen) {
        return 0;
    }
    bits_t r = {.b = rbsp, .len = rlen};
    const uint32_t profile = rd_u(&r, 8);
    rd_u(&r, 16); /* constraints, level */
    rd_ue(&r);    /* sps id */
    static const uint8_t high[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    for (size_t i = 0; i < sizeof(high); i++) {
        if (profile == high[i]) {
            return 0;
        }
    }
    rd_ue(&r); /* log2_max_frame_num - 4 */
    const uint32_t poc = rd_ue(&r);
    if (poc == 0) {
        rd_ue(&r);
    } else if (poc == 1) {
        rd_u(&r, 1);
        rd_ue(&r); /* se(v): only skipped, so read as ue */
        rd_ue(&r);
        const uint32_t n = rd_ue(&r);
        for (uint32_t i = 0; i < n && !r.bad; i++) {
            rd_ue(&r);
        }
    }
    rd_ue(&r); /* max_num_ref_frames */
    rd_u(&r, 1);
    rd_ue(&r);
    rd_ue(&r); /* size in macroblocks */
    if (!rd_u(&r, 1)) {
        rd_u(&r, 1); /* frame_mbs_only, else mbaff */
    }
    rd_u(&r, 1); /* direct_8x8 */
    if (rd_u(&r, 1)) {
        rd_ue(&r);
        rd_ue(&r);
        rd_ue(&r);
        rd_ue(&r);
    }
    if (!rd_u(&r, 1)) {
        return 0; /* no VUI: not this encoder's layout */
    }
    if (rd_u(&r, 1) && rd_u(&r, 8) == 255) {
        rd_u(&r, 32); /* aspect ratio */
    }
    if (rd_u(&r, 1)) {
        rd_u(&r, 1); /* overscan */
    }
    if (rd_u(&r, 1)) {
        rd_u(&r, 4);
        if (rd_u(&r, 1)) {
            rd_u(&r, 24);
        }
    }
    if (rd_u(&r, 1)) {
        rd_ue(&r);
        rd_ue(&r);
    }
    if (rd_u(&r, 1)) {
        rd_u(&r, 32); /* timing: 65 bits */
        rd_u(&r, 32);
        rd_u(&r, 1);
    }
    if (rd_u(&r, 1) || rd_u(&r, 1)) {
        return 0; /* HRD parameters: not handled */
    }
    rd_u(&r, 1); /* pic_struct_present */
    const size_t flag_at = r.pos;
    if (rd_u(&r, 1) || r.bad) {
        return 0; /* already restricted, or cut short */
    }

    writer_t w = {0};
    bits_t copy = {.b = rbsp, .len = rlen};
    for (size_t i = 0; i < flag_at; i++) {
        wr_u(&w, 1, rd_u(&copy, 1));
    }
    wr_u(&w, 1, 1); /* bitstream_restriction_flag */
    wr_u(&w, 1, 1); /* motion_vectors_over_pic_boundaries */
    wr_ue(&w, 0);   /* max_bytes_per_pic_denom: no limit */
    wr_ue(&w, 0);   /* max_bits_per_mb_denom: no limit */
    wr_ue(&w, 16);  /* log2_max_mv_length_horizontal */
    wr_ue(&w, 16);  /* log2_max_mv_length_vertical */
    wr_ue(&w, 0);   /* max_num_reorder_frames */
    wr_ue(&w, 1);   /* max_dec_frame_buffering */
    wr_u(&w, 1, 1); /* rbsp stop bit */
    if (w.bad || cap < 1) {
        return 0;
    }
    const size_t body = escape(w.b, (w.pos + 7) / 8, out + 1, cap - 1);
    if (!body) {
        return 0;
    }
    out[0] = nal[0];
    return body + 1;
}

size_t sps_patch_au(const uint8_t *au, size_t len, uint8_t *out, size_t cap)
{
    /* Find the SPS: from its start code to the next one. */
    size_t begin = 0, end = len;
    bool found = false;
    for (size_t i = 0; i + 3 < len; i++) {
        if (au[i] != 0 || au[i + 1] != 0) {
            continue;
        }
        size_t at;
        if (au[i + 2] == 1) {
            at = i + 3;
        } else if (au[i + 2] == 0 && au[i + 3] == 1) {
            at = i + 4;
        } else {
            continue;
        }
        if (found) {
            end = i; /* the next start code ends the SPS */
            break;
        }
        if (at < len && (au[at] & 0x1f) == 7) {
            begin = at;
            found = true;
        }
        i = at - 1;
    }
    if (!found) {
        return 0;
    }
    uint8_t sps[SPS_MAX];
    const size_t n = sps_patch(au + begin, end - begin, sps, sizeof(sps));
    const size_t out_len = len - (end - begin) + n;
    if (!n || out_len > cap) {
        return 0;
    }
    memcpy(out, au, begin);
    memcpy(out + begin, sps, n);
    memcpy(out + begin + n, au + end, len - end);
    return out_len;
}
