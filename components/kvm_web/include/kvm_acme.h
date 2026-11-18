/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A certificate a browser trusts with nothing imported: Let's Encrypt, for the
 * device's own Tailscale name (host.tailnet.ts.net). The control plane
 * publishes the DNS-01 challenge record for us, so the device needs no domain,
 * no DNS token and no open port.
 *
 * The certificate is served only to clients that ask for that name (SNI);
 * espkvm.local and the IP addresses keep the device's own certificate.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mbedtls/ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load the stored certificate and start the renewal check. Call before the web server. */
void kvm_acme_init(void);

/** Certificate selection hook for esp_https_server: serves ours for our name. */
int kvm_acme_sni_cb(mbedtls_ssl_context *ssl);

/** Get (or renew) the certificate now, in the background. @p staging uses the test CA. */
esp_err_t kvm_acme_run(bool staging);

/** Write the state as a JSON object into @p buf. */
void kvm_acme_status_json(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
