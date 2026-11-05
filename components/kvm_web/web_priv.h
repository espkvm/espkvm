/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between the web component's own files.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "esp_transport.h"

/** {"error": message} with @p status, e.g. "409 Conflict". */
esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message);

/** Recording, screenshots, and the files they leave on the card. See record_api.c. */
const httpd_uri_t *record_api_routes(size_t *count);
const httpd_uri_t *cec_api_routes(size_t *count);
/** Tailscale state, peers and logout. See ts_api.c. */
const httpd_uri_t *ts_api_routes(size_t *count);
/** Web Push: the key and the subscribed browsers. See push_api.c. */
const httpd_uri_t *push_api_routes(size_t *count);
/** The client's address on socket @p fd as text ("?" when unknown). */
void kvm_web_peer_ip(int fd, char *out, size_t cap);
/** Device profiles. See profiles_api.c. */
const httpd_uri_t *profiles_api_routes(size_t *count);

/** Screen areas. See regions_api.c. */
const httpd_uri_t *regions_api_routes(size_t *count);
/** The target's serial console: the /serial WebSocket and its REST. See serial_api.c. */
const httpd_uri_t *serial_api_routes(size_t *count);
/* agent_api.c: espkvm-agent on the target. */
const httpd_uri_t *agent_api_routes(size_t *count);
/**
 * A TLS transport for esp_http_client_config_t.transport that offers only
 * ChaCha20-Poly1305. AES-GCM goes to the hardware AES, which needs internal RAM
 * for DMA on every record and has no software fallback - with video running a
 * download died part-way ("-0x0084", then "invalid MAC"). NULL when mbedTLS has
 * no ChaCha20. The caller destroys it after esp_http_client_cleanup(). A server
 * without ChaCha20 refuses the handshake, so callers retry once without it.
 */
esp_transport_handle_t kvm_web_chacha_transport(void);
/** Download an image from a URL to the card or the rescue partition. See url_fetch.c. */
const httpd_uri_t *url_fetch_routes(size_t *count);
/** HDMI audio: the /audio socket, state and a WAV clip. See audio_api.c. */
const httpd_uri_t *audio_api_routes(size_t *count);
esp_err_t audio_api_init(void);
void audio_api_drop(int fd);

/** The target's log over the network (netconsole). See netlog_api.c. */
const httpd_uri_t *netlog_api_routes(size_t *count);
/** GET /api/v1/system/pinprobe: how fast some pins switch. See pinprobe_api.c. */
const httpd_uri_t *pinprobe_api_routes(size_t *count);
/** Offer/answer for the WebRTC video. See webrtc_api.c. */
const httpd_uri_t *webrtc_api_routes(size_t *count);
/** A session went away; forget it as a serial console client. */
void serial_api_drop(int fd);

/** The WebSocket handshake check every socket here goes through (origin, session). */
esp_err_t kvm_web_ws_pre_handshake(httpd_req_t *req);

/** A message from a WebRTC data channel: see http_server.c. */
void kvm_web_webrtc_input(const uint8_t owner[32], const uint8_t *msg, size_t len);
/** One binary frame to a WebSocket, under the lock every TLS write takes. */
esp_err_t kvm_web_ws_send_binary(int fd, const uint8_t *data, size_t len);
/** The agent_api switch for the machine-facing API; on false *out holds the reply. */
bool kvm_web_agent_allowed(httpd_req_t *req, esp_err_t *out);
/** Set the clock from the browser's time (Unix seconds) if it was never set. */
void kvm_web_clock_from_browser(long long t);
/** Restart a moment after the reply has gone out, from a timer. */
void kvm_web_restart_soon(uint32_t delay_ms);

