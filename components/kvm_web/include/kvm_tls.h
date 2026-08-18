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

#include <stdbool.h>
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
    char *ca_pem;    /**< the CA certificate to import, or NULL for an operator-supplied cert */
    size_t ca_len;
    bool byo;        /**< true when this is the operator's own certificate, not the self-signed one */
} kvm_tls_identity_t;

/**
 * Load the stored identity, generating one if there is none or if the stored
 * one no longer matches the configured hostname.
 *
 * Generation takes a few hundred milliseconds and happens once in the device's
 * life, so it is done inline at start-up rather than in the background: TLS
 * cannot start without it anyway.
 *
 * When @p allow_byo is true and the operator has installed their own
 * certificate (see kvm_tls_byo_set), that one is used instead of the self-signed
 * identity, with @p out->byo set and @p out->ca_pem left NULL. Pass false to
 * force the self-signed identity - the web server uses this as a fallback if the
 * operator's certificate is rejected by the TLS stack, so a bad upload can never
 * leave the console unreachable.
 *
 * @return ESP_OK with @p out filled, and the caller owning both buffers.
 */
esp_err_t kvm_tls_identity_get(kvm_tls_identity_t *out, bool allow_byo);

void kvm_tls_identity_free(kvm_tls_identity_t *id);

/** Discard the stored self-signed identity so the next start generates a new one. */
esp_err_t kvm_tls_identity_reset(void);

/**
 * Record the device's Tailscale identity (100.x address and MagicDNS FQDN, either
 * may be "") so the self-signed leaf certificate names them and is therefore valid
 * when the console is reached over the tailnet. Persisted, so from the next boot
 * the leaf carries them without waiting for the tailnet.
 *
 * @return true if the stored values changed - the caller should restart so the
 *         leaf is re-issued and served (the certificate is only applied at boot).
 */
bool kvm_tls_set_tailnet(const char *ip, const char *fqdn);

/** Room for the longest textual IPv6 address plus its terminator. */
#define KVM_TLS_IP6_MAX 46

/**
 * Record the routable IPv6 addresses the device is answering on, so the leaf
 * certificate names them and https://[address]/ is trusted like the v4 literal.
 * Both kinds are named: @p global is what reaches the device from outside, @p ula
 * the unique-local address that survives the ISP handing out a new prefix - the
 * one worth bookmarking, and so the one that must not be left out. Either may be
 * "" for none.
 *
 * Persisted; unlike the tailnet identity this does NOT ask for a restart, because
 * an autoconfigured address arrives seconds into every boot and a KVM that
 * restarts itself at that moment is worse than a certificate that picks the
 * address up next time.
 *
 * @return true if the stored values changed.
 */
/**
 * Record the IPv4 address the device was given by DHCP, so the certificate can
 * name it from the next restart. Returns true if it changed anything.
 *
 * A no-op when the address is static: that one is named from the setting
 * already, and is known before the network is even up.
 */
bool kvm_tls_set_ip4(const char *ip);

bool kvm_tls_set_ip6(const char *global, const char *ula);

/**
 * Install an operator-supplied certificate and matching private key (both PEM),
 * used in place of the self-signed identity from the next start. The pair is
 * validated (both parse, and the key matches the certificate) before it is
 * stored; on failure nothing is written and @p err carries a short reason.
 * @p cert_pem may be a chain (leaf first). Takes effect after a restart.
 */
esp_err_t kvm_tls_byo_set(const char *cert_pem, const char *key_pem, char *err, size_t errlen);

/** Remove any operator-supplied certificate, reverting to the self-signed one. */
esp_err_t kvm_tls_byo_clear(void);

/** True when an operator-supplied certificate is installed. */
bool kvm_tls_byo_present(void);

#ifdef __cplusplus
}
#endif
