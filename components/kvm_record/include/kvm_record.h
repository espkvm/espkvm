/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Recording the screen to the microSD card, and saving screenshots there.
 *
 * A recording is the H.264 stream the viewers already get, written as .ts files
 * into VIDEO/ - nothing is encoded twice. A screenshot is one JPEG in
 * SCREENSHOTS/, taken on the device, so it works on either codec.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_RECORD_DIR "VIDEO"
#define KVM_SCREENSHOT_DIR "SCREENSHOTS"

typedef struct {
    bool recording;
    /** File being written, relative to the card, e.g. "VIDEO/20260917-140322.ts". */
    char file[64];
    uint32_t seconds;
    uint64_t bytes;
    uint32_t frames;
    /** Frames left out: the card fell behind, or the stream skipped one. */
    uint32_t dropped;
    /** Why the last recording ended, or "" if none has. */
    char stopped[96];
    /** The recording is a dashcam clip. */
    bool event;
    /** The dashcam is keeping the last minutes in memory. */
    bool dashcam;
    /** The dashcam is switched on but this board has too little PSRAM for it. */
    bool dashcam_no_memory;
    /** How many seconds of the past it holds right now. */
    uint32_t preroll_seconds;
    /** A timelapse keeps one frame every this many seconds; 0 for anything else. */
    uint32_t timelapse_every;
    /** The recording has the target's sound. */
    bool audio;
    /** While a clip is being saved: seconds of screen still to come. */
    uint32_t clip_seconds_left;
    /** Clips written but still being turned into MP4. */
    uint32_t clips_converting;
    /** The last clip that became an MP4, e.g. "VIDEO/20260917-160801-event.mp4". */
    char last_clip[64];
} kvm_record_status_t;

/** Hook the recorder to the card's comings and goings. Call once at start-up. */
void kvm_record_init(void);

/** Start the dashcam's watch for events. Call once, after settings are loaded. */
void kvm_record_dashcam_init(void);

/** Why a recording cannot start right now, or NULL when it can. */
const char *kvm_record_blocked(void);

/**
 * Start recording. It stops by itself after @p max_seconds, or after the
 * rec_max_min setting when that is 0. On failure @p why holds the reason.
 */
esp_err_t kvm_record_start(uint32_t max_seconds, char *why, size_t why_cap);

#define KVM_TIMELAPSE_EVERY_MAX 3600

/**
 * Start a timelapse: one frame every @p every_s seconds, played back at 25 fps,
 * so an hour at one frame in 10 s plays in 14 s. Frames are the stream's
 * keyframes, which come every two seconds or so; a shorter interval gets those.
 * @p max_seconds of 0 runs until stopped or the card is full. Stopped like any
 * recording.
 */
esp_err_t kvm_record_start_timelapse(uint32_t every_s, uint32_t max_seconds, char *why, size_t why_cap);

/** Stop and close the file; returns once it is closed. @p reason is kept for status. */
void kvm_record_stop(const char *reason);

bool kvm_record_active(void);

void kvm_record_status(kvm_record_status_t *out);

/**
 * Something worth keeping happened: save a dashcam clip of it, titled @p what.
 * While a clip is already being saved the event becomes a chapter in it and
 * the clip runs longer. False (with @p why) when there is no clip to make -
 * the dashcam is off, or an ordinary recording holds the card.
 */
bool kvm_record_event(const char *what, char *why, size_t why_cap);

/** Why a screenshot cannot be saved right now, or NULL when it can. */
const char *kvm_record_screenshot_blocked(void);

/**
 * Take a screenshot and save it to the card. @p file gets its path relative to
 * the card; on failure @p why holds the reason.
 */
esp_err_t kvm_record_screenshot(char *file, size_t file_cap, char *why, size_t why_cap);

#ifdef __cplusplus
}
#endif
