/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The part of STUN (RFC 5389) an ICE-lite agent needs (RFC 8445): read a
 * Binding request, check that it carries our username and was signed with our
 * password, and answer it. The device never sends checks of its own.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wr_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STUN_HDR_LEN 20
/* A success response with XOR-MAPPED-ADDRESS (IPv4), MESSAGE-INTEGRITY and
   FINGERPRINT. */
#define STUN_RESPONSE_LEN (STUN_HDR_LEN + 12 + 24 + 8)

typedef struct {
    uint8_t txid[12];
    /* The browser picked this pair (ICE nomination). */
    bool use_candidate;
} stun_req_t;

/** First byte 0..3 and the magic cookie: a STUN message (RFC 7983 demux). */
bool stun_is(const uint8_t *p, size_t len);

/**
 * Check a Binding request: its USERNAME must start with "<local_ufrag>:", its
 * MESSAGE-INTEGRITY must verify with @p pwd (an HMAC keyed with our ice-pwd),
 * and a FINGERPRINT, when present, must match. 0 when it is ours.
 */
int stun_check_binding(const uint8_t *p, size_t len, const char *local_ufrag, wr_hmac_t *pwd,
                       stun_req_t *out);

/**
 * Copy our half of a Binding request's USERNAME ("ours:theirs") into @p out,
 * so one socket can tell its sessions apart before checking anything. 0 when
 * there is one and it fits. Says nothing about whether the request is genuine.
 */
int stun_local_ufrag(const uint8_t *p, size_t len, char *out, size_t cap);

/**
 * Write the success response to a request from @p ip_be : @p port_be (both in
 * network order). Returns its length, STUN_RESPONSE_LEN, or 0 if @p cap is
 * too small or the HMAC failed.
 */
size_t stun_build_success(const uint8_t txid[12], uint32_t ip_be, uint16_t port_be, wr_hmac_t *pwd,
                          uint8_t *out, size_t cap);

/** CRC-32 (IEEE), as FINGERPRINT uses it. Exposed for the tests. */
uint32_t stun_crc32(const uint8_t *p, size_t len);

#ifdef __cplusplus
}
#endif
