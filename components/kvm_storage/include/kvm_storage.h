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

/**
 * Whether the device can write to the card. False on this board: the SD write
 * path times out at any clock fast enough to serve from, so images are prepared
 * in an external reader and the card is served read-only. The web layer uses
 * this to disable upload and delete rather than let them fail.
 */
bool kvm_storage_writable(void);

/** Human-readable reason the card is read-only, for the UI. */
const char *kvm_storage_write_unavailable_reason(void);

/* ---- virtual media: an image file exposed to the target over USB MSC ------
 *
 * One image file on the card is "inserted" at a time. The firmware keeps the
 * card mounted throughout and reads the file on the target's behalf, so there
 * is no USB<->app ownership handoff: uploads and target reads coexist. The
 * image is served read-only, which is what booting from it needs and which
 * keeps a booting target from corrupting the operator's file.
 *
 * FAT32 caps a single file at 4 GiB, so that is the largest image for now.
 */

typedef struct {
    bool present;         /**< an image is selected and open */
    bool writable;        /**< host may write (always false in this version) */
    uint32_t block_size;  /**< logical block size offered to the host, 512 */
    uint64_t block_count; /**< image size / block_size */
    char name[64];        /**< file name currently offered, empty when ejected */
} kvm_media_t;

/**
 * Insert an image: open @p name (a file in the mount point root, e.g.
 * "ubuntu.img") read-only and compute its geometry. Passing NULL or "" ejects.
 * @return ESP_OK, ESP_ERR_INVALID_STATE with no card, ESP_ERR_NOT_FOUND if the
 *         file is missing, or ESP_ERR_INVALID_SIZE if it is smaller than a block.
 */
esp_err_t kvm_storage_media_select(const char *name);

/** Eject: close the image; the host then sees the drive with no medium. */
void kvm_storage_media_eject(void);

void kvm_storage_media_info(kvm_media_t *out);

/**
 * Read @p len bytes at byte @p offset from the inserted image. Thread-safe
 * against selection and against itself. Missing bytes past end-of-file are
 * zero-filled, so a full block is always returned.
 * @return bytes read (== @p len on success), or -1 when nothing is inserted.
 */
int32_t kvm_storage_media_read(uint64_t offset, void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
