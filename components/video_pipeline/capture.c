/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "capture_priv.h"

/*
 * Telemetry lives here rather than in the capture task so the HTTP layer can
 * read it without touching CSI state. Writers are the capture and monitor
 * tasks; readers are HTTP handlers. A spinlock keeps a reader from seeing a
 * half-updated mode.
 */
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;
static kvm_video_status_t s_status;

/** Accumulators for the rolling one-second window. */
static uint32_t s_window_frames;
static uint32_t s_window_skipped;
static uint64_t s_window_encode_us;
static uint32_t s_window_encodes;
static uint64_t s_window_bytes;
static int64_t s_window_start_us;

void capture_status_set_mode(uint32_t hres, uint32_t vres, bool interlaced)
{
    taskENTER_CRITICAL(&s_mu);
    if (s_status.hres != hres || s_status.vres != vres) {
        s_status.mode_changes++;
    }
    s_status.hres = hres;
    s_status.vres = vres;
    s_status.interlaced = interlaced;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_set_signal(bool present, uint8_t sys_status)
{
    taskENTER_CRITICAL(&s_mu);
    s_status.signal = present;
    s_status.sys_status = sys_status;
    if (!present) {
        s_status.fps_x100 = 0;
        s_status.kbps = 0;
        s_status.skipped_fps_x100 = 0;
    }
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_add_frame(size_t bytes)
{
    taskENTER_CRITICAL(&s_mu);
    s_window_frames++;
    s_window_bytes += bytes;
    taskEXIT_CRITICAL(&s_mu);
}

/*
 * How long the encoder actually took. This is the number that says whether
 * there is headroom left: at 1080p the JPEG engine needs roughly 50 ms, so a
 * source sending 30 frames a second is already asking for more than the device
 * can give, and no amount of network will change that.
 */
void capture_status_add_encode_time(uint32_t us)
{
    taskENTER_CRITICAL(&s_mu);
    s_window_encode_us += us;
    s_window_encodes++;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_add_skipped(void)
{
    taskENTER_CRITICAL(&s_mu);
    s_window_skipped++;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_tick(void)
{
    const int64_t now = esp_timer_get_time();

    taskENTER_CRITICAL(&s_mu);
    if (s_window_start_us == 0) {
        s_window_start_us = now;
        taskEXIT_CRITICAL(&s_mu);
        return;
    }
    const int64_t elapsed_us = now - s_window_start_us;
    if (elapsed_us < 1000000) {
        taskEXIT_CRITICAL(&s_mu);
        return;
    }
    const uint32_t frames = s_window_frames;
    const uint32_t skipped = s_window_skipped;
    const uint64_t bytes = s_window_bytes;
    const uint64_t encode_us = s_window_encode_us;
    const uint32_t encodes = s_window_encodes;
    s_window_frames = 0;
    s_window_skipped = 0;
    s_window_bytes = 0;
    s_window_encode_us = 0;
    s_window_encodes = 0;
    s_window_start_us = now;

    s_status.fps_x100 = (uint32_t)((uint64_t)frames * 100000000ull / (uint64_t)elapsed_us);
    s_status.kbps = (uint32_t)(bytes * 8000ull / (uint64_t)elapsed_us);
    s_status.skipped_fps_x100 = (uint32_t)((uint64_t)skipped * 100000000ull / (uint64_t)elapsed_us);
    s_status.encode_us = encodes ? (uint32_t)(encode_us / encodes) : 0;
    /* Share of wall clock the encoder was busy: the honest measure of how close
     * the pipeline is to saturated. Clamped because an encode that starts in
     * one window and ends in the next is counted whole, which can otherwise
     * report more than all of the available time. */
    const uint64_t busy = encode_us * 100ull / (uint64_t)elapsed_us;
    s_status.encoder_busy_pct = busy > 100u ? 100u : (uint32_t)busy;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_get(kvm_video_status_t *out)
{
    if (!out) {
        return;
    }
    taskENTER_CRITICAL(&s_mu);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mu);
}

static void camera_task(void *arg)
{
    (void)arg;
    /* Before the capture pipeline claims memory and the encoder engines. */
    capture_h264_probe();

    capture_ctx_t *ctx = capture_hw_init_start();
    if (ctx) {
        capture_monitor_start(ctx);
        capture_loop_run(ctx);
    }
    /* Falling off the end of a FreeRTOS task function aborts; the capture path
     * now gives up gracefully when there is no capture card. */
    vTaskDelete(NULL);
}

void capture_start(void)
{
    capture_mjpeg_bind_settings();
    const uint32_t cam_stack = 10240;
    xTaskCreatePinnedToCore(camera_task, "cam", cam_stack, NULL, 5, NULL, 0);
}
