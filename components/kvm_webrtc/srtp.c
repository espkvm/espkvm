/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See srtp.h. Only what a sender of one video stream needs: RTP out, RTCP in
 * (the browser's loss reports and keyframe requests) and RTCP out.
 */
#include "srtp.h"

#include <string.h>

#define RTP_HDR_LEN 12

enum {
    LABEL_RTP_KEY = 0,
    LABEL_RTP_AUTH = 1,
    LABEL_RTP_SALT = 2,
    LABEL_RTCP_KEY = 3,
    LABEL_RTCP_AUTH = 4,
    LABEL_RTCP_SALT = 5,
};

#define AUTH_KEY_LEN 20

int srtp_kdf(const uint8_t master_key[SRTP_KEY_LEN], const uint8_t master_salt[SRTP_SALT_LEN],
             uint8_t label, uint8_t *out, size_t len)
{
    /* key_derivation_rate 0: x = master_salt XOR (label << 48), as a counter
       block x * 2^16. */
    uint8_t iv[16] = {0};
    memcpy(iv, master_salt, SRTP_SALT_LEN);
    iv[7] ^= label;
    wr_aes_t *a = wr_aes_new(master_key);
    if (!a) {
        return -1;
    }
    const int r = wr_aes_ctr(a, iv, NULL, out, len);
    wr_aes_free(a);
    return r;
}

int srtp_init(srtp_ctx_t *c, const uint8_t master_key[SRTP_KEY_LEN],
              const uint8_t master_salt[SRTP_SALT_LEN])
{
    memset(c, 0, sizeof(*c));
    uint8_t k[SRTP_KEY_LEN], a[AUTH_KEY_LEN];
    int r = 0;
    r |= srtp_kdf(master_key, master_salt, LABEL_RTP_KEY, k, sizeof(k));
    c->rtp_aes = wr_aes_new(k);
    r |= srtp_kdf(master_key, master_salt, LABEL_RTP_AUTH, a, sizeof(a));
    c->rtp_auth = wr_hmac_new(a, sizeof(a));
    r |= srtp_kdf(master_key, master_salt, LABEL_RTP_SALT, c->rtp_salt, SRTP_SALT_LEN);
    r |= srtp_kdf(master_key, master_salt, LABEL_RTCP_KEY, k, sizeof(k));
    c->rtcp_aes = wr_aes_new(k);
    r |= srtp_kdf(master_key, master_salt, LABEL_RTCP_AUTH, a, sizeof(a));
    c->rtcp_auth = wr_hmac_new(a, sizeof(a));
    r |= srtp_kdf(master_key, master_salt, LABEL_RTCP_SALT, c->rtcp_salt, SRTP_SALT_LEN);
    memset(k, 0, sizeof(k));
    memset(a, 0, sizeof(a));
    if (r || !c->rtp_aes || !c->rtp_auth || !c->rtcp_aes || !c->rtcp_auth) {
        srtp_free(c);
        return -1;
    }
    return 0;
}

void srtp_free(srtp_ctx_t *c)
{
    wr_aes_free(c->rtp_aes);
    wr_aes_free(c->rtcp_aes);
    wr_hmac_free(c->rtp_auth);
    wr_hmac_free(c->rtcp_auth);
    memset(c, 0, sizeof(*c));
}

/* IV = (salt * 2^16) XOR (SSRC * 2^64) XOR (index * 2^16), index 48 bits. */
static void make_iv(uint8_t iv[16], const uint8_t salt[SRTP_SALT_LEN], const uint8_t ssrc[4],
                    uint64_t index)
{
    memset(iv, 0, 16);
    memcpy(iv, salt, SRTP_SALT_LEN);
    for (int i = 0; i < 4; i++) {
        iv[4 + i] ^= ssrc[i];
    }
    for (int i = 0; i < 6; i++) {
        iv[13 - i] ^= (uint8_t)(index >> (8 * i));
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int srtp_protect(srtp_ctx_t *c, uint8_t *pkt, size_t len, size_t cap)
{
    if (len < RTP_HDR_LEN || cap < len + SRTP_TAG_LEN || (pkt[0] >> 6) != 2) {
        return -1;
    }
    /* The header is authenticated but not encrypted, so where the payload
       starts matters: past any CSRCs and the header extension (RFC 3550 5.3.1). */
    size_t hl = RTP_HDR_LEN + 4u * (pkt[0] & 0x0f);
    if (pkt[0] & 0x10) {
        if (len < hl + 4) {
            return -1;
        }
        hl += 4u + 4u * (size_t)(pkt[hl + 2] << 8 | pkt[hl + 3]);
    }
    if (hl > len) {
        return -1;
    }
    const uint16_t seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
    if (c->seq_seen && seq < c->last_seq && (uint16_t)(c->last_seq - seq) > 0x8000) {
        c->roc++; /* wrapped */
    }
    if (!c->seq_seen || (uint16_t)(seq - c->last_seq) < 0x8000) {
        c->last_seq = seq;
    }
    c->seq_seen = true;

    uint8_t iv[16];
    make_iv(iv, c->rtp_salt, pkt + 8, (uint64_t)c->roc << 16 | seq);
    if (wr_aes_ctr(c->rtp_aes, iv, pkt + hl, pkt + hl, len - hl) != 0) {
        return -1;
    }
    uint8_t roc[4], mac[20];
    put32(roc, c->roc);
    const uint8_t *parts[2] = {pkt, roc};
    const size_t lens[2] = {len, 4};
    if (wr_hmac_sha1(c->rtp_auth, parts, lens, 2, mac) != 0) {
        return -1;
    }
    memcpy(pkt + len, mac, SRTP_TAG_LEN);
    return (int)(len + SRTP_TAG_LEN);
}

int srtcp_protect(srtp_ctx_t *c, uint8_t *pkt, size_t len, size_t cap)
{
    if (len < 8 || cap < len + SRTCP_TRAILER_LEN) {
        return -1;
    }
    const uint32_t index = c->rtcp_index++ & 0x7fffffffu;
    uint8_t iv[16];
    make_iv(iv, c->rtcp_salt, pkt + 4, index);
    if (wr_aes_ctr(c->rtcp_aes, iv, pkt + 8, pkt + 8, len - 8) != 0) {
        return -1;
    }
    put32(pkt + len, 0x80000000u | index); /* E: encrypted */
    uint8_t mac[20];
    const uint8_t *parts[1] = {pkt};
    const size_t lens[1] = {len + 4};
    if (wr_hmac_sha1(c->rtcp_auth, parts, lens, 1, mac) != 0) {
        return -1;
    }
    memcpy(pkt + len + 4, mac, SRTP_TAG_LEN);
    return (int)(len + SRTCP_TRAILER_LEN);
}

static bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

int srtcp_unprotect(srtp_ctx_t *c, uint8_t *pkt, size_t len)
{
    if (len < 8 + SRTCP_TRAILER_LEN) {
        return -1;
    }
    const size_t body = len - SRTCP_TRAILER_LEN;
    uint8_t mac[20];
    const uint8_t *parts[1] = {pkt};
    const size_t lens[1] = {body + 4};
    if (wr_hmac_sha1(c->rtcp_auth, parts, lens, 1, mac) != 0 ||
        !same(mac, pkt + body + 4, SRTP_TAG_LEN)) {
        return -1;
    }
    const uint32_t e_index = get32(pkt + body);
    const uint32_t index = e_index & 0x7fffffffu;

    /* Replay: anything at or below the sender's window, or already seen in
       it. A third sender takes the second's place. */
    const uint32_t ssrc = get32(pkt + 4);
    int w = SRTCP_SENDERS - 1;
    for (int i = 0; i < SRTCP_SENDERS; i++) {
        if (!c->in[i].seen || c->in[i].ssrc == ssrc) {
            w = i;
            break;
        }
    }
    if (c->in[w].seen && c->in[w].ssrc != ssrc) {
        c->in[w].seen = false;
    }
    if (c->in[w].seen) {
        if (index > c->in[w].max) {
            const uint32_t shift = index - c->in[w].max;
            c->in[w].window = shift >= 64 ? 0 : c->in[w].window << shift;
            c->in[w].window |= 1;
            c->in[w].max = index;
        } else {
            const uint32_t back = c->in[w].max - index;
            if (back >= 64 || (c->in[w].window >> back) & 1) {
                return -1;
            }
            c->in[w].window |= (uint64_t)1 << back;
        }
    } else {
        c->in[w].seen = true;
        c->in[w].ssrc = ssrc;
        c->in[w].max = index;
        c->in[w].window = 1;
    }

    if (e_index & 0x80000000u) {
        uint8_t iv[16];
        make_iv(iv, c->rtcp_salt, pkt + 4, index);
        if (wr_aes_ctr(c->rtcp_aes, iv, pkt + 8, pkt + 8, body - 8) != 0) {
            return -1;
        }
    }
    return (int)body;
}
