/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"
#include "mbedtls/ssl.h"

/*
 * A read that means "nothing yet", not "the other end is gone". Plain HTTP says
 * HTTPD_SOCK_ERR_TIMEOUT; HTTPS passes up the raw mbedTLS code instead, and
 * taking that for a dead connection cut uploads short.
 */
static inline bool kvm_recv_stalled(int n)
{
    return n == HTTPD_SOCK_ERR_TIMEOUT || n == MBEDTLS_ERR_SSL_WANT_READ ||
           n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT;
}

/* Silences in a row before an upload is called dead - each is recv_wait_timeout. */
#define KVM_RECV_MAX_STALLS 8
