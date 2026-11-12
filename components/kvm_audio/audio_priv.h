/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kvm_audio.h"

/** Called once from kvm_audio_init. */
esp_err_t opus_init(void);

/* The encoder takes 48 kHz, whatever the source plays. */
#define OUT_RATE 48000u
/* A 5 ms block is rate / 200 frames at any rate, so 240 at 48 kHz; one more
   for the rounding. */
#define RESAMPLE_MAX (OUT_RATE / 200 + 1)

/*
 * Linear interpolation to 48 kHz. Good enough to hear a target by; 88.2 and
 * 96 kHz alias a little, and HDMI sources rarely send those.
 */
typedef struct {
    uint32_t rate;
    uint32_t pos;     /* 16.16, into the current input block */
    int16_t prev[2];  /* the last input frame, for the step across blocks */
    bool have_prev;
} resampler_t;

/** Start over, at @p rate: after a gap or a new rate. */
void resample_reset(resampler_t *rs, uint32_t rate);

/**
 * One block at 48 kHz into @p out (room for RESAMPLE_MAX frames). Returns the
 * frames written; @p first_us is the time of the first one.
 */
size_t resample_block(resampler_t *rs, const kvm_audio_block_t *b, int16_t *out, int64_t *first_us);

/*
 * The USB speaker source (usb_source.c). The task is woken once per
 * @p block_frames of sound; a take hands out one whole block or nothing.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { USB_TAKE_NONE, USB_TAKE_OK, USB_TAKE_GAP } usb_take_t;

void usb_src_init(TaskHandle_t task, uint16_t block_frames);
/** The target has picked the speaker and is playing to it. */
bool usb_src_streaming(void);
/** Throw away what waits, so nobody hears it late. */
void usb_src_drain(void);
/** GAP: sound was lost, so the next block starts a new epoch. */
usb_take_t usb_src_take(int16_t *pcm, uint16_t frames, int64_t *start_us);
uint32_t usb_src_rate(void);
