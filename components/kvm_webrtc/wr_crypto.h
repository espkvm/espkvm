/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The two primitives STUN and SRTP need, behind one small interface: on the
 * device they are PSA (hardware AES and SHA), in the host tests plain C. That
 * keeps stun.c and srtp.c free of PSA, so the RFC vectors run on a host.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wr_aes wr_aes_t;
typedef struct wr_hmac wr_hmac_t;

/** AES-128 in counter mode with a fixed key. NULL when out of memory. */
wr_aes_t *wr_aes_new(const uint8_t key[16]);
void wr_aes_free(wr_aes_t *a);
/**
 * out = in XOR the keystream that starts at counter block @p iv (the low 16
 * bits count up). @p in may be NULL for the bare keystream. 0 on success.
 */
int wr_aes_ctr(wr_aes_t *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);

/** HMAC-SHA1 with a fixed key. NULL when out of memory. */
wr_hmac_t *wr_hmac_new(const uint8_t *key, size_t key_len);
void wr_hmac_free(wr_hmac_t *h);
/** HMAC-SHA1 over @p n parts, one after the other. 0 on success. */
int wr_hmac_sha1(wr_hmac_t *h, const uint8_t *const *parts, const size_t *lens, int n,
                 uint8_t out[20]);

#ifdef __cplusplus
}
#endif
