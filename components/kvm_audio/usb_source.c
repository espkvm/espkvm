/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sound from the target's USB speaker (aud_usb). kvm_hid puts the speaker in
 * the descriptor; the TinyUSB audio class fills its FIFO with 48 kHz 16-bit
 * stereo; this file wakes the audio task once per 5 ms of sound and hands out
 * whole blocks. The clock request and the alt-setting callbacks follow JetKVM's
 * jetkvm_audio.c (MIT).
 */
#include "audio_priv.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#if CFG_TUD_AUDIO

#define USB_RATE 48000u
#define FRAME_BYTES (KVM_AUDIO_CHANNELS * sizeof(int16_t))
#define CLOCK_ID 4 /* the clock source entity in kvm_hid's descriptor */

static TaskHandle_t s_task;
static volatile bool s_streaming;
static uint32_t s_isr_bytes; /* ISR only */
static uint32_t s_wake_bytes;

void usb_src_init(TaskHandle_t task, uint16_t block_frames)
{
    s_task = task;
    s_wake_bytes = block_frames * FRAME_BYTES;
}

bool usb_src_streaming(void)
{
    return s_streaming && tud_audio_mounted();
}

void usb_src_drain(void)
{
    tu_fifo_t *ff = tud_audio_get_ep_out_ff();
    if (ff) {
        tu_fifo_clear(ff);
    }
}

usb_take_t usb_src_take(int16_t *pcm, uint16_t frames, int64_t *start_us)
{
    tu_fifo_t *ff = tud_audio_get_ep_out_ff();
    if (!ff) {
        return USB_TAKE_NONE;
    }
    /* Overwritable FIFO: an index gap past its depth means it lapped us. */
    if (tu_ff_overflow_count(ff->depth, ff->wr_idx, ff->rd_idx) > ff->depth) {
        /* The task fell behind: what is left no longer runs on in time. */
        tu_fifo_clear(ff);
        return USB_TAKE_GAP;
    }
    const uint32_t want = (uint32_t)frames * FRAME_BYTES;
    const uint16_t avail = tud_audio_available();
    if (avail < want) {
        return USB_TAKE_NONE;
    }
    const int64_t now = esp_timer_get_time();
    if (tud_audio_read(pcm, (uint16_t)want) != want) {
        tu_fifo_clear(ff);
        return USB_TAKE_GAP;
    }
    /* The newest byte came about now; the block starts avail bytes earlier. */
    *start_us = now - (int64_t)(avail / FRAME_BYTES) * 1000000 / USB_RATE;
    return USB_TAKE_OK;
}

uint32_t usb_src_rate(void)
{
    return USB_RATE;
}

/* ISR context: count the sound in and wake the task once per block. */
bool IRAM_ATTR tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                                     uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;
    s_isr_bytes += n_bytes_received;
    if (s_task && s_wake_bytes && s_isr_bytes >= s_wake_bytes) {
        s_isr_bytes = 0;
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
    return true;
}

/* One fixed clock: 48 kHz, always valid. No volume or mute controls; the
 * target keeps its volume in software. */
bool tud_audio_get_req_entity_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    const audio_control_request_t *req = (const audio_control_request_t *)request;
    if (req->bEntityID != CLOCK_ID || req->bChannelNumber != 0) {
        return false;
    }
    if (req->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t rate = {.bCur = USB_RATE};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &rate, sizeof(rate));
        }
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) range = {.wNumSubRanges = 1,
                                                  .subrange = {{USB_RATE, USB_RATE, 0}}};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
        }
    }
    if (req->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof(valid));
    }
    return false;
}

/* Alt 1 = the target plays to us, alt 0 = it stopped. */
bool tud_audio_set_itf_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport;
    s_streaming = tu_le16toh(request->wValue) == 1;
    usb_src_drain();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport;
    (void)request;
    s_streaming = false;
    return true;
}

#else /* no audio class in this build */

void usb_src_init(TaskHandle_t task, uint16_t block_frames)
{
    (void)task;
    (void)block_frames;
}

bool usb_src_streaming(void)
{
    return false;
}

void usb_src_drain(void)
{
}

usb_take_t usb_src_take(int16_t *pcm, uint16_t frames, int64_t *start_us)
{
    (void)pcm;
    (void)frames;
    (void)start_us;
    return USB_TAKE_NONE;
}

uint32_t usb_src_rate(void)
{
    return 0;
}

#endif
