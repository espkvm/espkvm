/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional status OLED. A small I2C panel wired to the capture chip's I2C bus
 * (no dedicated pins) that shows the device's link and IP, capture status and
 * health at a glance - for a headless appliance in a rack. Off unless enabled in
 * settings, auto-detected on the shared bus, and a no-op when no panel answers,
 * so it is safe to build into every board.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the status-OLED task, once, at boot - after the capture path, so the
 * shared I2C bus exists. The task polls the oled_* settings and attaches the
 * panel when it is enabled and present, detaching when it is switched off. Safe
 * to call with the feature off or no panel wired: it just idles.
 */
void kvm_display_init(void);

#ifdef __cplusplus
}
#endif
