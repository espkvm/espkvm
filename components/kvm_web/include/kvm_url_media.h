/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual media from a URL: the target boots an image that stays on a web
 * server, a NAS for one. Nothing is written to the card. The device reads the
 * parts the target asks for with HTTP Range requests, so the server has to
 * support them (nginx, Apache, Caddy and most NAS servers do).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Serve the image at @p url as the medium, or stop with an empty @p url.
 * Returns at once. The connection is made on a task of its own and tried
 * again while it fails, so a URL picked before the network is up still
 * arrives. Whatever medium was in the drive is ejected first.
 */
void kvm_url_media_select(const char *url, bool cdrom);

typedef struct {
    const char *state;  /**< "off", "connecting", "ready", "error" */
    char message[96];   /**< why it failed, for "error" */
    uint64_t size;      /**< bytes, once "ready" */
} kvm_url_media_status_t;

void kvm_url_media_status(kvm_url_media_status_t *out);

#ifdef __cplusplus
}
#endif
