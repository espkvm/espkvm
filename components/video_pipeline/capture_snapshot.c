/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
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

/* One VNC viewer keeps these allocations until it disconnects. The capture
 * task is the only user of the encoder; its output stays untouched while the
 * network task sends it because another encode is requested only afterwards. */
static SemaphoreHandle_t s_stream_mu;
static SemaphoreHandle_t s_stream_done;
static jpeg_encoder_handle_t s_stream_encoder;
static uint8_t *s_stream_buf;
static size_t s_stream_cap, s_stream_len;
static bool s_stream_active, s_stream_wanted, s_stream_ready;
static esp_err_t s_stream_err;

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

static esp_err_t stream_buffer_open(capture_ctx_t *c)
{
    const size_t worst = (size_t)c->hres * (size_t)c->vres + 512u * 1024u;
    const size_t tries[] = {worst, 1024u * 1024u, 640u * 1024u, 384u * 1024u};
    jpeg_encode_memory_alloc_cfg_t mcfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]) && !s_stream_buf; i++) {
        if ((i == 0 || tries[i] < worst) &&
            (tries[i] <= heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) ||
             i == sizeof(tries) / sizeof(tries[0]) - 1)) {
            s_stream_buf = jpeg_alloc_encoder_mem(tries[i], &mcfg, &s_stream_cap);
        }
    }
    if (!s_stream_buf) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* The VNC client's JPEG quality, 1-100. */
static volatile uint8_t s_stream_quality = 80;

void capture_vnc_snapshot_quality(int quality)
{
    s_stream_quality = (uint8_t)(quality < 1 ? 1 : quality > 100 ? 100 : quality);
}

static esp_err_t stream_encode(capture_ctx_t *c, const void *src)
{
    esp_err_t err = s_stream_encoder ? ESP_OK : stream_encoder_open(c);
    if (err != ESP_OK) {
        return err;
    }
    const size_t worst = (size_t)c->hres * (size_t)c->vres + 512u * 1024u;
    if (!s_stream_buf || s_stream_cap < worst) {
        free(s_stream_buf);
        s_stream_buf = NULL;
        s_stream_cap = 0;
        err = stream_buffer_open(c);
        if (err != ESP_OK) {
            return err;
        }
    }
    const jpeg_encode_cfg_t cfg = {
        .width = c->hres,
        .height = c->vres,
        .src_type = (jpeg_enc_input_format_t)capture_pixfmt()->jpeg_src,
        .sub_sample = (jpeg_down_sampling_type_t)capture_pixfmt()->jpeg_subsample,
        .image_quality = s_stream_quality,
    };
    uint32_t len = 0;
    err = jpeg_encoder_process(s_stream_encoder, &cfg, src, (uint32_t)c->frame_bytes,
                               s_stream_buf, (uint32_t)s_stream_cap, &len);
    if (err != ESP_OK && s_stream_cap < worst) {
        jpeg_encode_cfg_t low = cfg;
        low.image_quality = SNAPSHOT_QUALITY_LOW;
        err = jpeg_encoder_process(s_stream_encoder, &low, src, (uint32_t)c->frame_bytes,
                                   s_stream_buf, (uint32_t)s_stream_cap, &len);
    }
    if (err == ESP_OK) {
        s_stream_len = len;
    }
    return err;
}

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

esp_err_t capture_vnc_snapshot_begin(void)
{
    stream_sync_init();
    if (!s_stream_mu || !s_stream_done) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_stream_mu, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_mu);
    const bool already_active = s_stream_active;
    if (!already_active) {
        s_stream_active = true;
        s_stream_wanted = false;
        s_stream_ready = false;
    }
    portEXIT_CRITICAL(&s_mu);
    xSemaphoreGive(s_stream_mu);
    return already_active ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t capture_vnc_snapshot_next(const uint8_t **jpeg, size_t *jpeg_len, uint32_t timeout_ms)
{
    if (!jpeg || !jpeg_len || !s_stream_mu || !s_stream_done) {
        return ESP_ERR_INVALID_ARG;
    }
    *jpeg = NULL;
    *jpeg_len = 0;
    (void)xSemaphoreTake(s_stream_done, 0);
    portENTER_CRITICAL(&s_mu);
    if (!s_stream_active) {
        portEXIT_CRITICAL(&s_mu);
        return ESP_ERR_INVALID_STATE;
    }
    s_stream_ready = false;
    s_stream_wanted = true;
    portEXIT_CRITICAL(&s_mu);
    if (xSemaphoreTake(s_stream_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        portENTER_CRITICAL(&s_mu);
        s_stream_wanted = false;
        portEXIT_CRITICAL(&s_mu);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_mu);
    const esp_err_t err = s_stream_ready ? s_stream_err : ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        *jpeg = s_stream_buf;
        *jpeg_len = s_stream_len;
    }
    s_stream_ready = false;
    portEXIT_CRITICAL(&s_mu);
    return err;
}

void capture_vnc_snapshot_end(void)
{
    if (!s_stream_mu || xSemaphoreTake(s_stream_mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    s_stream_active = false;
    s_stream_wanted = false;
    s_stream_ready = false;
    portEXIT_CRITICAL(&s_mu);
    if (s_stream_encoder) {
        jpeg_del_encoder_engine(s_stream_encoder);
        s_stream_encoder = NULL;
    }
    free(s_stream_buf);
    s_stream_buf = NULL;
    s_stream_cap = s_stream_len = 0;
    xSemaphoreGive(s_stream_mu);
}

void capture_snapshot_tick(capture_ctx_t *c, const void *frame)
{
    portENTER_CRITICAL(&s_mu);
    const bool stream_wanted = s_stream_active && s_stream_wanted;
    portEXIT_CRITICAL(&s_mu);
    if (stream_wanted && s_stream_mu && xSemaphoreTake(s_stream_mu, portMAX_DELAY) == pdTRUE) {
        portENTER_CRITICAL(&s_mu);
        const bool still_wanted = s_stream_active && s_stream_wanted;
        portEXIT_CRITICAL(&s_mu);
        if (still_wanted) {
            const esp_err_t stream_err = stream_encode(c, frame);
            portENTER_CRITICAL(&s_mu);
            const bool notify = s_stream_active && s_stream_wanted;
            if (notify) {
                s_stream_wanted = false;
                s_stream_ready = true;
                s_stream_err = stream_err;
            }
            portEXIT_CRITICAL(&s_mu);
            if (notify) {
                xSemaphoreGive(s_stream_done);
            }
            if (stream_err != ESP_OK) {
                ESP_LOGW(CAPTURE_LOG_TAG, "VNC JPEG: %s", esp_err_to_name(stream_err));
            }
        }
        xSemaphoreGive(s_stream_mu);
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
