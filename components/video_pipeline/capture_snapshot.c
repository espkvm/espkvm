/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One still picture of the screen, as a JPEG, whatever the stream is.
 *
 * On MJPEG the published frame already is one. On H.264 there is no picture to
 * hand out, so the capture task encodes the frame it is holding once more
 * through the JPEG engine - which H.264 leaves idle - and gives it back. The
 * engine and its output buffer exist only for that one frame: a screenshot is
 * rare, and 2.5 MB of PSRAM held for it the rest of the time is not.
 */
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "capture_pixfmt.h"
#include "capture_priv.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "video_frame.h"

#define SNAPSHOT_QUALITY 90
#define SNAPSHOT_QUALITY_LOW 60

static SemaphoreHandle_t s_one;  /* one request at a time */
static SemaphoreHandle_t s_done; /* the capture task answered */
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;
static bool s_wanted;  /* a caller is waiting for the next frame */
static uint8_t *s_jpeg; /* the answer, owned by the caller once taken */
static size_t s_jpeg_len;
static esp_err_t s_err;

/*
 * The VNC viewer's frames. Two output buffers: the client sends one while the
 * next is encoded into the other, so a frame is mostly ready by the time the
 * client asks for it. On boards that encode H.264 straight from the capture
 * buffer the JPEG is made by its own task, at the same time as the H.264 of
 * the same held frame: the two engines are separate, and the capture loop
 * waits for the slower one instead of both in turn. The buffers and the
 * encoder are kept while a VNC client is connected.
 */
#define STREAM_BUF_MAX (1024u * 1024u)
static SemaphoreHandle_t s_stream_mu;   /* the encoder and buffers' lifetime */
static SemaphoreHandle_t s_stream_done; /* a frame got ready */
static jpeg_encoder_handle_t s_stream_encoder;
static uint8_t *s_stream_buf[2];
static size_t s_stream_cap[2], s_stream_len[2];
static bool s_stream_active, s_stream_wanted;
static int s_stream_front = -1; /* the buffer the client has; not written */
static int s_stream_ready = -1; /* a finished frame not yet handed out */
static esp_err_t s_stream_err;
#if CAPTURE_DIRECT_ENCODE
static TaskHandle_t s_vnc_task;
static SemaphoreHandle_t s_vnc_kick, s_vnc_idle;
static capture_ctx_t *s_vnc_c;
static const void *s_vnc_src;
static int s_vnc_idx;
static bool s_vnc_running;
#endif

static esp_err_t encode(capture_ctx_t *c, const void *src, uint8_t **out, size_t *out_len)
{
    jpeg_encode_engine_cfg_t ecfg = {.intr_priority = 0, .timeout_ms = 500};
    jpeg_encoder_handle_t enc = NULL;
    esp_err_t err = jpeg_new_encoder_engine(&ecfg, &enc);
    if (err != ESP_OK) {
        return err;
    }
    /* The worst case first. A pre-3.0 board running H.264 has under 3 MB of
     * PSRAM left, so smaller buffers are tried after it: a 1080p screen at this
     * quality is rarely over half a megabyte. */
    const size_t worst = (size_t)c->hres * (size_t)c->vres + 512u * 1024u;
    const size_t tries[] = {worst, 1024u * 1024u, 640u * 1024u, 384u * 1024u};
    jpeg_encode_memory_alloc_cfg_t mcfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t cap = 0;
    uint8_t *buf = NULL;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]) && !buf; i++) {
        /* Skipped when it cannot fit, so the driver does not log a failure. */
        if ((i == 0 || tries[i] < worst) &&
            (tries[i] <= heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) ||
             i == sizeof(tries) / sizeof(tries[0]) - 1)) {
            buf = jpeg_alloc_encoder_mem(tries[i], &mcfg, &cap);
        }
    }
    if (!buf) {
        jpeg_del_encoder_engine(enc);
        return ESP_ERR_NO_MEM;
    }
    const jpeg_encode_cfg_t cfg = {
        .width = c->hres,
        .height = c->vres,
        .src_type = (jpeg_enc_input_format_t)capture_pixfmt()->jpeg_src,
        .sub_sample = (jpeg_down_sampling_type_t)capture_pixfmt()->jpeg_subsample,
        .image_quality = SNAPSHOT_QUALITY,
    };
    uint32_t len = 0;
    err = jpeg_encoder_process(enc, &cfg, src, (uint32_t)c->frame_bytes, buf, (uint32_t)cap, &len);
    if (err != ESP_OK && cap < worst) {
        /* A busy picture overflowed the small buffer: fewer bytes, lower quality. */
        jpeg_encode_cfg_t low = cfg;
        low.image_quality = SNAPSHOT_QUALITY_LOW;
        err = jpeg_encoder_process(enc, &low, src, (uint32_t)c->frame_bytes, buf, (uint32_t)cap, &len);
    }
    if (err == ESP_OK) {
        /* Copied out at its real size: the engine's buffer is sized for the worst
         * case. If there is no room for the copy, the buffer itself goes out. */
        *out = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (*out) {
            memcpy(*out, buf, len);
        } else {
            *out = buf;
            buf = NULL;
        }
        *out_len = len;
    }
    free(buf);
    jpeg_del_encoder_engine(enc);
    return err;
}

static void stream_sync_init(void)
{
    portENTER_CRITICAL(&s_mu);
    if (!s_stream_mu) {
        s_stream_mu = xSemaphoreCreateMutex();
        s_stream_done = xSemaphoreCreateBinary();
    }
    portEXIT_CRITICAL(&s_mu);
}

static esp_err_t stream_encoder_open(capture_ctx_t *c)
{
    (void)c;
    jpeg_encode_engine_cfg_t ecfg = {.intr_priority = 0, .timeout_ms = 500};
    return jpeg_new_encoder_engine(&ecfg, &s_stream_encoder);
}

static esp_err_t stream_buffers_open(capture_ctx_t *c)
{
    const size_t worst = (size_t)c->hres * (size_t)c->vres + 512u * 1024u;
    const size_t first = worst < STREAM_BUF_MAX ? worst : STREAM_BUF_MAX;
    const size_t tries[] = {first, 640u * 1024u, 384u * 1024u};
    jpeg_encode_memory_alloc_cfg_t mcfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]) && !s_stream_buf[k]; i++) {
            if (tries[i] <= first) {
                s_stream_buf[k] = jpeg_alloc_encoder_mem(tries[i], &mcfg, &s_stream_cap[k]);
            }
        }
        if (!s_stream_buf[k]) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void stream_buffers_free(void)
{
    for (int k = 0; k < 2; k++) {
        free(s_stream_buf[k]);
        s_stream_buf[k] = NULL;
        s_stream_cap[k] = s_stream_len[k] = 0;
    }
}

/* The VNC client's JPEG quality, 1-100. */
static volatile uint8_t s_stream_quality = 80;

void capture_vnc_snapshot_quality(int quality)
{
    s_stream_quality = (uint8_t)(quality < 1 ? 1 : quality > 100 ? 100 : quality);
}

/* Encode src into buffer idx. Holds s_stream_mu. */
static esp_err_t stream_encode(capture_ctx_t *c, const void *src, int idx)
{
    esp_err_t err = s_stream_encoder ? ESP_OK : stream_encoder_open(c);
    if (err == ESP_OK && !s_stream_buf[idx]) {
        err = stream_buffers_open(c);
    }
    if (err != ESP_OK) {
        return err;
    }
    jpeg_encode_cfg_t cfg = {
        .width = c->hres,
        .height = c->vres,
        .src_type = (jpeg_enc_input_format_t)capture_pixfmt()->jpeg_src,
        .sub_sample = (jpeg_down_sampling_type_t)capture_pixfmt()->jpeg_subsample,
        .image_quality = s_stream_quality,
    };
    uint32_t len = 0;
    err = jpeg_encoder_process(s_stream_encoder, &cfg, src, (uint32_t)c->frame_bytes, s_stream_buf[idx],
                               (uint32_t)s_stream_cap[idx], &len);
    if (err != ESP_OK && cfg.image_quality > SNAPSHOT_QUALITY_LOW) {
        /* A busy picture overflowed the buffer: fewer bytes, lower quality. */
        cfg.image_quality = SNAPSHOT_QUALITY_LOW;
        err = jpeg_encoder_process(s_stream_encoder, &cfg, src, (uint32_t)c->frame_bytes, s_stream_buf[idx],
                                   (uint32_t)s_stream_cap[idx], &len);
    }
    s_stream_len[idx] = err == ESP_OK ? len : 0;
    return err;
}

/* Takes the frame for the client if one is wanted: the buffer to write, or -1. */
static int stream_claim(void)
{
    int idx = -1;
    portENTER_CRITICAL(&s_mu);
    if (s_stream_active && s_stream_wanted) {
        s_stream_wanted = false;
        idx = s_stream_front == 0 ? 1 : 0;
    }
    portEXIT_CRITICAL(&s_mu);
    return idx;
}

static void stream_finish(int idx, esp_err_t err)
{
    portENTER_CRITICAL(&s_mu);
    const bool active = s_stream_active;
    if (active) {
        s_stream_ready = err == ESP_OK ? idx : -1;
        s_stream_err = err;
    }
    portEXIT_CRITICAL(&s_mu);
    if (active) {
        xSemaphoreGive(s_stream_done);
    }
    if (err != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "VNC JPEG: %s", esp_err_to_name(err));
    }
}

#if CAPTURE_DIRECT_ENCODE
static void vnc_jpeg_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)xSemaphoreTake(s_vnc_kick, portMAX_DELAY);
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (xSemaphoreTake(s_stream_mu, portMAX_DELAY) == pdTRUE) {
            err = stream_encode(s_vnc_c, s_vnc_src, s_vnc_idx);
            xSemaphoreGive(s_stream_mu);
        }
        stream_finish(s_vnc_idx, err);
        xSemaphoreGive(s_vnc_idle);
    }
}

void capture_vnc_snapshot_wait(void)
{
    if (s_vnc_running) {
        (void)xSemaphoreTake(s_vnc_idle, portMAX_DELAY);
        s_vnc_running = false;
        xSemaphoreGive(s_vnc_idle);
    }
}
#else
void capture_vnc_snapshot_wait(void)
{
}
#endif

bool capture_snapshot_wanted(void)
{
    portENTER_CRITICAL(&s_mu);
    const bool wanted = s_wanted;
    portEXIT_CRITICAL(&s_mu);
    return wanted;
}

bool capture_vnc_snapshot_wanted(void)
{
    portENTER_CRITICAL(&s_mu);
    const bool wanted = s_stream_active && s_stream_wanted;
    portEXIT_CRITICAL(&s_mu);
    return wanted;
}

void capture_snapshot_usb_started(void)
{
    /* No raw frame will follow the source switch to satisfy these waits. */
    portENTER_CRITICAL(&s_mu);
    const bool snapshot = s_wanted;
    const bool stream = s_stream_active && s_stream_wanted;
    if (snapshot) {
        s_wanted = false;
        s_err = ESP_ERR_NOT_SUPPORTED;
    }
    if (stream) {
        s_stream_wanted = false;
        s_stream_ready = -1;
        s_stream_err = ESP_ERR_NOT_SUPPORTED;
    }
    portEXIT_CRITICAL(&s_mu);
    if (snapshot) xSemaphoreGive(s_done);
    if (stream) xSemaphoreGive(s_stream_done);
}

esp_err_t capture_vnc_snapshot_begin(void)
{
    if (capture_source_is_usb()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    stream_sync_init();
    if (!s_stream_mu || !s_stream_done) {
        return ESP_ERR_NO_MEM;
    }
#if CAPTURE_DIRECT_ENCODE
    if (!s_vnc_task) {
        s_vnc_kick = xSemaphoreCreateBinary();
        s_vnc_idle = xSemaphoreCreateBinary();
        if (!s_vnc_kick || !s_vnc_idle) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_vnc_idle);
        if (xTaskCreateWithCaps(vnc_jpeg_task, "vnc_jpeg", 3072, NULL, uxTaskPriorityGet(NULL), &s_vnc_task,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            s_vnc_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    portENTER_CRITICAL(&s_mu);
    const bool already_active = s_stream_active;
    if (!already_active) {
        s_stream_active = true;
        s_stream_wanted = false;
        s_stream_front = -1;
        s_stream_ready = -1;
    }
    portEXIT_CRITICAL(&s_mu);
    return already_active ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t capture_vnc_snapshot_next(const uint8_t **jpeg, size_t *jpeg_len, uint32_t timeout_ms)
{
    if (!jpeg || !jpeg_len || !s_stream_mu || !s_stream_done) {
        return ESP_ERR_INVALID_ARG;
    }
    *jpeg = NULL;
    *jpeg_len = 0;
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        portENTER_CRITICAL(&s_mu);
        if (capture_source_is_usb()) {
            portEXIT_CRITICAL(&s_mu);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (!s_stream_active) {
            portEXIT_CRITICAL(&s_mu);
            return ESP_ERR_INVALID_STATE;
        }
        const int ready = s_stream_ready;
        if (ready >= 0) {
            /* Hand it out and start on the next one at once. */
            s_stream_front = ready;
            s_stream_ready = -1;
            s_stream_wanted = true;
            *jpeg = s_stream_buf[ready];
            *jpeg_len = s_stream_len[ready];
        } else {
            s_stream_wanted = true;
        }
        portEXIT_CRITICAL(&s_mu);
        if (ready >= 0) {
            return ESP_OK;
        }
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0 || xSemaphoreTake(s_stream_done, until - now) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        portENTER_CRITICAL(&s_mu);
        const esp_err_t err = s_stream_ready >= 0 ? ESP_OK : s_stream_err;
        portEXIT_CRITICAL(&s_mu);
        if (err != ESP_OK) {
            return err;
        }
    }
}

void capture_vnc_snapshot_end(void)
{
    if (!s_stream_mu || xSemaphoreTake(s_stream_mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    s_stream_active = false;
    s_stream_wanted = false;
    s_stream_front = -1;
    s_stream_ready = -1;
    portEXIT_CRITICAL(&s_mu);
    if (s_stream_encoder) {
        jpeg_del_encoder_engine(s_stream_encoder);
        s_stream_encoder = NULL;
    }
    stream_buffers_free();
    (void)xSemaphoreTake(s_stream_done, 0);
    xSemaphoreGive(s_stream_mu);
}

void capture_snapshot_tick(capture_ctx_t *c, const void *frame)
{
    const int idx = stream_claim();
    if (idx >= 0) {
#if CAPTURE_DIRECT_ENCODE
        /* Encoded by its own task while the caller encodes H.264 from the
         * same frame; capture_vnc_snapshot_wait() before the frame is let go. */
        if (s_vnc_task && xSemaphoreTake(s_vnc_idle, 0) == pdTRUE) {
            s_vnc_c = c;
            s_vnc_src = frame;
            s_vnc_idx = idx;
            s_vnc_running = true;
            xSemaphoreGive(s_vnc_kick);
            return;
        }
#endif
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (xSemaphoreTake(s_stream_mu, portMAX_DELAY) == pdTRUE) {
            err = stream_encode(c, frame, idx);
            xSemaphoreGive(s_stream_mu);
        }
        stream_finish(idx, err);
        return;
    }
    portENTER_CRITICAL(&s_mu);
    const bool wanted = s_wanted;
    portEXIT_CRITICAL(&s_mu);
    if (!wanted) {
        return;
    }
    uint8_t *jpeg = NULL;
    size_t len = 0;
    const esp_err_t err = encode(c, frame, &jpeg, &len);
    if (err != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "snapshot: %s (PSRAM free %u KB, largest block %u KB)", esp_err_to_name(err),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    }
    portENTER_CRITICAL(&s_mu);
    const bool still = s_wanted; /* the caller may have given up meanwhile */
    if (still) {
        s_wanted = false;
        s_jpeg = jpeg;
        s_jpeg_len = len;
        s_err = err;
    }
    portEXIT_CRITICAL(&s_mu);
    if (still) {
        xSemaphoreGive(s_done);
    } else {
        free(jpeg);
    }
}

static esp_err_t copy_published(uint8_t **out, size_t *out_len, uint32_t timeout_ms)
{
    /* The newest published frame is the screen as it is: the encoder only skips
     * a frame identical to it. */
    video_frame_viewer_enter();
    video_frame_ref_t ref;
    bool got = video_frame_acquire(&ref);
    if (!got && video_frame_wait_new(video_frame_seq(), timeout_ms)) {
        got = video_frame_acquire(&ref);
    }
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (got) {
        err = ESP_ERR_INVALID_STATE;
        if (ref.payload == VIDEO_PAYLOAD_JPEG && ref.len) {
            *out = heap_caps_malloc(ref.len, MALLOC_CAP_SPIRAM);
            err = *out ? ESP_OK : ESP_ERR_NO_MEM;
            if (*out) {
                memcpy(*out, ref.data, ref.len);
                *out_len = ref.len;
            }
        }
        video_frame_release(&ref);
    }
    video_frame_viewer_leave();
    return err;
}

esp_err_t capture_snapshot_jpeg(uint8_t **out, size_t *out_len, uint32_t timeout_ms)
{
    if (!out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;
    if (capture_source_is_usb()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    kvm_video_status_t vs;
    capture_status_get(&vs);
    if (!vs.signal) {
        return ESP_ERR_NOT_FOUND;
    }
    if (video_frame_payload() == VIDEO_PAYLOAD_JPEG) {
        return copy_published(out, out_len, timeout_ms);
    }

    if (!s_one) {
        portENTER_CRITICAL(&s_mu);
        if (!s_one) {
            s_one = xSemaphoreCreateMutex();
            s_done = xSemaphoreCreateBinary();
        }
        portEXIT_CRITICAL(&s_mu);
    }
    if (!s_one || !s_done || xSemaphoreTake(s_one, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    (void)xSemaphoreTake(s_done, 0); /* a late answer to an abandoned request */
    portENTER_CRITICAL(&s_mu);
    if (capture_source_is_usb()) {
        portEXIT_CRITICAL(&s_mu);
        xSemaphoreGive(s_one);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_wanted = true;
    portEXIT_CRITICAL(&s_mu);
    /* The capture loop only reaches a frame while somebody is watching. */
    video_frame_viewer_enter();
    const bool answered = xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    video_frame_viewer_leave();

    esp_err_t err = ESP_ERR_TIMEOUT;
    uint8_t *late = NULL;
    portENTER_CRITICAL(&s_mu);
    if (answered) {
        err = s_err;
        *out = s_jpeg;
        *out_len = s_jpeg_len;
    } else {
        s_wanted = false;
        late = s_jpeg; /* an answer that came just after the wait gave up */
    }
    s_jpeg = NULL;
    portEXIT_CRITICAL(&s_mu);
    free(late);
    xSemaphoreGive(s_one);
    return err;
}
