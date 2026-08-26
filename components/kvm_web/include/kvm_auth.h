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
 * How the start-up reset window is going, for whatever the board can show it
 * on. Called from the polling loop, so it must return promptly and must not
 * assume any particular subsystem is up yet.
 *
 * @param pct  -1 while the window is open and the button is not down, else
 *             0..100 of the hold still needed to trigger the reset
 * @param done NULL until the reset fires, then a short line saying what it did
 */
typedef void (*kvm_auth_reset_ui_cb_t)(int pct, const char *done);

/**
 * Watch the board button for a few seconds and clear the password if it is
 * held. Call early in start-up, before the network is brought up: the pin is
 * shared with the Ethernet interface, and claiming it later takes the network
 * down with it.
 *
 * @param ui optional progress sink, or NULL. Without one the gesture is silent,
 *           which is how it shipped and why nobody could tell whether their
 *           press had registered: the window is the one moment a headless
 *           device has something to say and no network to say it over.
 */
void kvm_auth_check_reset_button(kvm_auth_reset_ui_cb_t ui);

/** Whether a request must carry a valid session to be served at all. */
bool kvm_auth_required(void);

/**
 * True when the request carries a valid session, or when authentication is
 * switched off. Handlers call this first and answer 401 when it is false.
 */
/*
 * The header our own console puts on everything it sends.
 *
 * A page on another site can make a browser send a request with this device's
 * cookie attached, but it cannot add a header of its own without asking
 * permission first - which this device never gives. So a state-changing request
 * has to carry either this or a JSON content type, neither of which a plain
 * cross-site form can produce.
 */
#define KVM_CONSOLE_HEADER "X-ESP-KVM"

/**
 * Whether the request's Origin, if it sent one, is this device.
 *
 * True when there is no Origin at all: that is a same-origin GET or a client
 * that is not a browser, and neither can be a cross-site forgery.
 */
/*
 * The viewing token: a credential that opens the picture and nothing else.
 *
 * Home Assistant's camera integrations can be given a URL and little else, and
 * this device authenticates with a session cookie - so the only way to put a
 * target's screen on a dashboard used to be turning the login off. This is the
 * alternative: a long random string that reaches the MJPEG stream, a single
 * frame and the capture's figures, and no other endpoint at all.
 *
 * Off until one is made. Only the SHA-256 of it is stored, and it is shown once.
 */
#define KVM_AUTH_TOKEN_CHARS 32

/** Make one (replacing any before it) and write it to @p out, once. */
esp_err_t kvm_auth_token_create(char *out, size_t out_len);

/** Forget it. Whatever was handed out stops working. */
esp_err_t kvm_auth_token_revoke(void);

/** Whether a token has been made at all - never says what it is. */
bool kvm_auth_token_exists(void);

/**
 * Whether this request carries the token, as `Authorization: Bearer <token>` or
 * `?token=<token>`. Call it only from the handlers a viewer may reach.
 */
bool kvm_auth_token_ok(httpd_req_t *req);

bool kvm_auth_origin_ok(httpd_req_t *req);

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
