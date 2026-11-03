/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vencrypt_protocol.h"

static bool send_exact(const vencrypt_io_t *io, const uint8_t *data, size_t len)
{
    while (len) {
        const int n = io->send(io->ctx, data, len);
        if (n > 0) {
            if ((size_t)n > len) return false;
            data += n;
            len -= (size_t)n;
        } else if (n == VENC_IO_INTERRUPTED) {
            continue;
        } else if (n == VENC_IO_WANT_READ || n == VENC_IO_WANT_WRITE) {
            if (!io->wait(io->ctx, n == VENC_IO_WANT_WRITE)) return false;
        } else {
            return false;
        }
    }
    return true;
}

static bool recv_exact(const vencrypt_io_t *io, uint8_t *data, size_t len)
{
    while (len) {
        const int n = io->recv(io->ctx, data, len);
        if (n > 0) {
            if ((size_t)n > len) return false;
            data += n;
            len -= (size_t)n;
        } else if (n == VENC_IO_INTERRUPTED) {
            continue;
        } else if (n == VENC_IO_WANT_READ || n == VENC_IO_WANT_WRITE) {
            if (!io->wait(io->ctx, n == VENC_IO_WANT_WRITE)) return false;
        } else {
            return false;
        }
    }
    return true;
}

static uint32_t get_be32(const uint8_t p[4])
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

vencrypt_result_t vencrypt_server_negotiate(const vencrypt_io_t *io, uint32_t *subtype)
{
    if (!io || !io->send || !io->recv || !io->wait || !subtype) {
        return VENC_RESULT_IO_ERROR;
    }

    static const uint8_t version[] = {0, 2};
    uint8_t client_version[sizeof(version)];
    if (!send_exact(io, version, sizeof(version)) ||
        !recv_exact(io, client_version, sizeof(client_version))) {
        return VENC_RESULT_IO_ERROR;
    }
    if (client_version[0] != version[0] || client_version[1] != version[1]) {
        const uint8_t rejected = 1;
        (void)send_exact(io, &rejected, sizeof(rejected));
        return VENC_RESULT_BAD_VERSION;
    }

    /* No TLS bytes are consumed here: the next bytes belong to ClientHello. */
    static const uint8_t offer[] = {
        0,                   /* VeNCrypt version accepted */
        2,                   /* number of subtypes */
        0, 0, 1, 5,          /* X509Vnc (261) */
        0, 0, 1, 2,          /* TLSVnc (258) */
    };
    uint8_t selection[4];
    if (!send_exact(io, offer, sizeof(offer)) || !recv_exact(io, selection, sizeof(selection))) {
        return VENC_RESULT_IO_ERROR;
    }

    *subtype = get_be32(selection);
    if (*subtype != VENC_SUBTYPE_TLS_VNC && *subtype != VENC_SUBTYPE_X509_VNC) {
        return VENC_RESULT_BAD_SUBTYPE;
    }
    return VENC_RESULT_OK;
}
