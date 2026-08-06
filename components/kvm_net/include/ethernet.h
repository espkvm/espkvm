/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ethernet_init(void);

/**
 * Advertise the console over mDNS as <hostname>.local, under the service the
 * device actually serves (_https or _http). Called by whichever interface
 * becomes the active link - Ethernet or WiFi.
 */
void kvm_net_advertise(const char *hostname);

/**
 * Current Ethernet link state. @p up is set to whether the cable is up, @p mbps
 * to the negotiated speed (10 or 100, 0 when down). Either pointer may be NULL.
 */
void kvm_eth_link(bool *up, int *mbps);

/**
 * Send a Wake-on-LAN magic packet to @p mac ("AA:BB:CC:DD:EE:FF") as a UDP
 * broadcast, to power on a target that has WoL enabled. Returns
 * ESP_ERR_INVALID_ARG for a malformed MAC.
 */
esp_err_t kvm_wol_send(const char *mac);
