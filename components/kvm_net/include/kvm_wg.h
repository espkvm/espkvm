/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional WireGuard tunnel, so the device is reachable over a VPN without
 * exposing it. Split-tunnel: only the device's own tunnel address routes through
 * WireGuard, so the console stays reachable on the LAN as well. Everything is
 * gated by the wg_* settings and off by default.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare internal state. Does not connect (the network is not up yet). */
esp_err_t kvm_wg_init(void);

/**
 * Reconcile the tunnel with the wg_* settings: connect (or reconnect with new
 * parameters) when enabled and configured, otherwise tear it down. Generates and
 * stores a private key on first use if none is set. Safe to call repeatedly.
 */
esp_err_t kvm_wg_apply(void);

typedef struct {
    bool enabled;         /**< the tunnel is configured and started */
    bool up;              /**< a handshake with the peer has completed */
    char address[40];     /**< the device's address on the tunnel (>= wg_address max_len) */
    char public_key[48];  /**< our public key (base64) to add to the peer */
} kvm_wg_status_t;

/** Fill @p out with the tunnel's current state. */
void kvm_wg_status(kvm_wg_status_t *out);

#ifdef __cplusplus
}
#endif
