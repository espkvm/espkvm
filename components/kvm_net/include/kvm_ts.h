/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional native Tailscale membership via the bundled microlink client, so the
 * device joins a tailnet directly - no separate gateway, NAT traversal handled -
 * and is reachable at its 100.x address from anywhere on the mesh. Everything is
 * gated by the ts_* settings and off by default. Unlike WireGuard this needs the
 * network to be up before it starts, so bring-up waits for the Ethernet IP.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare internal state and start watching for the network to come up. */
esp_err_t kvm_ts_init(void);

/**
 * Reconcile the tailnet session with the ts_* settings: join (or rejoin with new
 * parameters) when enabled and configured and the network is up, otherwise tear
 * it down. Safe to call repeatedly; the slow work runs on a worker task.
 */
esp_err_t kvm_ts_apply(void);

typedef struct {
    bool enabled;       /**< the client is configured and started */
    bool up;            /**< registered with the control plane and ready */
    char address[24];   /**< our tailnet IP (100.x.y.z), empty until assigned */
    int  peers;         /**< number of known tailnet peers */
    /** When the node key runs out, in Unix epoch seconds; 0 when the control
     *  plane reported none (key expiry switched off) or has not answered yet.
     *  Tailscale allows six months at most, so this is a date worth watching. */
    int64_t key_expiry;
    bool key_expired;   /**< the control plane says the key is already out */
    char name[128];     /**< our MagicDNS name, "" when MagicDNS is off */
    /** The name the control plane will help certify (DNSConfig.CertDomains);
     *  "" when HTTPS certificates are off for the tailnet. */
    char cert_domain[128];
    /** While the device waits for a person to approve it (no auth key): the
     *  URL to open. "" otherwise. */
    char login_url[160];
    /** Why the control plane refused the device, "" if it did not. */
    char error[96];
    char derp[8];       /**< our home relay region, e.g. "fra"; "" before the map */
    bool derp_up;       /**< the relay connection is up */
} kvm_ts_status_t;

/** One tailnet peer, as the console shows it. */
typedef struct {
    char ip[16];        /**< 100.x.y.z */
    char name[64];      /**< MagicDNS name */
    char user[64];      /**< owner's login, "" when unknown */
    char user_display[48];
    int  online;        /**< the netmap's view: 1, 0, -1 unknown */
    bool direct;        /**< traffic goes straight, not through the relay */
    char endpoint[24];  /**< the direct address in use, "" over the relay */
    char derp[8];       /**< the peer's home relay region */
    unsigned rtt_ms;    /**< last probe round trip, 0 = none yet */
    bool rtt_direct;    /**< that probe went straight */
} kvm_ts_peer_t;

/** Number of peers kvm_ts_peer() can return. */
int kvm_ts_peer_count(void);

/** Fill @p out with peer @p index; false past the end or when not running. */
bool kvm_ts_peer(int index, kvm_ts_peer_t *out);

/** The peer with tailnet address @p ip, if any. */
bool kvm_ts_peer_by_ip(const char *ip, kvm_ts_peer_t *out);

/**
 * Leave the tailnet: forget this device's keys and its auth key, so the next
 * start registers as a new device (and with no key, asks for a login).
 */
esp_err_t kvm_ts_logout(void);

/** Fill @p out with the tailnet client's current state. */
void kvm_ts_status(kvm_ts_status_t *out);

/**
 * Publish a TXT record through the control plane, for an ACME DNS-01 challenge
 * on our own tailnet name. Blocks until the control plane answers (up to a
 * minute). @p err gets a short reason on failure.
 */
esp_err_t kvm_ts_set_txt(const char *name, const char *value, char *err, size_t err_len);

/**
 * Callback invoked once the tailnet is up with the device's 100.x address and
 * MagicDNS name (either may be ""), so another component (the TLS layer) can name
 * them in the console certificate. Returning true means the values were new and
 * the device should restart to re-issue the certificate. Registered by the
 * composition root to avoid a dependency cycle with the web/TLS component.
 */
typedef bool (*kvm_ts_identity_cb_t)(const char *ip, const char *fqdn);
void kvm_ts_set_identity_cb(kvm_ts_identity_cb_t cb);

/**
 * Handler for requests the control server sends to the device itself, such as
 * the Tailscale admin console asking for the certificate's state. Gets the
 * method and the path with its query, writes a JSON body, returns the HTTP
 * status. Registered by the web component (same cycle as above).
 */
typedef int (*kvm_ts_c2n_cb_t)(const char *method, const char *path, char *body, size_t cap);
void kvm_ts_set_c2n_cb(kvm_ts_c2n_cb_t cb);

#ifdef __cplusplus
}
#endif
