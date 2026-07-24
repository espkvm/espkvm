/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

esp_err_t ethernet_init(void);

/**
 * Send a Wake-on-LAN magic packet to @p mac ("AA:BB:CC:DD:EE:FF") as a UDP
 * broadcast, to power on a target that has WoL enabled. Returns
 * ESP_ERR_INVALID_ARG for a malformed MAC.
 */
esp_err_t kvm_wol_send(const char *mac);
