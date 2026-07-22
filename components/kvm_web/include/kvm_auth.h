/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Who is allowed to drive the target machine.
 *
 * A KVM hands whoever reaches it a keyboard and a screen on someone else's
 * computer, so the question is not decoration. The password is stored as a
 * PBKDF2-HMAC-SHA256 hash with a per-device salt; sessions are random tokens
 * held in RAM and handed out in an HttpOnly cookie, so a page cannot read
 * them and a reboot ends every session.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load the stored credentials. Call once at start-up. */
esp_err_t kvm_auth_init(void);

/**
 * Watch the board button for a few seconds and clear the password if it is
 * held. Call early in start-up, before the network is brought up: the pin is
 * shared with the Ethernet interface, and claiming it later takes the network
 * down with it.
 */
void kvm_auth_check_reset_button(void);

/** Whether a request must carry a valid session to be served at all. */
bool kvm_auth_required(void);

/**
 * True when the request carries a valid session, or when authentication is
 * switched off. Handlers call this first and answer 401 when it is false.
 */
bool kvm_auth_check(httpd_req_t *req);

/** Send the 401 that tells the console to show a login form. */
esp_err_t kvm_auth_challenge(httpd_req_t *req);

/**
 * Remember that this socket presented a valid session at its upgrade, and
 * forget it when the session closes. WebSocket frames carry no headers, so
 * this is the only thing left to check them against.
 */
void kvm_auth_mark_socket(int fd);
void kvm_auth_forget_socket(int fd);
bool kvm_auth_socket_ok(int fd);

/**
 * Refuse a WebSocket by closing it. The upgrade has already been answered by
 * the time a handler runs, so there is no status line left to send.
 */
esp_err_t kvm_auth_reject_ws(httpd_req_t *req);

/** Register the login, logout, session and password endpoints. */
void kvm_auth_register(httpd_handle_t server);

/**
 * Compute PBKDF2-HMAC-SHA256. Exposed for the start-up self-test, which
 * checks it against a published vector: a key derivation that is subtly wrong
 * fails silently, and silently means "every password is accepted".
 */
esp_err_t kvm_auth_pbkdf2(const char *password, const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
