/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * An optional battery-backed clock (DS3231) on the capture I2C bus, so the
 * device knows the time after a restart with no network to ask - which signed
 * file names and two-factor codes both need. Without one, nothing changes.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Look for the clock chip the settings ask for (rtc_chip, rtc_bus, rtc_sda,
 * rtc_scl) - on @p bus, the capture board's, unless it is wired to pins of its
 * own. If it holds a valid time and the system clock
 * is unset, set the system clock from it. Then keep it in step: once the
 * system clock is right (NTP, or a browser at sign-in), the time is written
 * back to the chip. Call once, as early as the bus exists.
 */
void kvm_rtc_init(i2c_master_bus_handle_t bus);

/** A clock chip answered on the bus. */
bool kvm_rtc_present(void);

/** Which one: "DS3231", "PCF8563", "PCF85063", "PCF8523", or "" without one. */
const char *kvm_rtc_name(void);

/** The clock chip's thermometer (the air by the board), read once a minute. */
bool kvm_rtc_temperature(float *celsius);

#ifdef __cplusplus
}
#endif
