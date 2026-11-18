/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's own speaker: an ES8311 codec and an amplifier on fixed pins
 * (Kconfig, Board pins, Speaker). It plays one short sound, a knock when a
 * console connects, and holds nothing in between.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Look for the codec once the I2C bus is up, and report the speaker capability. */
void kvm_speaker_init(void);

/** A console was opened: knock, if spk_knock is on and the last knock was 30 s ago. */
void kvm_speaker_console_joined(void);

#ifdef __cplusplus
}
#endif
