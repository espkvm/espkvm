/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * wr_crypto.h in plain C for the host tests: a textbook AES-128 (encrypt
 * only, which is all counter mode needs) and SHA-1 with HMAC. Slow and
 * simple; never built into the firmware.
 */
#include "wr_crypto.h"

#include <stdlib.h>
#include <string.h>

/* ---- AES-128 ---- */

static uint8_t sbox[256];

static uint8_t xt(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) {
            r ^= a;
        }
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void make_sbox(void)
{
    if (sbox[0]) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0;
        for (int j = 1; j < 256 && i; j++) {
            if (gmul((uint8_t)i, (uint8_t)j) == 1) {
                inv = (uint8_t)j;
                break;
            }
        }
        uint8_t s = inv, x = inv;
        for (int k = 0; k < 4; k++) {
            x = (uint8_t)((x << 1) | (x >> 7));
            s ^= x;
        }
        sbox[i] = s ^ 0x63;
    }
}

struct wr_aes {
    uint8_t rk[176];
};

wr_aes_t *wr_aes_new(const uint8_t key[16])
{
    make_sbox();
    wr_aes_t *a = calloc(1, sizeof(*a));
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            const uint8_t f = t[0];
            t[0] = sbox[t[1]] ^ rcon;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[f];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) {
            a->rk[i + k] = a->rk[i - 16 + k] ^ t[k];
        }
    }
    return a;
}

void wr_aes_free(wr_aes_t *a)
{
    free(a);
}

static void encrypt_block(const wr_aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) {
        s[i] = in[i] ^ a->rk[i];
    }
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) {
            t[i] = sbox[s[i]];
        }
        /* ShiftRows: column-major state. */
        for (int c = 0; c < 4; c++) {
            for (int row = 0; row < 4; row++) {
                s[c * 4 + row] = t[((c + row) % 4) * 4 + row];
            }
        }
        if (r != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *m = s + c * 4;
                const uint8_t a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
                m[0] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
                m[1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
                m[2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
                m[3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] ^= a->rk[r * 16 + i];
        }
    }
    memcpy(out, s, 16);
}

int wr_aes_ctr(wr_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        encrypt_block(a, ctr, ks);
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            out[off + i] = (in ? in[off + i] : 0) ^ ks[i];
        }
        for (int i = 15; i >= 0 && ++ctr[i] == 0; i--) {
        }
    }
    return 0;
}

/* ---- SHA-1 and HMAC ---- */

typedef struct {
    uint32_t h[5];
    uint64_t n;
    uint8_t buf[64];
    size_t used;
} sha1_t;

static uint32_t rol(uint32_t x, int k)
{
    return x << k | x >> (32 - k);
}

static void sha1_block(sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
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
    static const uint32_t h0[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, h0, sizeof(h0));
    s->n = 0;
    s->used = 0;
}

static void sha1_update(sha1_t *s, const uint8_t *p, size_t n)
{
    s->n += n;
    while (n) {
        size_t k = 64 - s->used;
        if (k > n) {
            k = n;
        }
        memcpy(s->buf + s->used, p, k);
        s->used += k;
        p += k;
        n -= k;
        if (s->used == 64) {
            sha1_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha1_final(sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->n * 8;
    const uint8_t one = 0x80, zero = 0;
    sha1_update(s, &one, 1);
    while (s->used != 56) {
        sha1_update(s, &zero, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

struct wr_hmac {
    uint8_t k[64];
};

wr_hmac_t *wr_hmac_new(const uint8_t *key, size_t key_len)
{
    wr_hmac_t *h = calloc(1, sizeof(*h));
    if (key_len > 64) {
        sha1_t s;
        sha1_init(&s);
        sha1_update(&s, key, key_len);
        sha1_final(&s, h->k);
    } else {
        memcpy(h->k, key, key_len);
    }
    return h;
}

void wr_hmac_free(wr_hmac_t *h)
{
    free(h);
}

int wr_hmac_sha1(wr_hmac_t *h, const uint8_t *const *parts, const size_t *lens, int n,
                 uint8_t out[20])
{
    uint8_t pad[64], inner[20];
    sha1_t s;
    for (int i = 0; i < 64; i++) {
        pad[i] = h->k[i] ^ 0x36;
    }
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    for (int i = 0; i < n; i++) {
        sha1_update(&s, parts[i], lens[i]);
    }
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = h->k[i] ^ 0x5c;
    }
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
    return 0;
}
