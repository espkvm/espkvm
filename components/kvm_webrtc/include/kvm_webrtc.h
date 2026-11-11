/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * H.264 to the browser over WebRTC.
 *
 * A second way out for the same encoded frames the WebSocket carries, not a
 * replacement: both run at once, each viewer on the transport its console
 * picked. Only video goes this way. Keys, the mouse and everything else stay
 * on the WebSocket.
 *
 * The device is an ICE-lite agent with host candidates only, and the DTLS
 * server. The browser offers, the device answers over the signed-in REST API
 * (no trickle), the browser checks connectivity, DTLS runs, and then frames go
 * out as SRTP until the browser leaves, its sign-in ends, or it stops
 * answering.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether the sign-in a session was opened under still holds; called every few
 * seconds. @p owner is what kvm_webrtc_offer() was given.
 */
typedef bool (*kvm_webrtc_owner_alive_fn)(const uint8_t owner[32]);

/** Start the module. The certificate is made in the background. */
esp_err_t kvm_webrtc_init(kvm_webrtc_owner_alive_fn alive);

/**
 * Answer a browser's offer. On success *answer is a malloc'd SDP answer and
 * *id the session's number. On failure @p err says why and *status is the HTTP
 * status to send.
 */
esp_err_t kvm_webrtc_offer(const char *offer, const uint8_t owner[32], char **answer,
                           uint32_t *id, int *status, char *err, size_t err_len);

/** The browser is done with a session. */
void kvm_webrtc_close(uint32_t id, const uint8_t owner[32]); /* only the sign-in that made it */

/**
 * Start or stop sending sound in session @p id, when its offer asked for it.
 * Off at first: the browser asks once the operator turns the sound on, and
 * the encoder runs only while some session wants it.
 */
void kvm_webrtc_sound(uint32_t id, const uint8_t owner[32], bool on);

/** Sessions open now. */
int kvm_webrtc_count(void);

/**
 * A message from a session's data channel - the console's pointer moves.
 * Called on the module's receive task; @p owner is what kvm_webrtc_offer() was
 * given.
 */
typedef void (*kvm_webrtc_input_fn)(const uint8_t owner[32], const uint8_t *msg, size_t len);

/** Where data channel messages go; NULL drops them. */
void kvm_webrtc_set_input(kvm_webrtc_input_fn fn);

#ifdef __cplusplus
}
#endif
