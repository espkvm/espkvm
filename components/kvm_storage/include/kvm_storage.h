/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
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
#include "kvm_media.h"

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
    /** Card clock the bus runs at now, in kHz; it only goes down after errors. */
    uint32_t bus_khz;
    /** Failed transfers since the mount (each was retried). */
    uint32_t bus_errors;
    /** The clock it started at, and climbs back to after errors. */
    uint32_t bus_max_khz;
    /** Seconds until it tries one step faster again; 0 when not slowed. */
    uint32_t bus_retry_s;
} kvm_storage_status_t;

/**
 * Power the slot, bring up the SDIO bus, and mount the card's FAT filesystem.
 * Safe to call when there is no card: it reports the slot empty and returns
 * without error, so start-up is never held up by a missing card.
 */
esp_err_t kvm_storage_init(void);

/** SDMMC slot the card uses: 0 when it sits on the slot 0 pins, else 1. */
int kvm_storage_sd_slot(void);

/**
 * True when the card and the WiFi co-processor need the same SDMMC slot, so
 * only one of them can run. False on a board without WiFi.
 */
bool kvm_storage_shares_wifi_slot(void);

/**
 * The controller's other slot is in use (the WiFi co-processor). Speed probes
 * stop then: a probe that fails resets the controller, which would take that
 * slot down with it.
 */
void kvm_storage_set_other_slot_busy(bool busy);

/**
 * Called when a card appears in the slot or stops answering, so the caller can
 * offer the chosen medium to the target or show the drive as empty. Runs on the
 * storage task, not in an interrupt.
 */
void kvm_storage_set_slot_changed_cb(void (*cb)(void));

/**
 * Register a callback run just before the card's filesystem goes away - the
 * card is pulled out, handed whole to the target, re-read, or the SD host goes
 * to the WiFi chip - so open files can be closed first. It may block until they
 * are. @p why says which, for the log and the console.
 */
void kvm_storage_set_fs_leaving_cb(void (*cb)(const char *why));

/**
 * Hand the SD slot back so a WiFi co-processor on the same slot can use it.
 * Ejects any exposed image and unmounts the card. @p was_mounted,
 * if not NULL, is set to whether a card was actually mounted, so the caller
 * knows whether to call kvm_storage_bus_resume() afterwards.
 */
esp_err_t kvm_storage_bus_suspend(bool *was_mounted);

/** Re-mount the microSD after a kvm_storage_bus_suspend(). */
esp_err_t kvm_storage_bus_resume(void);

void kvm_storage_status(kvm_storage_status_t *out);

/** Where the card is mounted, e.g. "/sd". Valid whether or not one is present. */
const char *kvm_storage_mount_point(void);

/*
 * Files by a path relative to the card's root ("ubuntu.iso", "VIDEO/a.ts"), in
 * 64 bits. stdio cannot do that: off_t is 32-bit here, so stat() and fseek()
 * break at 2 GB, and exFAT files may pass 4 GB.
 */

/** Size of a regular file. Fails for a directory or a path with "..". */
esp_err_t kvm_storage_file_size(const char *rel, uint64_t *size);

typedef struct kvm_file kvm_file_t;

/** Open for reading at byte @p at. NULL when missing or out of memory. */
kvm_file_t *kvm_storage_file_open(const char *rel, uint64_t at);
/** Read up to @p len bytes. Returns the count, 0 at the end, -1 on error. */
int kvm_storage_file_read(kvm_file_t *f, void *buf, size_t len);
void kvm_storage_file_close(kvm_file_t *f);

/**
 * Whether the device can write to the card: a card is mounted, it is not handed
 * to the target, and the chip is rev >= 3.0 or the board powers the slot's IO
 * from its LDO. A pre-3.0 chip without that LDO times out on writes, so its card
 * stays read-only. The web layer uses this to enable upload and delete.
 */
bool kvm_storage_writable(void);

/** Human-readable reason writing is unavailable (NULL when writable), for the UI. */
const char *kvm_storage_write_unavailable_reason(void);

/* ---- sources for virtual media (kvm_media) ---------------------------------
 *
 * The card can back the target's USB drive in two ways: one image file, read
 * by the firmware on the target's behalf so uploads and target reads coexist,
 * or the whole card as raw sectors. Each call makes a source to hand to
 * kvm_media_insert(). A card source leaves the drive by itself when the card
 * goes away.
 *
 * On exFAT an image may pass 4 GiB. The browser upload cannot make one that big
 * (HTTP body sizes are 32-bit), so such a file is copied onto the card elsewhere.
 */

/**
 * An image file in the card's root, e.g. "ubuntu.iso", served read-only, as a
 * CD-ROM when @p cdrom (2048-byte blocks), else as a removable disk.
 * @return ESP_OK, ESP_ERR_INVALID_STATE with no card, ESP_ERR_INVALID_ARG for a
 *         name with a slash, or ESP_ERR_NOT_FOUND if the file is missing.
 */
esp_err_t kvm_storage_source_image(const char *name, bool cdrom, kvm_media_source_t *out);

/**
 * The whole microSD card as a removable flash drive: every file on it, not one
 * image. Writable by the target where the card can be written at all (see
 * kvm_storage_writable). From here the firmware keeps off the filesystem until
 * kvm_storage_reread().
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when no card is mounted.
 */
esp_err_t kvm_storage_source_whole_card(kvm_media_source_t *out);

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
 * The built-in rescue image as a source, read-only. Coexists with the card.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED with no rescue partition,
 *         ESP_ERR_NOT_FOUND when it is empty, or ESP_ERR_INVALID_STATE while it
 *         is being written.
 */
esp_err_t kvm_storage_source_rescue(bool cdrom, kvm_media_source_t *out);

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
