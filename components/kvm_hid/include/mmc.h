/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The optical drive's side of SCSI: the MMC commands a CD/DVD drive answers
 * and a disk does not. TinyUSB handles INQUIRY, READ CAPACITY, READ(10) and the
 * other commands every drive has; these are the rest, which Windows, macOS and
 * some firmware ask an optical drive before they trust it.
 *
 * Plain C with no ESP-IDF in it, so tools/test.sh runs it on a host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What is in the drive. Blocks are 2048 bytes. */
typedef struct {
    bool present;
    uint64_t blocks;
    /* Anything that changes when another image goes in: a swap has to reach
       the host as "new media" even when the size is the same. */
    uint32_t id;
} mmc_media_t;

/* The media event the host has not been told about yet. Zeroed at start. */
typedef struct {
    bool seen;
    bool seen_present;
    uint32_t seen_id;
    uint8_t pending; /* MMC media event code, 0 = nothing */
} mmc_events_t;

typedef struct {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
} mmc_sense_t;

/* Above this an image is offered as a DVD: a CD holds at most ~870 MB. */
#define MMC_DVD_ABOVE_BLOCKS (900ull * 1024 * 1024 / 2048)

/* Not one of ours: the caller answers ILLEGAL REQUEST as before. */
#define MMC_NOT_MMC (-2)

/**
 * Answer one MMC command. @return the reply length written to @p out (at most
 * @p cap), 0 for a command with no data, -1 with @p sense filled in when the
 * command fails, or MMC_NOT_MMC.
 */
int32_t mmc_command(const uint8_t cdb[16], const mmc_media_t *m, mmc_events_t *ev,
                    uint8_t *out, uint32_t cap, mmc_sense_t *sense);
