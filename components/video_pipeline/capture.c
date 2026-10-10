/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "capture_priv.h"
#include "screentext_store.h"
#include "esp_task_wdt.h"

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
static uint64_t s_window_ppa_us;
/* Latency, per window: landed -> taken by the loop, published -> sent, and
 * landed -> sent (the whole way through the device). */
static uint64_t s_window_wait_us, s_window_send_us, s_window_total_us;
static uint32_t s_window_waits, s_window_sends, s_window_total_max_us;
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

void capture_status_set_input(uint8_t hz, bool too_fast)
{
    taskENTER_CRITICAL(&s_mu);
    s_status.input_hz = hz;
    s_status.too_fast = too_fast;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_set_signal(bool present, uint8_t sys_status)
{
    taskENTER_CRITICAL(&s_mu);
    s_status.signal = present;
    s_status.sys_status = sys_status;
    if (!present) {
        s_status.input_hz = 0;
        s_status.too_fast = false;
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

void capture_status_add_ppa_time(uint32_t us)
{
    taskENTER_CRITICAL(&s_mu);
    s_window_ppa_us += us;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_add_wait(uint32_t us)
{
    taskENTER_CRITICAL(&s_mu);
    s_window_wait_us += us;
    s_window_waits++;
    taskEXIT_CRITICAL(&s_mu);
}

void capture_status_add_sent(uint32_t send_us, uint32_t total_us)
{
    /* A new viewer is sent the last frame at once, however old it is; that is
     * a replay, not the live path being slow. */
    if (send_us > 1000000u) {
        return;
    }
    taskENTER_CRITICAL(&s_mu);
    s_window_send_us += send_us;
    s_window_total_us += total_us;
    s_window_sends++;
    if (total_us > s_window_total_max_us) {
        s_window_total_max_us = total_us;
    }
    taskEXIT_CRITICAL(&s_mu);
}

/*
 * How much of the configured quality the slowest viewer's link can take, in
 * percent. A viewer that cannot take a frame (its socket is still full of the
 * last one) is skipped, and for H.264 asked to wait for a keyframe - which is
 * many times bigger than a frame and fills the link again. On a slow WiFi link
 * that loop is the picture running smoothly for a second and then stuttering.
 * So two misses in a second cut the quality by 30% (down to a quarter), and
 * every three clean seconds give back a quarter of it.
 */
#define LINK_MIN_PCT 25
#define LINK_CLEAN_US (3 * 1000000LL)
static uint32_t s_link_pct = 100;
/* s_status.link_pct is filled at the first window; until then it reads 0,
 * which the console takes as "not known yet". */
static uint32_t s_window_misses;
static int64_t s_link_last_miss_us;
static int64_t s_link_last_step_us;

void capture_link_miss(void)
{
    taskENTER_CRITICAL(&s_mu);
    s_window_misses++;
    s_link_last_miss_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mu);
}

uint32_t capture_link_pct(void)
{
    return s_link_pct;
}

/* Called once a second with the window lock held. */
static void link_step_locked(int64_t now)
{
    if (s_window_misses >= 2) {
        const uint32_t next = s_link_pct * 7u / 10u;
        s_link_pct = next < LINK_MIN_PCT ? LINK_MIN_PCT : next;
        s_link_last_step_us = now;
    } else if (s_link_pct < 100 && now - s_link_last_miss_us > LINK_CLEAN_US &&
               now - s_link_last_step_us > LINK_CLEAN_US) {
        const uint32_t next = s_link_pct * 125u / 100u + 1u;
        s_link_pct = next > 100u ? 100u : next;
        s_link_last_step_us = now;
    }
    s_window_misses = 0;
    s_status.link_pct = s_link_pct;
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
    const uint64_t ppa_us = s_window_ppa_us;
    const uint32_t encodes = s_window_encodes;
    s_window_frames = 0;
    s_window_skipped = 0;
    s_window_bytes = 0;
    s_window_encode_us = 0;
    s_window_ppa_us = 0;
    s_window_encodes = 0;
    s_window_start_us = now;
    link_step_locked(now);
    s_status.lag_wait_ms = s_window_waits ? (uint32_t)(s_window_wait_us / s_window_waits / 1000) : 0;
    s_status.lag_send_ms = s_window_sends ? (uint32_t)(s_window_send_us / s_window_sends / 1000) : 0;
    s_status.lag_total_ms = s_window_sends ? (uint32_t)(s_window_total_us / s_window_sends / 1000) : 0;
    s_status.lag_total_max_ms = s_window_total_max_us / 1000;
    s_window_wait_us = s_window_send_us = s_window_total_us = 0;
    s_window_waits = s_window_sends = s_window_total_max_us = 0;

    s_status.fps_x100 = (uint32_t)((uint64_t)frames * 100000000ull / (uint64_t)elapsed_us);
    s_status.kbps = (uint32_t)(bytes * 8000ull / (uint64_t)elapsed_us);
    s_status.skipped_fps_x100 = (uint32_t)((uint64_t)skipped * 100000000ull / (uint64_t)elapsed_us);
    s_status.encode_us = encodes ? (uint32_t)(encode_us / encodes) : 0;
    s_status.ppa_us = encodes ? (uint32_t)(ppa_us / encodes) : 0;
    /* Share of wall clock spent turning frames into a stream - colour conversion
     * plus encode - the honest measure of how close the pipeline is to
     * saturated. Clamped because work that starts in one window and ends in the
     * next is counted whole, which can otherwise exceed all the available time. */
    const uint64_t busy = (encode_us + ppa_us) * 100ull / (uint64_t)elapsed_us;
    s_status.encoder_busy_pct = busy > 100u ? 100u : (uint32_t)busy;
    taskEXIT_CRITICAL(&s_mu);
}

static capture_memory_pressure_cb_t s_pressure_cb;

void capture_set_memory_pressure_cb(capture_memory_pressure_cb_t cb)
{
    s_pressure_cb = cb;
}

bool capture_release_memory(bool failed)
{
    if (!s_pressure_cb) {
        return false;
    }
    s_pressure_cb(failed);
    return true;
}

void capture_status_get(kvm_video_status_t *out)
{
    if (!out) {
        return;
    }
    taskENTER_CRITICAL(&s_mu);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mu);
    /* Kept by the capture task rather than in the status block: it is one
       integer written by one task and read here, and it has no business
       holding the critical section the encoder's counters live in. */
    out->flat_ms = capture_flat_ms();
    out->flat_dark = out->flat_ms && capture_flat_dark();
    int64_t last_us = 0;
    capture_frame_counter(&out->frames, &last_us);
    const int64_t age = last_us ? (esp_timer_get_time() - last_us) / 1000 : -1;
    out->h264_cpu = CAPTURE_YUV_SWAP;
    out->frame_age_ms = age < 0 ? UINT32_MAX : age > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)age;
}

static bool s_probed;

static void reserve_task(void *arg)
{
    /* Before the capture pipeline claims memory and the encoder engines. */
    capture_h264_probe();
    capture_mjpeg_probe();
    /* First, while PSRAM is one piece: every codec buffer lives in it. */
    (void)capture_arena_init();
#if CAPTURE_YUV_SWAP
    /*
     * The byte-reordering buffer: 4 MB of PSRAM that MJPEG cannot encode a
     * frame without. Taken first so it sits next to MJPEG's own buffers; H.264
     * hands both back together when it needs the room (capture_h264.c), and
     * MJPEG takes this one again before its outputs when it opens.
     */
    if (capture_yuv_swap_reserve(capture_yuv_swap_max_bytes()) != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "reordering buffer not reserved; MJPEG will try again later");
    }
#endif
    /* The encoder's reference frame wants one internal block of ~135 KB. At
     * this point the block is there; a few seconds of network and TLS later it
     * is often not. Sized for the largest mode, which is what sources send. */
    capture_h264_reserve(0, 0);
    xSemaphoreGive((SemaphoreHandle_t)arg);
    vTaskDelete(NULL);
}

void capture_reserve_early(void)
{
    if (s_probed) {
        return;
    }
    s_probed = true;
    /* Its own task: app_main's stack is 3.5 KB, too small for the encoders. */
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done && xTaskCreatePinnedToCore(reserve_task, "codecs", 8192, done, 5, NULL, 0) == pdPASS) {
        (void)xSemaphoreTake(done, portMAX_DELAY);
    } else {
        ESP_LOGE(CAPTURE_LOG_TAG, "codec probe task could not start");
    }
    if (done) {
        vSemaphoreDelete(done);
    }
}

static void camera_task(void *arg)
{
    (void)arg;
    capture_reserve_early();

    capture_ctx_t *ctx = capture_hw_init_start();
    if (ctx) {
        capture_monitor_start(ctx);
        /* The loop feeds the dog once per pass; every wait in it is bounded. */
        (void)esp_task_wdt_add(NULL);
        capture_loop_run(ctx);
        (void)esp_task_wdt_delete(NULL);
    }
    /* Falling off the end of a FreeRTOS task function aborts; the capture path
     * now gives up gracefully when there is no capture card. */
    vTaskDelete(NULL);
}

void capture_start(void)
{
    /* Before the task starts: the store is written from the capture task and
     * cleared from the monitor task, so it cannot be built lazily by whichever
     * gets there first. */
    screentext_store_init();
    capture_screentext_init();
    capture_mjpeg_bind_settings();
    /* Peak 1.4 KB measured; the rest was internal RAM for nothing. */
    const uint32_t cam_stack = 6144;
    xTaskCreatePinnedToCore(camera_task, "cam", cam_stack, NULL, 5, NULL, 0);
}

/*
 * Every software restart parks the capture DMA first.
 *
 * Linked with --wrap=esp_restart, so this covers the OTA reboot, the console's
 * restart button and every other deliberate restart in the firmware, without
 * each of them having to remember. The panic path does not come through here -
 * it calls esp_restart_noos() directly - which is why the stop lives in a
 * normal context and can use the driver rather than poking registers.
 *
 * The encoder and the PPA also write PSRAM by DMA, and a 1080p H.264 frame
 * takes 43 ms or more, so the loop is told to start no new frame and the one
 * in hand is waited for, here and in the pre-3.0 boards' separate encoder
 * task.
 */
static atomic_bool s_parking;
static atomic_int s_frames_busy;

bool capture_park_frame_begin(void)
{
    atomic_fetch_add(&s_frames_busy, 1);
    if (atomic_load(&s_parking)) {
        atomic_fetch_sub(&s_frames_busy, 1);
        return false;
    }
    return true;
}

void capture_park_frame_end(void)
{
    atomic_fetch_sub(&s_frames_busy, 1);
}

void __real_esp_restart(void) __attribute__((noreturn));

void __wrap_esp_restart(void)
{
    atomic_store(&s_parking, true);
    capture_hw_quiesce();
    for (int i = 0; i < 30 && atomic_load(&s_frames_busy) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    __real_esp_restart();
}
