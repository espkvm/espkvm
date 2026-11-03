/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    VENC_IO_ERROR = -1,
    VENC_IO_INTERRUPTED = -2,
    VENC_IO_WANT_READ = -3,
    VENC_IO_WANT_WRITE = -4,
    VENC_IO_CLOSED = 0,
};

typedef int (*vencrypt_io_fn)(void *ctx, uint8_t *data, size_t len);
typedef int (*vencrypt_send_fn)(void *ctx, const uint8_t *data, size_t len);
typedef bool (*vencrypt_wait_fn)(void *ctx, bool writing);

typedef struct {
    void *ctx;
    vencrypt_send_fn send;
    vencrypt_io_fn recv;
    vencrypt_wait_fn wait;
} vencrypt_io_t;

typedef enum {
    VENC_RESULT_OK,
    VENC_RESULT_IO_ERROR,
    VENC_RESULT_BAD_VERSION,
    VENC_RESULT_BAD_SUBTYPE,
} vencrypt_result_t;

#define VENC_SUBTYPE_TLS_VNC 258u
#define VENC_SUBTYPE_X509_VNC 261u

/** Negotiate VeNCrypt 0.2 and return the selected TLS+VNC subtype. */
vencrypt_result_t vencrypt_server_negotiate(const vencrypt_io_t *io, uint32_t *subtype);
