/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional MQTT bridge: reports the device's live state to an MQTT broker and,
 * through Home Assistant's MQTT discovery, appears there as one device with a
 * handful of sensors (temperature, viewers, video mode, target power, ...) and
 * buttons (power/reset/force-off, Wake-on-LAN, restart). Everything is gated by
 * the mqtt_* settings and off by default, so a device that never enables it pays
 * nothing beyond the linked code.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the publish timer and internal state. Does not connect. */
esp_err_t kvm_mqtt_init(void);

/**
 * Reconcile the client with the mqtt_* settings: connect (or reconnect with new
 * parameters) when enabled and a broker host is set, otherwise stop and free the
 * client. Safe to call repeatedly - e.g. whenever a setting changes.
 */
esp_err_t kvm_mqtt_apply(void);

/**
 * Re-publish discovery and state now, if connected. Call after something that
 * changes which entities should exist (e.g. ATX became available) so Home
 * Assistant learns about it without waiting for the next reconnect.
 */
void kvm_mqtt_notify(void);

/**
 * Report the bridge's state for the UI. @p enabled is whether a client is
 * running at all (MQTT turned on with a broker set); @p connected is whether it
 * currently has a live session with the broker. Either pointer may be NULL.
 */
void kvm_mqtt_status(bool *enabled, bool *connected);

#ifdef __cplusplus
}
#endif
