/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * An update on trial.
 *
 * Normally a new image confirms itself once the web server is up. That proves
 * the device can serve a page, not that the person who updated it can still
 * reach it: a broken VPN, Wi-Fi or TLS leaves a device that looks fine from the
 * inside and is lost from the outside. An update installed "on trial" stays
 * unconfirmed until a console says it works. If nobody does within the time
 * given, the device goes back to the previous image.
 */
#pragma once

#include "esp_err.h"
#include "esp_partition.h"

#include <stdbool.h>
#include <stdint.h>

/** Shortest and longest trial accepted, in seconds. */
#define KVM_OTA_TRIAL_MIN_S 60
#define KVM_OTA_TRIAL_MAX_S 3600

/**
 * Remember that the image just written to @p target goes on trial for
 * @p seconds once it boots. 0 clears any earlier request. Call it before
 * esp_ota_set_boot_partition(), so a failure here can still stop the update.
 */
esp_err_t kvm_ota_trial_arm(const esp_partition_t *target, uint32_t seconds);

/**
 * At boot, once the web server is up. Starts the trial when the running image
 * is a fresh update that was asked to be on trial.
 *
 * @return true when a trial runs: the caller must not confirm the image.
 */
bool kvm_ota_trial_begin(void);

/** Seconds until the rollback, 0 when no trial runs. */
uint32_t kvm_ota_trial_left(void);

/** The operator says the update works: confirm it and stop the clock. */
esp_err_t kvm_ota_trial_keep(void);

/**
 * The operator says it does not: boot the previous image next. The caller
 * restarts the device.
 */
void kvm_ota_trial_rollback(void);
