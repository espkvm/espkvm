/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A TESmart KVM switch over the network, for kvm_atx.c. See kvmsw_tesmart.c.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Start or stop talking to the switch at @p host : @p port. */
esp_err_t kvmsw_tesmart_apply(bool on, const char *host, int port, int ports);

/** @p machine from 0, or -1 before the switch has answered. */
void kvmsw_tesmart_status(int *machine, bool *busy, bool *error);

/** Ask for @p machine (from 0). Returns at once. */
esp_err_t kvmsw_tesmart_select(int machine);
