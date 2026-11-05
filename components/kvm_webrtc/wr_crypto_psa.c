/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * wr_crypto.h over PSA. Keys are imported once per session and kept as
 * volatile PSA keys, so a packet costs one cipher or MAC operation and no
 * key schedule.
 */
#include "wr_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "psa/crypto.h"

#define CTR_CHUNK 512

struct wr_aes {
    psa_key_id_t id;
};

struct wr_hmac {
    psa_key_id_t id;
};

wr_aes_t *wr_aes_new(const uint8_t key[16])
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return NULL;
    }
    wr_aes_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    if (psa_import_key(&attr, key, 16, &a->id) != PSA_SUCCESS) {
        free(a);
        return NULL;
    }
    return a;
}

void wr_aes_free(wr_aes_t *a)
{
    if (a) {
        psa_destroy_key(a->id);
        free(a);
    }
}

int wr_aes_ctr(wr_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    /* PSA has no "keystream only": encrypt zeros, in place (PSA allows it). */
    if (!in) {
        memset(out, 0, len);
        in = out;
    }
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t got = 0, tail = 0;
    if (psa_cipher_encrypt_setup(&op, a->id, PSA_ALG_CTR) != PSA_SUCCESS ||
        psa_cipher_set_iv(&op, iv, 16) != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        return -1;
    }
    /*
     * At most 512 bytes a call: the P4's AES driver does that much in block
     * mode, and anything longer through DMA, which takes descriptors and
     * bounce buffers from internal RAM for every call. One video packet per
     * call, for every viewer, ran that RAM out (and with it SDIO to the
     * Wi-Fi chip). 512 is a whole number of AES blocks, so the counter runs
     * on unbroken.
     */
    for (size_t off = 0; off < len;) {
        const size_t n = len - off > CTR_CHUNK ? CTR_CHUNK : len - off;
        size_t done = 0;
        if (psa_cipher_update(&op, in + off, n, out + got, len - got, &done) != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            return -1;
        }
        got += done;
        off += n;
    }
    if (psa_cipher_finish(&op, out + got, len - got, &tail) != PSA_SUCCESS) {
        psa_cipher_abort(&op);
        return -1;
    }
    return got + tail == len ? 0 : -1;
}

wr_hmac_t *wr_hmac_new(const uint8_t *key, size_t key_len)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return NULL;
    }
    wr_hmac_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, PSA_BYTES_TO_BITS(key_len));
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_1));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    if (psa_import_key(&attr, key, key_len, &h->id) != PSA_SUCCESS) {
        free(h);
        return NULL;
    }
    return h;
}

void wr_hmac_free(wr_hmac_t *h)
{
    if (h) {
        psa_destroy_key(h->id);
        free(h);
    }
}

int wr_hmac_sha1(wr_hmac_t *h, const uint8_t *const *parts, const size_t *lens, int n,
                 uint8_t out[20])
{
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    if (psa_mac_sign_setup(&op, h->id, PSA_ALG_HMAC(PSA_ALG_SHA_1)) != PSA_SUCCESS) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (psa_mac_update(&op, parts[i], lens[i]) != PSA_SUCCESS) {
            psa_mac_abort(&op);
            return -1;
        }
    }
    size_t got = 0;
    if (psa_mac_sign_finish(&op, out, 20, &got) != PSA_SUCCESS || got != 20) {
        psa_mac_abort(&op);
        return -1;
    }
    return 0;
}
