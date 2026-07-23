/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The microSD card: powered, mounted, and reported.
 *
 * This is the foundation the virtual-media feature stands on - a disk image
 * the target boots from has to live somewhere first. On its own it does
 * nothing the target can see; it makes the card readable and writable to the
 * firmware and says, honestly, whether there is a card at all.
 *
 * The slot is on a 4-bit SDIO bus and its power is gated by a GPIO, so a card
 * that is present but unpowered is indistinguishable from an empty slot until
 * the gate is opened - which is why powering the slot is the first thing this
 * does.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted;
    /** Card capacity and free space, in bytes. Zero when nothing is mounted. */
    uint64_t total_bytes;
    uint64_t free_bytes;
    /** Card name as reported over the bus, e.g. "SD32G". Empty when unmounted. */
    char name[24];
} kvm_storage_status_t;

/**
 * Power the slot, bring up the SDIO bus, and mount the card's FAT filesystem.
 * Safe to call when there is no card: it reports the slot empty and returns
 * without error, so start-up is never held up by a missing card.
 */
esp_err_t kvm_storage_init(void);

void kvm_storage_status(kvm_storage_status_t *out);

/** Where the card is mounted, e.g. "/sd". Valid whether or not one is present. */
const char *kvm_storage_mount_point(void);

#ifdef __cplusplus
}
#endif
