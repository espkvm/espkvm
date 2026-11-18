/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See rtp_h264.h.
 */
#include "rtp_h264.h"

#include <stdbool.h>
#include <string.h>

/* The next start code at or after @p from; sets *sc_len to 3 or 4. */
static size_t next_start(const uint8_t *p, size_t len, size_t from, size_t *sc_len)
{
    for (size_t i = from; i + 3 <= len; i++) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 4 <= len && p[i + 2] == 0 && p[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    *sc_len = 0;
    return len;
}

/* Writes the header (and the extension, if on) and returns its length. */
static size_t header(rtp_h264_t *st, uint8_t *b, bool marker, uint32_t ts)
{
    b[0] = st->playout_ext ? 0x90 : 0x80; /* version 2, X when extended */
    b[1] = (uint8_t)((marker ? 0x80 : 0) | (st->pt & 0x7f));
    b[2] = (uint8_t)(st->seq >> 8);
    b[3] = (uint8_t)st->seq;
    b[4] = (uint8_t)(ts >> 24);
    b[5] = (uint8_t)(ts >> 16);
    b[6] = (uint8_t)(ts >> 8);
    b[7] = (uint8_t)ts;
    b[8] = (uint8_t)(st->ssrc >> 24);
    b[9] = (uint8_t)(st->ssrc >> 16);
    b[10] = (uint8_t)(st->ssrc >> 8);
    b[11] = (uint8_t)st->ssrc;
    st->seq++;
    if (!st->playout_ext) {
        return RTP_HDR_LEN;
    }
    b[RTP_HDR_LEN] = 0xbe; /* one-byte-header extensions */
    b[RTP_HDR_LEN + 1] = 0xde;
    b[RTP_HDR_LEN + 2] = 0;  /* one 32-bit word follows */
    b[RTP_HDR_LEN + 3] = 1;
    b[RTP_HDR_LEN + 4] = (uint8_t)(st->playout_ext << 4 | 2); /* id, 3 bytes */
    b[RTP_HDR_LEN + 5] = 0;                                   /* min 0, max 0 */
    b[RTP_HDR_LEN + 6] = 0;
    b[RTP_HDR_LEN + 7] = 0;
    return RTP_HDR_LEN + RTP_EXT_LEN;
}

int rtp_h264_send(rtp_h264_t *st, const uint8_t *au, size_t len, uint32_t ts, uint8_t *buf,
                  rtp_emit_fn emit, void *ctx)
{
    if (st->max_payload < 3) {
        return -1;
    }
    int count = 0;
    size_t sc;
    size_t at = next_start(au, len, 0, &sc);
    while (at < len) {
        const size_t nal = at + sc;
        size_t sc2;
        size_t end = next_start(au, len, nal, &sc2);
        /* Trailing zeros belong to the next start code, not to this unit. */
        size_t nal_end = end;
        while (nal_end > nal && au[nal_end - 1] == 0 && end < len) {
            nal_end--;
        }
        const size_t nal_len = nal_end - nal;
        const bool last = end >= len;
        at = end;
        sc = sc2;
        if (nal_len == 0) {
            continue;
        }
        const uint8_t nal_hdr = au[nal];
        if ((nal_hdr & 0x1f) == 9) {
            continue; /* access unit delimiter: says nothing RTP does not */
        }
        if (nal_len <= st->max_payload) {
            const size_t h = header(st, buf, last, ts);
            memcpy(buf + h, au + nal, nal_len);
            count++;
            if (emit(ctx, buf, h + nal_len) != 0) {
                return -1;
            }
            continue;
        }
        /* FU-A: the NAL header is split into an indicator and a fragment
           header, and its own byte is not repeated in the fragments. */
        const uint8_t indicator = (uint8_t)((nal_hdr & 0xe0) | 28);
        const size_t chunk = st->max_payload - 2;
        size_t off = nal + 1;
        const size_t stop = nal + nal_len;
        bool first = true;
        while (off < stop) {
            size_t n = stop - off;
            if (n > chunk) {
                n = chunk;
            }
            const bool end_frag = off + n >= stop;
            const size_t h = header(st, buf, last && end_frag, ts);
            buf[h] = indicator;
            buf[h + 1] = (uint8_t)((first ? 0x80 : 0) | (end_frag ? 0x40 : 0) | (nal_hdr & 0x1f));
            memcpy(buf + h + 2, au + off, n);
            count++;
            if (emit(ctx, buf, h + 2 + n) != 0) {
                return -1;
            }
            off += n;
            first = false;
        }
    }
    return count;
}
