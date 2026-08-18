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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the status-display task, once, at boot. The task polls the disp_*
 * settings and attaches the panel when it is enabled and present, detaching
 * when it is switched off. Safe to call with the feature off or no panel wired:
 * it just idles.
 *
 * Call it early - it is what lets the panel show the start-up reset window.
 * The I2C panels need the capture chip's bus, which does not exist yet at that
 * point; the task retries on its own and picks them up once it does, so an
 * early call costs an OLED nothing but its first few seconds.
 */
void kvm_display_init(void);

/**
 * Take the panel over with a notice until it is cleared or it expires.
 *
 * A notice outranks the status rotation: it is for something happening right
 * now, in front of the device, that the person standing there is waiting on -
 * the start-up reset window is the first user. Status screens tell you what the
 * device is; a notice tells you what it is doing this second.
 *
 * @param title  a few characters, drawn large and kept stable across updates
 * @param detail one short line under it, the part that changes
 * @param pct    0..100 to draw progress, or -1 for a notice with none
 * @param ttl_ms how long it survives without another call, or 0 to keep it up
 *               until kvm_display_notice_clear(). A TTL means a caller that dies
 *               mid-way cannot strand the panel on a stale notice.
 *
 * Safe to call before the panel is up, with the feature off, or from a build
 * without display support - it is then a no-op. Cheap enough to call from a
 * polling loop: it only repaints when something actually changed.
 */
void kvm_display_notice(const char *title, const char *detail, int pct, uint32_t ttl_ms);

/** Drop any notice and go back to the status rotation. */
void kvm_display_notice_clear(void);

#ifdef __cplusplus
}
#endif
