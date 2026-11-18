/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between the recorder and the dashcam, inside this component.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RECORD_CHAPTERS_MAX 16
#define RECORD_CHAPTER_TITLE 48

typedef struct {
    int64_t at_us;
    char title[RECORD_CHAPTER_TITLE];
} record_chapter_t;

/** What a finished recording left behind, handed to the dashcam. */
typedef struct {
    char file[64];  /* the first part, e.g. "VIDEO/20260917-140322-event.ts" */
    int parts;
    bool event;     /* a dashcam clip */
    bool timelapse;
    uint64_t bytes;
    /* A clip from the dashcam on the card: segments seg_first..seg_last, joined
     * into file (with .mp4 for .ts). They stay on the card until
     * record_unpin_segments(seg_first). */
    bool segments;
    uint32_t seg_first, seg_last;
    int64_t t0_us;  /* time of the first frame of the first part */
    uint32_t width, height;
    int chapters;
    record_chapter_t chapter[RECORD_CHAPTERS_MAX];
    char stopped[96];
} record_finished_t;

/** Keep the frame reader running, and the last minutes in memory, while on. */
void record_set_dashcam(bool on);

/**
 * Keep the sound's encoder running for the dashcam in memory while audio is
 * on, so a clip's past has sound too. Called once a second: audio can be
 * switched on or off at any time.
 */
void record_dashcam_audio(void);

/** How many seconds of the past the ring holds now. */
uint32_t record_preroll_seconds(void);

/**
 * Save a clip: the last @p pre_s seconds from memory, then @p post_s more.
 * The clip is a recording with an end time; see record_extend_event.
 */
esp_err_t record_start_event(const char *title, uint32_t pre_s, uint32_t post_s, char *why,
                             size_t why_cap);

/** Push the end of a running event clip out to @p end_us, within the clip's cap. */
void record_extend_event(int64_t end_us);

bool record_event_running(void);

/** The ring a recording used has been given back (or belongs to the dashcam). */
bool record_ring_idle(void);

/** Start the dashcam's background writer on the card. */
esp_err_t record_start_segments(char *why, size_t why_cap);

/** Stop it; with @p forget, delete what it kept. */
void record_stop_segments(const char *reason, bool forget);

bool record_segments_running(void);

/** Clips queued or being converted, and the last one finished. From dashcam.c. */
uint32_t dashcam_clips_converting(void);
void dashcam_last_clip(char *out, size_t cap);

/** A clip made of segments is done with them. */
void record_unpin_segments(uint32_t first);

/** Where segment @p n lives, as a full path. */
size_t record_segment_path(uint32_t n, char *out, size_t cap);

/** A chapter at @p at_us in the recording that is running. */
void record_add_chapter(int64_t at_us, const char *title);

/** Called on the writer task when a recording has closed. Must return quickly. */
void record_set_finished_cb(void (*cb)(const record_finished_t *done));
