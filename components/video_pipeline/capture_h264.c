/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * H.264: CSI (RGB888) -> PPA (YUV420) -> hardware encoder -> Annex-B.
 *
 * The trade this makes is described in h264_probe.c: it is slower per frame
 * than MJPEG because of the colour conversion, and far cheaper on the wire
 * because a screen that barely changes produces P-frames of a few hundred
 * bytes instead of another 82 KB JPEG. On a KVM, which shows a mostly static
 * screen, the wire wins.
 *
 * Two things the encoder decides for us, both visible here:
 *  - SPS and PPS are emitted in front of every IDR, so a viewer that joins
 *    mid-stream needs no side-channel to configure its decoder - only an IDR,
 *    which is what video_frame_request_keyframe() asks for.
 *  - Frame size is fixed at open, so a resolution change means a new encoder.
 */
#include "capture_priv.h"

#include <inttypes.h>
#include <stdint.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kvm_settings.h"
#include "video_frame.h"

/** Macroblocks are 16x16, so the encoder works on a size rounded up to that.
 *  1080 becomes 1088; the SPS crops the padding away again. */
#define MB_ALIGN(v) (((v) + 15u) & ~15u)

#define H264_MAX_W MB_ALIGN(CAPTURE_MAX_H_RES)
#define H264_MAX_H MB_ALIGN(CAPTURE_MAX_V_RES)

/**
 * Two output slots, not three: unlike MJPEG these are small, and a second slot
 * is already enough for an encode to proceed while a viewer sends the previous
 * frame.
 */
#define H264_SLOTS 2

/** Room for the largest frame worth sending; an IDR at 1080p is far below it. */
#define H264_SLOT_CAP ((size_t)CAPTURE_MAX_H_RES * CAPTURE_MAX_V_RES / 2u)

static ppa_client_handle_t s_ppa;
static esp_h264_enc_handle_t s_enc;
static esp_h264_enc_param_hw_handle_t s_param;
static uint8_t *s_buf[H264_SLOTS];
static uint32_t s_buf_alloc[H264_SLOTS];

/*
 * PPA colour conversion and the encode run in two tasks over two YUV buffers, so
 * one frame is being encoded while the next is being converted. Measured on this
 * board the conversion dominates (~104 ms vs ~45 ms at 1080p), so overlapping
 * lifts the frame rate by roughly the encode time. The capture loop runs the PPA
 * stage (it must consume the CSI frame promptly, before the receiver reuses it);
 * a dedicated task runs the encoder from whichever YUV buffer the PPA just
 * filled. A free-slot queue and a job queue pass the two buffers between them.
 */
#define H264_YUV_BUFS 2
static uint8_t *s_yuv[H264_YUV_BUFS];
static uint32_t s_yuv_alloc[H264_YUV_BUFS];

typedef struct {
    int slot; /* index into s_yuv, or -1 as the shutdown sentinel */
    uint32_t hres;
    uint32_t vres;
} h264_job_t;

static QueueHandle_t s_free_slots; /* YUV buffers the PPA may write */
static QueueHandle_t s_jobs;       /* YUV buffers filled and awaiting encode */
static SemaphoreHandle_t s_enc_done;
static TaskHandle_t s_enc_task;

static void h264_encode_task(void *arg);

/** Size the encoder is currently configured for, 0 when it is not open. */
static uint32_t s_enc_w;
static uint32_t s_enc_h;
/** Last values pushed into the encoder, so a setting change is noticed. */
static uint8_t s_gop;
static uint32_t s_bitrate;

static const char *h264_err_name(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return "ok";
    case ESP_H264_ERR_ARG:
        return "invalid argument";
    case ESP_H264_ERR_MEM:
        return "out of memory";
    case ESP_H264_ERR_UNSUPPORTED:
        return "unsupported on this chip";
    case ESP_H264_ERR_TIMEOUT:
        return "timeout";
    case ESP_H264_ERR_OVERFLOW:
        return "output buffer overflow";
    default:
        return "failed";
    }
}

static void encoder_release(void)
{
    if (s_enc) {
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
    }
    s_param = NULL;
    s_enc_w = 0;
    s_enc_h = 0;
}

/** Keyframe interval: two seconds of the current frame rate, within the byte
 *  the encoder takes. Long enough not to cost bandwidth, short enough that a
 *  decoder recovers on its own if it ever loses sync. */
static uint8_t wanted_gop(void)
{
    int32_t fps = kvm_setting_int("vid_fps_max");
    if (fps < 1) {
        fps = 30;
    }
    int32_t gop = fps * 2;
    if (gop > 250) {
        gop = 250;
    }
    return (uint8_t)gop;
}

static uint32_t wanted_bitrate(void)
{
    int32_t kbps = kvm_setting_int("h264_kbps");
    if (kbps < 100) {
        kbps = 4000;
    }
    return (uint32_t)kbps * 1000u;
}

static esp_err_t encoder_open(uint32_t w, uint32_t h)
{
    encoder_release();

    int32_t fps = kvm_setting_int("vid_fps_max");
    if (fps < 1 || fps > 255) {
        fps = 30;
    }
    s_gop = wanted_gop();
    s_bitrate = wanted_bitrate();

    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = s_gop,
        .fps = (uint8_t)fps,
        .res = {.width = (uint16_t)w, .height = (uint16_t)h},
        .rc = {.bitrate = s_bitrate, .qp_min = 25, .qp_max = 45},
    };
    esp_h264_err_t herr = esp_h264_enc_hw_new(&cfg, &s_enc);
    if (herr != ESP_H264_ERR_OK || !s_enc) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 encoder for %" PRIu32 "x%" PRIu32 ": %s", w, h,
                 h264_err_name(herr));
        s_enc = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    herr = esp_h264_enc_open(s_enc);
    if (herr != ESP_H264_ERR_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 encoder open: %s", h264_err_name(herr));
        encoder_release();
        return ESP_FAIL;
    }
    (void)esp_h264_enc_hw_get_param_hd(s_enc, &s_param);
    s_enc_w = w;
    s_enc_h = h;
    ESP_LOGI(CAPTURE_LOG_TAG, "h264 encoder %" PRIu32 "x%" PRIu32 " @%" PRId32 " fps, %" PRIu32
             " kbit/s, gop %u", w, h, fps, s_bitrate / 1000u, s_gop);
    return ESP_OK;
}

static void h264_free_buffers(void)
{
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        if (s_yuv[i]) {
            esp_h264_free(s_yuv[i]);
            s_yuv[i] = NULL;
        }
    }
    for (int i = 0; i < H264_SLOTS; i++) {
        if (s_buf[i]) {
            esp_h264_free(s_buf[i]);
            s_buf[i] = NULL;
        }
    }
}

static esp_err_t h264_open(void)
{
    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&ppa_cfg, &s_ppa);
    if (err != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "PPA client: %s", esp_err_to_name(err));
        return err;
    }

    size_t align = 64;
    (void)esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align);
    const size_t yuv_bytes = (size_t)H264_MAX_W * H264_MAX_H * 3u / 2u;
    bool yuv_ok = true;
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        s_yuv[i] = esp_h264_aligned_calloc(align, 1, yuv_bytes, &s_yuv_alloc[i], ESP_H264_MEM_SPIRAM);
        if (!s_yuv[i]) {
            yuv_ok = false;
            break;
        }
    }
    for (int i = 0; i < H264_SLOTS && yuv_ok; i++) {
        s_buf[i] = esp_h264_aligned_calloc(align, 1, H264_SLOT_CAP, &s_buf_alloc[i],
                                           ESP_H264_MEM_SPIRAM);
        if (!s_buf[i]) {
            break;
        }
    }
    if (!yuv_ok || !s_buf[H264_SLOTS - 1]) {
        ESP_LOGE(CAPTURE_LOG_TAG, "not enough PSRAM for the H.264 path");
        h264_free_buffers();
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
        return ESP_ERR_NO_MEM;
    }
    /*
     * The allocator zeroed these buffers with the CPU, leaving dirty cache
     * lines. The encoder writes the input buffer back before reading it with
     * DMA, and those stale lines would land on top of what the PPA just put
     * there. Flush them once, here, rather than debug it later.
     */
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        (void)esp_cache_msync(s_yuv[i], s_yuv_alloc[i], ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    /* Two YUV buffers cycle between the PPA stage and the encode task. */
    s_free_slots = xQueueCreate(H264_YUV_BUFS, sizeof(int));
    s_jobs = xQueueCreate(H264_YUV_BUFS, sizeof(h264_job_t));
    s_enc_done = xSemaphoreCreateBinary();
    if (!s_free_slots || !s_jobs || !s_enc_done) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 queues could not be created");
        goto fail;
    }
    for (int i = 0; i < H264_YUV_BUFS; i++) {
        xQueueSend(s_free_slots, &i, 0);
    }
    if (xTaskCreatePinnedToCore(h264_encode_task, "h264enc", 4096, NULL, 5, &s_enc_task, 1) !=
        pdPASS) {
        ESP_LOGE(CAPTURE_LOG_TAG, "h264 encode task could not start");
        s_enc_task = NULL;
        goto fail;
    }

    size_t cap = H264_SLOT_CAP;
    for (int i = 0; i < H264_SLOTS; i++) {
        if (s_buf_alloc[i] < cap) {
            cap = s_buf_alloc[i];
        }
    }
    video_frame_install(VIDEO_PAYLOAD_H264, s_buf, H264_SLOTS, cap);
    return ESP_OK;

fail:
    if (s_jobs) {
        vQueueDelete(s_jobs);
        s_jobs = NULL;
    }
    if (s_free_slots) {
        vQueueDelete(s_free_slots);
        s_free_slots = NULL;
    }
    if (s_enc_done) {
        vSemaphoreDelete(s_enc_done);
        s_enc_done = NULL;
    }
    h264_free_buffers();
    ppa_unregister_client(s_ppa);
    s_ppa = NULL;
    return ESP_ERR_NO_MEM;
}

static void h264_close(void)
{
    /* Stop the encode task: a negative-slot job is the sentinel it breaks on.
     * Wait for it to finish whatever it was encoding before freeing anything. */
    if (s_enc_task) {
        const h264_job_t stop = {.slot = -1};
        xQueueSend(s_jobs, &stop, portMAX_DELAY);
        xSemaphoreTake(s_enc_done, portMAX_DELAY);
        s_enc_task = NULL;
    }
    if (s_jobs) {
        vQueueDelete(s_jobs);
        s_jobs = NULL;
    }
    if (s_free_slots) {
        vQueueDelete(s_free_slots);
        s_free_slots = NULL;
    }
    if (s_enc_done) {
        vSemaphoreDelete(s_enc_done);
        s_enc_done = NULL;
    }
    encoder_release();
    h264_free_buffers();
    if (s_ppa) {
        (void)ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
}

/**
 * Make the next frame an IDR.
 *
 * The encoder has no explicit request for one, but it starts a new GOP whenever
 * the configured GOP length differs from the one in force (see
 * h264_hw_enc_gop_mode_process). Alternating between two adjacent lengths is
 * therefore a keyframe request, and costs nothing else.
 */
static void force_idr(void)
{
    if (!s_param) {
        return;
    }
    /*
     * Toggle between the wanted GOP length and one adjacent to it. Any change of
     * the configured length starts a new GOP, so this forces an IDR while the
     * effective length stays at (or one off) the intended value. Deriving the
     * pair from wanted_gop() every time - rather than stepping s_gop itself - is
     * deliberate: the old code did s_gop-- on each request, so after enough
     * keyframe requests (every reconnecting viewer sends one) the GOP walked down
     * to ~2 and nearly every frame became an IDR, wrecking the bitrate.
     */
    const uint8_t base = wanted_gop();
    const uint8_t alt = (base > 2u) ? (uint8_t)(base - 1u) : (uint8_t)(base + 1u);
    const uint8_t next = (s_gop == base) ? alt : base;
    if (esp_h264_enc_set_gop(&s_param->base, next) == ESP_H264_ERR_OK) {
        s_gop = next;
    }
}

static void follow_settings(void)
{
    if (!s_param) {
        return;
    }
    const uint32_t bitrate = wanted_bitrate();
    if (bitrate != s_bitrate) {
        if (esp_h264_enc_set_bitrate(&s_param->base, bitrate) == ESP_H264_ERR_OK) {
            s_bitrate = bitrate;
        }
    }
    /* GOP is left alone: it is also the keyframe-request mechanism, and the
     * encoder applies a changed value at the next IDR anyway. */
}

/* Encode one filled YUV buffer and publish it. Runs on the encode task, so it
 * overlaps the PPA conversion of the following frame. */
static void h264_encode_job(const h264_job_t *job)
{
    if (s_enc_w != job->hres || s_enc_h != job->vres) {
        if (encoder_open(job->hres, job->vres) != ESP_OK) {
            return;
        }
        /* A fresh encoder starts on an IDR, so nothing else to ask for. */
        (void)video_frame_take_keyframe_request();
    } else if (video_frame_take_keyframe_request()) {
        force_idr();
    }
    follow_settings();

    const uint32_t padded_w = MB_ALIGN(job->hres);
    const uint32_t padded_h = MB_ALIGN(job->vres);

    int slot = -1;
    uint8_t *dst = NULL;
    size_t cap = 0;
    if (video_frame_begin_write(&slot, &dst, &cap, 1000) != ESP_OK) {
        return;
    }

    const int64_t enc_started_us = esp_timer_get_time();
    esp_h264_enc_in_frame_t in = {
        .raw_data = {.buffer = s_yuv[job->slot],
                     .len = (uint32_t)((size_t)padded_w * padded_h * 3u / 2u)},
        .pts = (uint32_t)(esp_timer_get_time() / 1000),
    };
    esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = dst, .len = (uint32_t)cap}};
    esp_h264_err_t herr = esp_h264_enc_process(s_enc, &in, &out);
    capture_status_add_encode_time((uint32_t)(esp_timer_get_time() - enc_started_us));
    if (herr != ESP_H264_ERR_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "h264 encode: %s", h264_err_name(herr));
        if (herr == ESP_H264_ERR_OVERFLOW || herr == ESP_H264_ERR_MEM) {
            video_frame_request_keyframe();
        }
        return;
    }

    /*
     * Every frame goes out, including the near-empty ones a still screen
     * produces. There is no equivalent of the MJPEG skip here and no need for
     * one: an unchanged screen already costs a few hundred bytes per frame, and
     * a decoder that stops receiving has no way to tell a still picture from a
     * dead link.
     */
    video_frame_publish(slot, out.length, out.frame_type == ESP_H264_FRAME_TYPE_IDR);
    capture_status_add_frame(out.length);
}

static void h264_encode_task(void *arg)
{
    (void)arg;
    h264_job_t job;
    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
        if (job.slot < 0) {
            break; /* shutdown sentinel from h264_close() */
        }
        h264_encode_job(&job);
        /* Hand the YUV buffer back so the PPA stage can fill it again. */
        xQueueSend(s_free_slots, &job.slot, 0);
    }
    xSemaphoreGive(s_enc_done);
    vTaskDelete(NULL);
}

/*
 * Capture-loop side: convert the RGB frame into a free YUV buffer with the PPA
 * and hand it to the encode task. Returns as soon as the conversion is done -
 * the encode happens on the other task, overlapping the next conversion. The
 * frame is dropped (its buffer returned) if the encoder has not caught up.
 */
static esp_err_t h264_encode(capture_ctx_t *c, const void *src, bool force_publish)
{
    (void)force_publish;
    if (!s_ppa || !s_yuv[0] || !s_free_slots || !s_jobs) {
        return ESP_ERR_INVALID_STATE;
    }
    if (c->hres == 0 || c->vres == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int slot = -1;
    if (xQueueReceive(s_free_slots, &slot, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* The encoder is still busy with both buffers; skip this frame rather
         * than stall the capture loop. */
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t padded_w = MB_ALIGN(c->hres);
    const uint32_t padded_h = MB_ALIGN(c->vres);
    const int64_t ppa_started_us = esp_timer_get_time();

    /*
     * scale_x / scale_y stay at 1.0. A PPA transaction that scales while
     * writing YUV420 never completes - the blocking call waits forever and
     * takes the capture task with it. Downscaling, if it is ever wanted, has to
     * be a separate RGB pass.
     *
     * The destination picture is the macroblock-aligned size while the written
     * block is the real one, which puts each row at the stride the encoder
     * expects and leaves the padding rows untouched.
     */
    ppa_srm_oper_config_t srm = {
        .in = {.buffer = (void *)src,
               .pic_w = c->hres,
               .pic_h = c->vres,
               .block_w = c->hres,
               .block_h = c->vres,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
        .out = {.buffer = s_yuv[slot],
                .buffer_size = s_yuv_alloc[slot],
                .pic_w = padded_w,
                .pic_h = padded_h,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &srm);
    capture_status_add_ppa_time((uint32_t)(esp_timer_get_time() - ppa_started_us));
    if (err != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "ppa rgb->yuv: %s", esp_err_to_name(err));
        xQueueSend(s_free_slots, &slot, 0); /* return the unused buffer */
        return err;
    }

    const h264_job_t job = {.slot = slot, .hres = c->hres, .vres = c->vres};
    if (xQueueSend(s_jobs, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xQueueSend(s_free_slots, &slot, 0);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static const capture_codec_t s_h264 = {
    .name = "h264",
    .payload = VIDEO_PAYLOAD_H264,
    .open = h264_open,
    .close = h264_close,
    .encode = h264_encode,
};

const capture_codec_t *capture_codec_h264(void)
{
    return &s_h264;
}
