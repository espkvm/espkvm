/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The receiving half of SCTP (RFC 4960), as much as a WebRTC data channel
 * needs when the browser only sends.
 *
 * The console opens one channel with negotiated id 0, unordered and without
 * retransmission, so there is no channel-open handshake (RFC 8832) and the
 * device never sends DATA: it answers the association setup, acknowledges
 * what arrives, follows FORWARD-TSN past what the browser gave up on, and
 * hands each message up as it comes. One SCTP packet travels in one DTLS
 * record, both ways.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload protocol ids (RFC 8831): binary and string messages. */
#define SCTP_PPID_STRING 51
#define SCTP_PPID_BINARY 53

typedef struct {
    uint16_t local_port, peer_port;
    uint32_t my_vtag, peer_vtag;
    uint32_t my_tsn;
    bool up;           /* COOKIE ECHO answered */
    bool have_tsn;     /* the peer's INIT gave its first TSN */
    uint32_t cum_tsn;  /* every TSN up to this one has arrived */
    uint32_t ahead;    /* bit i: cum_tsn + 1 + i arrived */
    uint8_t unacked;   /* DATA packets since the last SACK */
} sctp_assoc_t;

/** A message: one whole DATA chunk. */
typedef void (*sctp_msg_fn)(void *ctx, uint16_t stream, uint32_t ppid, const uint8_t *data,
                            size_t len);

/** Start an association; @p vtag and @p tsn are random, @p vtag not 0. */
void sctp_init(sctp_assoc_t *a, uint16_t local_port, uint32_t vtag, uint32_t tsn);

/**
 * Take one SCTP packet from the peer. Messages go to @p fn; the answer, if
 * there is one, is written to @p out. Returns the answer's length, 0 for none.
 */
size_t sctp_input(sctp_assoc_t *a, const uint8_t *in, size_t len, uint8_t *out, size_t cap,
                  sctp_msg_fn fn, void *ctx);

/**
 * A SACK for what arrived since the last one, or 0 when nothing did. DATA is
 * acknowledged every SCTP_SACK_EVERY packets, or at once when one is missing
 * (RFC 4960 6.2); the caller sends this for the rest within 200 ms.
 */
size_t sctp_sack(sctp_assoc_t *a, uint8_t *out, size_t cap);

#define SCTP_SACK_EVERY 4

/** CRC32c over @p len bytes, as the SCTP checksum wants it. */
uint32_t sctp_crc32c(const uint8_t *p, size_t len);

#ifdef __cplusplus
}
#endif
