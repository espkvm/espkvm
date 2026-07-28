/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device's own TLS identity.
 *
 * A KVM ships with nobody to issue it a certificate, and an operator who must
 * supply a PEM before the web interface works has no way in at all. So the
 * device generates a self-signed one on first boot and keeps it in NVS. The
 * browser will warn once, which is the honest state of affairs: nothing has
 * vouched for this device except the device.
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The server's certificate chain, its private key, and the CA to import. */
typedef struct {
    char *cert_pem;  /**< leaf followed by the CA (the chain served to clients) */
    size_t cert_len; /**< including the terminator, as esp_https_server wants */
    char *key_pem;   /**< the leaf's private key */
    size_t key_len;
    char *ca_pem;    /**< the CA certificate alone - what an operator imports to trust the device */
    size_t ca_len;
} kvm_tls_identity_t;

/**
 * Load the stored identity, generating one if there is none or if the stored
 * one no longer matches the configured hostname.
 *
 * Generation takes a few hundred milliseconds and happens once in the device's
 * life, so it is done inline at start-up rather than in the background: TLS
 * cannot start without it anyway.
 *
 * @return ESP_OK with @p out filled, and the caller owning both buffers.
 */
esp_err_t kvm_tls_identity_get(kvm_tls_identity_t *out);

void kvm_tls_identity_free(kvm_tls_identity_t *id);

/** Discard the stored identity so the next start generates a new one. */
esp_err_t kvm_tls_identity_reset(void);

#ifdef __cplusplus
}
#endif
