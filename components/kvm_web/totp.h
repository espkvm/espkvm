/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Time-based one-time passwords (RFC 6238): the six digits an authenticator
 * app shows. Plain C with its own SHA-1, so it builds and is tested on a host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TOTP_SECRET_LEN 20 /* 160 bits, what RFC 4226 recommends */
#define TOTP_PERIOD 30
#define TOTP_DIGITS 6

/** HMAC-SHA1 of @p msg under @p key. */
void totp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                    uint8_t out[20]);

/** The code for time step @p counter, with @p digits digits (6 or 8). */
uint32_t totp_code(const uint8_t *secret, size_t secret_len, uint64_t counter, int digits);

/**
 * Is @p code right for the step that holds @p unix_time, or the one either side
 * of it (a clock half a minute out, a code typed as it changed)? A step not
 * above @p last_used is refused, so a code works once. On success *@p used_out
 * is the step it matched.
 */
bool totp_check(const uint8_t *secret, size_t secret_len, uint64_t unix_time, uint32_t code,
                uint64_t last_used, uint64_t *used_out);

/** RFC 4648 base32 without padding, as authenticator apps take it. Returns the length. */
size_t totp_base32(const uint8_t *in, size_t len, char *out, size_t cap);
