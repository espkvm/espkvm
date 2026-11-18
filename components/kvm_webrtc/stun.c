/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See stun.h. Plain C apart from the HMAC, which comes through wr_crypto.h.
 * Every length here comes off the network, so every one is checked against
 * the packet before it is used.
 */
#include "stun.h"

#include <string.h>

#define MAGIC 0x2112A442u
#define FP_XOR 0x5354554Eu

#define T_BINDING_REQ 0x0001
#define T_BINDING_OK 0x0101

#define A_USERNAME 0x0006
#define A_MESSAGE_INTEGRITY 0x0008
#define A_XOR_MAPPED_ADDRESS 0x0020
#define A_USE_CANDIDATE 0x0025
#define A_FINGERPRINT 0x8028

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

static uint32_t crc_more(uint32_t c, const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return c;
}

uint32_t stun_crc32(const uint8_t *p, size_t len)
{
    return ~crc_more(0xFFFFFFFFu, p, len);
}

bool stun_is(const uint8_t *p, size_t len)
{
    return len >= STUN_HDR_LEN && p[0] < 4 && rd32(p + 4) == MAGIC;
}

/*
 * HMAC over the message up to @p upto, with the header's length field
 * rewritten to end just after the MESSAGE-INTEGRITY attribute - which is what
 * the sender signed, whatever follows it (RFC 5389 15.4).
 */
static int integrity(const uint8_t *p, size_t upto, wr_hmac_t *pwd, uint8_t mac[20])
{
    uint8_t hdr[STUN_HDR_LEN];
    memcpy(hdr, p, STUN_HDR_LEN);
    wr16(hdr + 2, (uint16_t)(upto - STUN_HDR_LEN + 24));
    const uint8_t *parts[2] = {hdr, p + STUN_HDR_LEN};
    const size_t lens[2] = {STUN_HDR_LEN, upto - STUN_HDR_LEN};
    return wr_hmac_sha1(pwd, parts, lens, 2, mac);
}

/* A constant-time compare: the MAC is a secret until it matches. */
static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

int stun_check_binding(const uint8_t *p, size_t len, const char *local_ufrag, wr_hmac_t *pwd,
                       stun_req_t *out)
{
    if (!stun_is(p, len) || rd16(p) != T_BINDING_REQ) {
        return -1;
    }
    const size_t body = rd16(p + 2);
    if ((body & 3) || STUN_HDR_LEN + body > len) {
        return -1;
    }
    const size_t end = STUN_HDR_LEN + body;
    const size_t ufrag_len = strlen(local_ufrag);

    bool user_ok = false, mi_ok = false, use_cand = false;
    size_t off = STUN_HDR_LEN;
    while (off + 4 <= end) {
        const uint16_t type = rd16(p + off);
        const size_t alen = rd16(p + off + 2);
        const size_t padded = (alen + 3) & ~(size_t)3;
        if (off + 4 + padded > end) {
            return -1;
        }
        const uint8_t *v = p + off + 4;
        if (mi_ok && type != A_FINGERPRINT) {
            return -1; /* only FINGERPRINT may follow MESSAGE-INTEGRITY */
        }
        switch (type) {
        case A_USERNAME:
            /* "ours:theirs" - only our half is ours to check. */
            user_ok = alen > ufrag_len && memcmp(v, local_ufrag, ufrag_len) == 0 &&
                      v[ufrag_len] == ':';
            break;
        case A_USE_CANDIDATE:
            use_cand = true;
            break;
        case A_MESSAGE_INTEGRITY: {
            uint8_t mac[20];
            if (alen != 20 || integrity(p, off, pwd, mac) != 0 || !same(mac, v, 20)) {
                return -1;
            }
            mi_ok = true;
            break;
        }
        case A_FINGERPRINT: {
            if (alen != 4) {
                return -1;
            }
            /* Over the message before it, the length field covering it. */
            uint8_t hdr[STUN_HDR_LEN];
            memcpy(hdr, p, STUN_HDR_LEN);
            wr16(hdr + 2, (uint16_t)(off - STUN_HDR_LEN + 8));
            const uint32_t c = ~crc_more(crc_more(0xFFFFFFFFu, hdr, STUN_HDR_LEN),
                                         p + STUN_HDR_LEN, off - STUN_HDR_LEN);
            if ((c ^ FP_XOR) != rd32(v)) {
                return -1;
            }
            break;
        }
        default:
            break; /* PRIORITY, ICE-CONTROLLING and the rest are not ours to act on */
        }
        off += 4 + padded;
    }
    if (!user_ok || !mi_ok) {
        return -1;
    }
    memcpy(out->txid, p + 8, 12);
    out->use_candidate = use_cand;
    return 0;
}

int stun_local_ufrag(const uint8_t *p, size_t len, char *out, size_t cap)
{
    if (!stun_is(p, len) || rd16(p) != T_BINDING_REQ) {
        return -1;
    }
    const size_t body = rd16(p + 2);
    if ((body & 3) || STUN_HDR_LEN + body > len) {
        return -1;
    }
    const size_t end = STUN_HDR_LEN + body;
    size_t off = STUN_HDR_LEN;
    while (off + 4 <= end) {
        const size_t alen = rd16(p + off + 2);
        if (off + 4 + ((alen + 3) & ~(size_t)3) > end) {
            return -1;
        }
        if (rd16(p + off) == A_USERNAME) {
            const uint8_t *v = p + off + 4;
            const uint8_t *colon = memchr(v, ':', alen);
            const size_t n = colon ? (size_t)(colon - v) : 0;
            if (n == 0 || n >= cap) {
                return -1;
            }
            memcpy(out, v, n);
            out[n] = 0;
            return 0;
        }
        off += 4 + ((alen + 3) & ~(size_t)3);
    }
    return -1;
}

size_t stun_build_success(const uint8_t txid[12], uint32_t ip_be, uint16_t port_be, wr_hmac_t *pwd,
                          uint8_t *out, size_t cap)
{
    if (cap < STUN_RESPONSE_LEN) {
        return 0;
    }
    uint8_t *p = out;
    wr16(p, T_BINDING_OK);
    wr16(p + 2, STUN_RESPONSE_LEN - STUN_HDR_LEN);
    wr32(p + 4, MAGIC);
    memcpy(p + 8, txid, 12);

    /* XOR-MAPPED-ADDRESS: where the request came from, xored with the cookie. */
    uint8_t *a = p + STUN_HDR_LEN;
    wr16(a, A_XOR_MAPPED_ADDRESS);
    wr16(a + 2, 8);
    a[4] = 0;
    a[5] = 0x01; /* IPv4 */
    const uint8_t *port = (const uint8_t *)&port_be;
    const uint8_t *ip = (const uint8_t *)&ip_be;
    a[6] = port[0] ^ 0x21;
    a[7] = port[1] ^ 0x12;
    a[8] = ip[0] ^ 0x21;
    a[9] = ip[1] ^ 0x12;
    a[10] = ip[2] ^ 0xA4;
    a[11] = ip[3] ^ 0x42;

    const size_t mi_at = STUN_HDR_LEN + 12;
    uint8_t *m = p + mi_at;
    wr16(m, A_MESSAGE_INTEGRITY);
    wr16(m + 2, 20);
    if (integrity(p, mi_at, pwd, m + 4) != 0) {
        return 0;
    }

    const size_t fp_at = mi_at + 24;
    uint8_t *f = p + fp_at;
    wr16(f, A_FINGERPRINT);
    wr16(f + 2, 4);
    /* The length field already covers FINGERPRINT, as the CRC requires. */
    wr32(f + 4, stun_crc32(p, fp_at) ^ FP_XOR);
    return STUN_RESPONSE_LEN;
}
