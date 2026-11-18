/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SHA-1 lives here rather than coming from PSA: TOTP is defined over HMAC-SHA1,
 * the amount of data is a few dozen bytes per login, and keeping it plain C
 * means the RFC's test vectors run on a host.
 */
#include "totp.h"

#include <string.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} sha1_t;

static uint32_t rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_init(sha1_t *s)
{
    static const uint32_t iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

static void sha1_update(sha1_t *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n--) {
        s->buf[s->fill++] = *p++;
        if (s->fill == 64) {
            sha1_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha1_final(sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->len * 8;
    const uint8_t pad = 0x80, zero = 0;
    sha1_update(s, &pad, 1);
    while (s->fill != 56) {
        sha1_update(s, &zero, 1);
    }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void totp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                    uint8_t out[20])
{
    uint8_t k[64] = {0};
    if (key_len > 64) {
        sha1_t s;
        sha1_init(&s);
        sha1_update(&s, key, key_len);
        sha1_final(&s, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t pad[64];
    uint8_t inner[20];
    sha1_t s;
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, msg, msg_len);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}

uint32_t totp_code(const uint8_t *secret, size_t secret_len, uint64_t counter, int digits)
{
    uint8_t msg[8];
    for (int i = 0; i < 8; i++) {
        msg[i] = (uint8_t)(counter >> (56 - 8 * i));
    }
    uint8_t mac[20];
    totp_hmac_sha1(secret, secret_len, msg, sizeof(msg), mac);
    const int off = mac[19] & 0x0f;
    const uint32_t bin = (uint32_t)(mac[off] & 0x7f) << 24 | (uint32_t)mac[off + 1] << 16 |
                         (uint32_t)mac[off + 2] << 8 | mac[off + 3];
    uint32_t mod = 1;
    for (int i = 0; i < digits; i++) {
        mod *= 10;
    }
    return bin % mod;
}

bool totp_check(const uint8_t *secret, size_t secret_len, uint64_t unix_time, uint32_t code,
                uint64_t last_used, uint64_t *used_out)
{
    const uint64_t now = unix_time / TOTP_PERIOD;
    bool ok = false;
    /* Every candidate is computed, so the time taken says nothing about which matched. */
    for (int d = -1; d <= 1; d++) {
        const uint64_t step = now + (uint64_t)(int64_t)d;
        const bool match = totp_code(secret, secret_len, step, TOTP_DIGITS) == code;
        if (match && step > last_used && !ok) {
            ok = true;
            if (used_out) {
                *used_out = step;
            }
        }
    }
    return ok;
}

size_t totp_base32(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | in[i];
        bits += 8;
        while (bits >= 5 && o + 1 < cap) {
            out[o++] = abc[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0 && o + 1 < cap) {
        out[o++] = abc[(acc << (5 - bits)) & 31];
    }
    if (cap) {
        out[o] = '\0';
    }
    return o;
}
