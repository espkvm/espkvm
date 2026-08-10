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

/**
 * Hand the SD host controller back so something else can use the shared SDMMC
 * bus (on the ESP32-P4 the microSD and a WiFi co-processor's SDIO share one
 * controller). Ejects any exposed image and unmounts the card. @p was_mounted,
 * if not NULL, is set to whether a card was actually mounted, so the caller
 * knows whether to call kvm_storage_bus_resume() afterwards.
 */
esp_err_t kvm_storage_bus_suspend(bool *was_mounted);

/** Re-mount the microSD after a kvm_storage_bus_suspend(). */
esp_err_t kvm_storage_bus_resume(void);

void kvm_storage_status(kvm_storage_status_t *out);

/** Where the card is mounted, e.g. "/sd". Valid whether or not one is present. */
const char *kvm_storage_mount_point(void);

/**
 * Whether the device can write to the card. On pre-3.0 silicon this is always
 * false: the SD write path times out at any clock fast enough to serve from, so
 * images are prepared in an external reader and the card is served read-only. On
 * rev >= 3.0 the controller writes reliably, so this is true whenever a card is
 * mounted. The web layer uses it to enable/disable upload and delete.
 */
bool kvm_storage_writable(void);

/** Human-readable reason writing is unavailable (NULL when writable), for the UI. */
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
    bool cdrom;           /**< served as a CD-ROM (2048-byte, PDT 0x05) not a disk */
    uint32_t block_size;  /**< logical block size offered to the host, 512 or 2048 */
    uint64_t block_count; /**< image size / block_size */
    char name[64];        /**< file name currently offered, empty when ejected */
} kvm_media_t;

/**
 * Insert an image: open @p name (a file in the mount point root, e.g.
 * "ubuntu.img") read-only and compute its geometry. Passing NULL or "" ejects.
 * @p cdrom presents it as a CD-ROM (2048-byte blocks, for .iso images) rather
 * than a removable disk (512-byte blocks). The caller decides the type, since it
 * knows the file name; the storage layer only serves whatever it is told.
 * @return ESP_OK, ESP_ERR_INVALID_STATE with no card, ESP_ERR_NOT_FOUND if the
 *         file is missing, or ESP_ERR_INVALID_SIZE if it is smaller than a block.
 */
esp_err_t kvm_storage_media_select(const char *name, bool cdrom);

/**
 * Expose the whole microSD card to the target as a removable flash drive - every
 * file on it, not one image - by serving its raw sectors. Read-only like the rest.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when no card is mounted.
 */
esp_err_t kvm_storage_media_select_whole_sd(void);

/** Eject: close the image; the host then sees the drive with no medium. */
void kvm_storage_media_eject(void);

void kvm_storage_media_info(kvm_media_t *out);

/* ---- built-in rescue image (on-flash) -------------------------------------
 *
 * A small bootable image - iPXE, memtest, a DOS floppy - kept in a flash
 * partition and served to the target over the same USB drive as the card
 * images. It needs no microSD, reads fast because flash is memory-mapped, and
 * unlike the card it can be (re)written from the console, since this board's
 * flash writes are reliable where its SD writes are not. It is small by design:
 * a full OS image belongs on the card. Absent on a device whose partition table
 * predates the feature.
 */
typedef struct {
    bool supported;         /**< the rescue partition exists on this device */
    bool has_image;         /**< it holds an image (its first sector is written) */
    uint64_t capacity_bytes; /**< partition size, the largest image it can hold */
} kvm_rescue_t;

void kvm_storage_rescue_status(kvm_rescue_t *out);

/**
 * Insert the built-in rescue image as the USB drive. Coexists with the card:
 * this simply makes the flash image the inserted medium instead of a file.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED with no rescue partition, or
 *         ESP_ERR_NOT_FOUND when the partition is present but empty.
 */
esp_err_t kvm_storage_media_select_rescue(bool cdrom);

/*
 * Write a new image into the rescue partition, streamed like a firmware update.
 * Unlike the microSD, this board's flash writes are reliable, so this is offered
 * from the console. begin() ejects the rescue image if it is currently served,
 * erases what @p total needs, and locks out a second writer; write() appends;
 * end() finishes; abort() releases the lock on a failed transfer.
 */
esp_err_t kvm_storage_rescue_write_begin(size_t total);
esp_err_t kvm_storage_rescue_write(const void *buf, size_t len);
esp_err_t kvm_storage_rescue_write_end(void);
void kvm_storage_rescue_write_abort(void);

/**
 * Read @p len bytes at byte @p offset from the inserted image. Thread-safe
 * against selection and against itself. Missing bytes past end-of-file are
 * zero-filled, so a full block is always returned.
 * @return bytes read (== @p len on success), or -1 when nothing is inserted.
 */
int32_t kvm_storage_media_read(uint64_t offset, void *buf, uint32_t len);

/**
 * Whether the target may write to the inserted medium. Only the whole-card
 * passthrough is writable, and only on rev >= 3.0 silicon (where SD writes are
 * reliable); images, the rescue partition and CD-ROM media are always read-only.
 */
bool kvm_storage_media_writable(void);

/**
 * Write @p len bytes at byte @p offset to the inserted medium. Only valid when
 * kvm_storage_media_writable() is true (the whole-card passthrough). Thread-safe.
 * @return bytes written (== @p len), or -1 if the medium is not writable or the
 *         card write failed.
 */
int32_t kvm_storage_media_write(uint64_t offset, const void *buf, uint32_t len);

/**
 * Whether the whole card is currently handed to the target as a read-write drive.
 * While it is, the firmware keeps off the filesystem (uploads and the file list
 * are disabled) so the target owns the card alone; kvm_storage_reread() re-reads
 * it when the operator takes it back.
 */
bool kvm_storage_card_handed_over(void);

/**
 * Re-read the card's filesystem after the target had write access to it (see the
 * whole-card passthrough). Unmounts and remounts so the firmware's view reflects
 * anything the target wrote. A no-op if the card was never handed over.
 */
void kvm_storage_reread(void);

#ifdef __cplusplus
}
#endif
