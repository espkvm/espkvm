/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * The Wi-Fi co-processor's own firmware (the C6 or C5 running esp-hosted), and
 * installing the image this firmware carries into it over SDIO.
 */

typedef enum {
    KVM_COPROC_IDLE = 0,
    KVM_COPROC_UPDATING, /**< writing; percent says how far */
    KVM_COPROC_DONE,     /**< written; the device restarts in a few seconds */
    KVM_COPROC_FAILED,   /**< see msg; the old firmware still runs */
} kvm_coproc_state_t;

typedef struct {
    bool bundled;       /**< this build carries an image to install */
    bool running;       /**< the co-processor is up (a Wi-Fi mode is on) */
    char fw[16];        /**< its version; "" when it cannot say (an early build) */
    char bundled_fw[16];/**< the version of the image this build carries */
    bool update;        /**< the carried image is newer than what runs */
    kvm_coproc_state_t state;
    int percent;
    char msg[96];
} kvm_coproc_status_t;

/** Called by the Wi-Fi code once the co-processor answers: reads its version. */
void kvm_coproc_on_up(void);

void kvm_coproc_status(kvm_coproc_status_t *out);

/**
 * Install the carried image. Runs on a worker; poll kvm_coproc_status(). Needs
 * the co-processor up, so a Wi-Fi mode must be on. When it finishes the device
 * restarts, and the co-processor with it.
 */
esp_err_t kvm_coproc_update_start(void);
