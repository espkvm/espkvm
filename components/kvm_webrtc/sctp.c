/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See sctp.h. Everything here comes off the network, through DTLS but still
 * from the browser: every length is checked before it is read.
 */
#include "sctp.h"

#include <string.h>

enum {
    CH_DATA = 0,
    CH_INIT = 1,
    CH_INIT_ACK = 2,
    CH_SACK = 3,
    CH_HEARTBEAT = 4,
    CH_HEARTBEAT_ACK = 5,
    CH_ABORT = 6,
    CH_SHUTDOWN = 7,
    CH_SHUTDOWN_ACK = 8,
    CH_COOKIE_ECHO = 10,
    CH_COOKIE_ACK = 11,
    CH_FORWARD_TSN = 192,
};

#define HDR_LEN 12
#define RWND 65536u
/* Inbound streams the device takes; the console uses one. */
#define MAX_STREAMS 16u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint32_t sctp_crc32c(const uint8_t *p, size_t len)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0x82f63b78u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

void sctp_init(sctp_assoc_t *a, uint16_t local_port, uint32_t vtag, uint32_t tsn)
{
    memset(a, 0, sizeof(*a));
    a->local_port = local_port;
    a->my_vtag = vtag ? vtag : 1;
    a->my_tsn = tsn;
}

/* The answer being built: the common header, then chunks. */
typedef struct {
    uint8_t *b;
    size_t cap, len;
    bool full;
} out_t;

static uint8_t *chunk(out_t *o, uint8_t type, uint8_t flags, size_t value_len)
{
    const size_t n = 4 + value_len;
    const size_t padded = (n + 3) & ~(size_t)3;
    if (o->full || o->len + padded > o->cap) {
        o->full = true;
        return NULL;
    }
    uint8_t *c = o->b + o->len;
    memset(c, 0, padded);
    c[0] = type;
    c[1] = flags;
    wr16(c + 2, (uint16_t)n);
    o->len += padded;
    return c + 4;
}

/* a - b for TSNs, which wrap. */
static int32_t tsn_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

/* Move cum_tsn over every TSN that has arrived in a row. */
static void advance(sctp_assoc_t *a)
{
    while (a->ahead & 1u) {
        a->cum_tsn++;
        a->ahead >>= 1;
    }
}

/* Note @p tsn as arrived; false when it already had (or is too far ahead to
   keep track of, which a peer that waits for SACKs never reaches). */
static bool take_tsn(sctp_assoc_t *a, uint32_t tsn)
{
    const int32_t d = tsn_diff(tsn, a->cum_tsn);
    if (d <= 0 || d > 32) {
        return false;
    }
    const uint32_t bit = 1u << (d - 1);
    if (a->ahead & bit) {
        return false;
    }
    a->ahead |= bit;
    advance(a);
    return true;
}

static void put_sack(sctp_assoc_t *a, out_t *o)
{
    /* Gap blocks for what arrived past cum_tsn, offsets from it. */
    uint16_t gaps[16][2];
    int n = 0;
    for (int i = 0; i < 32 && n < 16;) {
        if (!(a->ahead & (1u << i))) {
            i++;
            continue;
        }
        const int start = i;
        while (i < 32 && (a->ahead & (1u << i))) {
            i++;
        }
        gaps[n][0] = (uint16_t)(start + 1);
        gaps[n][1] = (uint16_t)i;
        n++;
    }
    uint8_t *v = chunk(o, CH_SACK, 0, 12 + (size_t)n * 4);
    if (!v) {
        return;
    }
    a->unacked = 0;
    wr32(v, a->cum_tsn);
    wr32(v + 4, RWND);
    wr16(v + 8, (uint16_t)n);
    wr16(v + 10, 0);
    for (int i = 0; i < n; i++) {
        wr16(v + 12 + i * 4, gaps[i][0]);
        wr16(v + 14 + i * 4, gaps[i][1]);
    }
}

static void put_init_ack(const sctp_assoc_t *a, out_t *o)
{
    /* Fixed part, then: a state cookie (the association is held here, so it
       only has to come back), Forward-TSN-Supported, and the extension list
       naming FORWARD TSN - without them the browser will not abandon a
       message and the channel would not be the unreliable one it asked for. */
    const size_t cookie = 4 + 8, fwd = 4, ext = 4 + 1;
    const size_t ext_padded = (ext + 3) & ~(size_t)3;
    uint8_t *v = chunk(o, CH_INIT_ACK, 0, 16 + cookie + fwd + ext_padded);
    if (!v) {
        return;
    }
    wr32(v, a->my_vtag);
    wr32(v + 4, RWND);
    wr16(v + 8, MAX_STREAMS); /* outbound: unused, but at least one */
    wr16(v + 10, MAX_STREAMS);
    wr32(v + 12, a->my_tsn);
    uint8_t *p = v + 16;
    wr16(p, 7); /* state cookie */
    wr16(p + 2, (uint16_t)cookie);
    wr32(p + 4, a->my_vtag);
    wr32(p + 8, a->peer_vtag);
    p += cookie;
    wr16(p, 0xc000); /* Forward-TSN-Supported */
    wr16(p + 2, (uint16_t)fwd);
    p += fwd;
    wr16(p, 0x8008); /* supported extensions */
    wr16(p + 2, (uint16_t)ext);
    p[4] = CH_FORWARD_TSN;
}

static size_t finish(const sctp_assoc_t *a, out_t *o);

size_t sctp_input(sctp_assoc_t *a, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                  sctp_msg_fn fn, void *ctx)
{
    if (len < HDR_LEN + 4 || cap < HDR_LEN) {
        return 0;
    }
    const uint32_t vtag = rd32(in + 4);
    out_t o = {.b = out, .cap = cap, .len = HDR_LEN};
    bool sack = false;

    size_t off = HDR_LEN;
    while (off + 4 <= len) {
        const uint8_t type = in[off];
        const uint16_t clen = rd16(in + off + 2);
        if (clen < 4 || off + clen > len) {
            break;
        }
        const uint8_t *v = in + off + 4;
        const size_t vlen = clen - 4u;
        if (type == CH_INIT) {
            /* Alone in its packet, with tag 0. A repeated INIT gets the same
               answer: the browser resends it until an INIT ACK arrives. */
            if (vtag != 0 || vlen < 16 || rd32(v) == 0) {
                return 0;
            }
            a->peer_port = rd16(in);
            a->peer_vtag = rd32(v);
            a->cum_tsn = rd32(v + 12) - 1u;
            a->ahead = 0;
            a->have_tsn = true;
            a->up = false;
            put_init_ack(a, &o);
            break;
        }
        if (vtag != a->my_vtag) {
            return 0; /* not this association */
        }
        switch (type) {
        case CH_COOKIE_ECHO:
            a->up = true;
            (void)chunk(&o, CH_COOKIE_ACK, 0, 0);
            break;
        case CH_DATA:
            if (a->have_tsn && vlen >= 12) {
                const uint8_t flags = in[off + 1];
                const uint32_t tsn = rd32(v);
                const uint16_t stream = rd16(v + 4);
                const uint32_t ppid = rd32(v + 8);
                const bool whole = (flags & 0x03) == 0x03; /* B and E */
                if (take_tsn(a, tsn) && whole && fn) {
                    fn(ctx, stream, ppid, v + 12, vlen - 12);
                }
                /* Every few packets, or at once with a gap to report. */
                if (++a->unacked >= SCTP_SACK_EVERY || a->ahead) {
                    sack = true;
                }
            }
            break;
        case CH_FORWARD_TSN:
            if (a->have_tsn && vlen >= 4) {
                const int32_t d = tsn_diff(rd32(v), a->cum_tsn);
                if (d > 32) {
                    a->cum_tsn += (uint32_t)d;
                    a->ahead = 0;
                } else if (d > 0) {
                    a->cum_tsn += (uint32_t)d;
                    a->ahead = d == 32 ? 0 : a->ahead >> d;
                }
                advance(a);
                sack = true;
            }
            break;
        case CH_HEARTBEAT: {
            uint8_t *r = chunk(&o, CH_HEARTBEAT_ACK, 0, vlen);
            if (r) {
                memcpy(r, v, vlen);
            }
            break;
        }
        case CH_SHUTDOWN:
            a->up = false;
            (void)chunk(&o, CH_SHUTDOWN_ACK, 0, 0);
            break;
        case CH_ABORT:
            a->up = false;
            return 0;
        default:
            /* SACK (nothing of ours is in flight), ERROR, and anything this
               side does not speak. */
            break;
        }
        off += (clen + 3u) & ~3u;
    }
    if (sack) {
        put_sack(a, &o);
    }
    return finish(a, &o);
}

size_t sctp_sack(sctp_assoc_t *a, uint8_t *out, size_t cap)
{
    if (!a->up || !a->unacked || cap < HDR_LEN) {
        return 0;
    }
    out_t o = {.b = out, .cap = cap, .len = HDR_LEN};
    put_sack(a, &o);
    return finish(a, &o);
}

/* The common header and the checksum, once the chunks are in. */
static size_t finish(const sctp_assoc_t *a, out_t *o)
{
    uint8_t *out = o->b;
    if (o->full || o->len == HDR_LEN) {
        return 0;
    }
    wr16(out, a->local_port);
    wr16(out + 2, a->peer_port);
    wr32(out + 4, a->peer_vtag);
    wr32(out + 8, 0);
    /* The checksum goes in little-endian, unlike every other field. */
    const uint32_t crc = sctp_crc32c(out, o->len);
    out[8] = (uint8_t)crc;
    out[9] = (uint8_t)(crc >> 8);
    out[10] = (uint8_t)(crc >> 16);
    out[11] = (uint8_t)(crc >> 24);
    return o->len;
}
