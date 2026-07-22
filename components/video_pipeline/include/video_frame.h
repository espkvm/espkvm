/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The published frame.
 *
 * One encoder writes, several readers send. The buffers belong to whichever
 * codec is running - MJPEG needs megabytes per slot, H.264 needs a fraction of
 * that - so the store holds pointers rather than allocations, and a codec
 * hands its buffers over when it opens and takes them back when it closes.
 *
 * Slots exist so that an encode can proceed while a reader is still writing an
 * earlier frame to a slow socket. A slot is writable when no reader holds it
 * and it is not the one currently published.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_SLOT_COUNT 3

/**
 * What the buffers hold. These values travel in the video WebSocket header, so
 * they are part of the protocol and must not be renumbered.
 */
typedef enum {
    VIDEO_PAYLOAD_NONE = 0,
    VIDEO_PAYLOAD_JPEG = 1,
    VIDEO_PAYLOAD_H264 = 2, /**< Annex-B; SPS and PPS precede every IDR */
} video_payload_t;

/** A published frame held against reuse until released. */
typedef struct {
    int slot;
    const uint8_t *data;
    size_t len;
    uint32_t seq;
    video_payload_t payload;
    /** Decodable on its own: JPEG always, H.264 only on an IDR. */
    bool keyframe;
} video_frame_ref_t;

/** Mutexes and semaphores. Call once, before the web server starts. */
void video_frame_store_init(void);

/** False until the store has been initialised and a codec has installed buffers. */
bool video_frame_store_ready(void);

/**
 * Hand the store a codec's output buffers.
 * @param bufs  @p count pointers, kept by the store until the next install
 * @param cap   bytes usable in each buffer
 */
void video_frame_install(video_payload_t payload, uint8_t *const *bufs, int count, size_t cap);

/**
 * Stop publishing and wait for readers to let go, so a codec can free its
 * buffers. Readers see "no frame yet" from here until the next install.
 * @return false when a reader still holds a frame after @p timeout_ms, in
 *         which case the buffers must not be freed.
 */
bool video_frame_quiesce(uint32_t timeout_ms);

/**
 * Claim a slot to encode into. Blocks while every slot is held by a reader.
 * @return ESP_ERR_TIMEOUT when none came free, ESP_ERR_INVALID_STATE with no
 *         buffers installed.
 */
esp_err_t video_frame_begin_write(int *out_slot, uint8_t **out_buf, size_t *out_cap,
                                  uint32_t timeout_ms);

/** Make the slot written by @ref video_frame_begin_write the published frame. */
void video_frame_publish(int slot, size_t len, bool keyframe);

/**
 * Whether the published frame is byte-identical to @p data. Used to drop a
 * re-encode of a screen that has not changed.
 */
bool video_frame_front_matches(const uint8_t *data, size_t len);

/** Take a reference to the published frame. @return false when there is none. */
bool video_frame_acquire(video_frame_ref_t *out);
void video_frame_release(const video_frame_ref_t *ref);

/** Sequence number of the published frame; advances on every publish. */
uint32_t video_frame_seq(void);

/** What the installed codec produces, without holding a frame to find out. */
video_payload_t video_frame_payload(void);

/** Wait for a frame newer than @p seen. @return false on timeout. */
bool video_frame_wait_new(uint32_t seen, uint32_t timeout_ms);

/*
 * Viewers. The encoder runs only while someone is watching: a KVM is idle most
 * of its life, and encoding 1080p into a buffer nobody reads costs the same
 * PSRAM bandwidth as encoding one that is watched.
 */
void video_frame_viewer_enter(void);
void video_frame_viewer_leave(void);
int video_frame_viewer_count(void);

/**
 * Ask for the next frame to be decodable on its own. A viewer that joins
 * mid-stream cannot start on an H.264 P-frame; MJPEG ignores this.
 */
void video_frame_request_keyframe(void);

/** For the codec: consume a pending request. */
bool video_frame_take_keyframe_request(void);

#ifdef __cplusplus
}
#endif
