/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A finished recording, from transport stream to MP4, without re-encoding.
 *
 * A .ts is what survives a pulled card while it is being written; an MP4 is
 * what plays in a browser, in a phone's player and inside a Telegram chat. So a
 * dashcam clip is recorded as .ts and turned into an MP4 once it is complete.
 * The index goes at the front of the file, so it starts playing before it has
 * all arrived, and the moments that made the clip become chapters.
 *
 * Reads the .ts twice - once for the sample table, once for the samples - so
 * memory stays a few kilobytes per minute of video. Plain C over callbacks, so
 * it is tested on a host. Only reads what ts_mux writes: one H.264 access unit
 * per PES packet, on one PID.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Fill up to @p cap bytes; return how many, 0 at the end. */
    size_t (*read)(void *io, uint8_t *buf, size_t cap);
    /** Back to the start of the .ts. */
    bool (*rewind)(void *io);
    bool (*write)(void *io, const uint8_t *data, size_t len);
    void *io;
} mp4_io_t;

typedef struct {
    int64_t at_us; /**< from the first frame of the file */
    const char *title;
} mp4_chapter_t;

/** @return true when the MP4 was written whole; otherwise @p err says why. */
bool ts_to_mp4(const mp4_io_t *io, uint32_t width, uint32_t height, const mp4_chapter_t *chapters,
               int chapter_count, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
