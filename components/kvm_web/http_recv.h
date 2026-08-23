/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"
#include "mbedtls/ssl.h"

/*
 * A read that means "nothing yet", not "the other end is gone".
 *
 * Over plain HTTP a sender that goes quiet comes back as HTTPD_SOCK_ERR_TIMEOUT.
 * Over HTTPS the same silence comes back as the raw mbedTLS code: the socket
 * carries a receive timeout, mbedtls_net_recv turns EAGAIN into WANT_READ, and
 * esp_https_server passes it up untouched. Reading that as a dead connection cut
 * uploads short whenever something else on the device starved them for a while.
 */
static inline bool kvm_recv_stalled(int n)
{
    return n == HTTPD_SOCK_ERR_TIMEOUT || n == MBEDTLS_ERR_SSL_WANT_READ ||
           n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT;
}

/* Silences in a row to sit through before calling an upload dead. Each one lasts
 * recv_wait_timeout, so this is minutes, not seconds. */
#define KVM_RECV_MAX_STALLS 8
