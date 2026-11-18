/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One encoded H.264 frame (Annex B, as the encoder writes it) into RTP packets
 * (RFC 6184, packetization-mode 1): a NAL unit that fits goes whole, a larger
 * one is cut into FU-A fragments.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTP_HDR_LEN 12
/* The playout-delay extension, when it is on: a one-byte-header block of one
   word holding one 3-byte element (RFC 8285). */
#define RTP_EXT_LEN 8

typedef struct {
    uint32_t ssrc;
    uint16_t seq; /* the next packet's */
    uint8_t pt;
    /* RTP payload per packet; header, extension and SRTP tag come on top. */
    size_t max_payload;
    /* The playout-delay extension's id, or 0 for none. Sent as min = max = 0:
       show each frame as soon as it is decoded, with no smoothing buffer. */
    uint8_t playout_ext;
} rtp_h264_t;

/**
 * Called for each packet, written into the caller's @p buf. Returning non-zero
 * stops the frame (the link is full): the rest is not sent.
 */
typedef int (*rtp_emit_fn)(void *ctx, uint8_t *pkt, size_t len);

/**
 * Packetize @p au with RTP timestamp @p ts. @p buf must hold RTP_HDR_LEN +
 * RTP_EXT_LEN + max_payload (+ whatever the emitter appends). Returns the number of packets
 * emitted, or -1 if the emitter stopped it.
 */
int rtp_h264_send(rtp_h264_t *st, const uint8_t *au, size_t len, uint32_t ts, uint8_t *buf,
                  rtp_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif
