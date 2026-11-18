/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SRTP and SRTCP (RFC 3711) with the one profile every browser offers,
 * AES_CM_128_HMAC_SHA1_80. One context per direction: the device protects
 * what it sends with the DTLS server's keys and checks what the browser sends
 * with the client's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wr_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SRTP_KEY_LEN 16
#define SRTP_SALT_LEN 14
#define SRTP_TAG_LEN 10
/* SRTCP adds the E flag + index in front of the tag. */
#define SRTCP_TRAILER_LEN (4 + SRTP_TAG_LEN)
#define SRTCP_SENDERS 2

typedef struct {
    wr_aes_t *rtp_aes, *rtcp_aes;
    wr_hmac_t *rtp_auth, *rtcp_auth;
    uint8_t rtp_salt[SRTP_SALT_LEN], rtcp_salt[SRTP_SALT_LEN];
    /* Sending RTP: the rollover counter, bumped when the sequence wraps. */
    uint32_t roc;
    uint16_t last_seq;
    bool seq_seen;
    /* Sending RTCP. */
    uint32_t rtcp_index;
    /* Receiving RTCP: a replay window over the last 64 indexes for each
       sender. A browser that gets audio and video may report on both from
       separate SSRCs, each with its own index. */
    struct {
        uint32_t ssrc;
        uint32_t max;
        uint64_t window;
        bool seen;
    } in[SRTCP_SENDERS];
} srtp_ctx_t;

/** The RFC 3711 4.3 key derivation, for the tests. */
int srtp_kdf(const uint8_t master_key[SRTP_KEY_LEN], const uint8_t master_salt[SRTP_SALT_LEN],
             uint8_t label, uint8_t *out, size_t len);

int srtp_init(srtp_ctx_t *c, const uint8_t master_key[SRTP_KEY_LEN],
              const uint8_t master_salt[SRTP_SALT_LEN]);
void srtp_free(srtp_ctx_t *c);

/**
 * Encrypt an RTP packet in place and append its tag. @p cap must leave
 * SRTP_TAG_LEN bytes. Returns the new length or -1.
 */
int srtp_protect(srtp_ctx_t *c, uint8_t *pkt, size_t len, size_t cap);

/** Same for a compound RTCP packet; @p cap must leave SRTCP_TRAILER_LEN. */
int srtcp_protect(srtp_ctx_t *c, uint8_t *pkt, size_t len, size_t cap);

/**
 * Check and decrypt an SRTCP packet in place. Returns the length of the plain
 * RTCP or -1 when it is forged, damaged or a replay.
 */
int srtcp_unprotect(srtp_ctx_t *c, uint8_t *pkt, size_t len);

#ifdef __cplusplus
}
#endif
