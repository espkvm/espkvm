/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A small RGB565 copy of the screen, for a status display's picture page.
 *
 * The PPA scales the captured frame and converts it in one pass, straight
 * from the frame the capture task is holding, so nothing is copied at full
 * size. It runs only when the display asks, which is at most a few times a
 * second, and with nobody watching it reads the newest frame the way the
 * screen readers do, so the encoder stays off.
 *
 * The output buffer belongs to this file and is kept once made: the display
 * asks again within a fraction of a second, and freeing 115 KB of PSRAM that
 * often would only cut up the heap.
 */
#include <string.h>

#include "capture.h"
#include "capture_pixfmt.h"
#include "capture_priv.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if !CAPTURE_YUV_SWAP

static SemaphoreHandle_t s_one;  /* one request at a time */
static SemaphoreHandle_t s_done; /* the capture task answered */
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;
static bool s_wanted;
static int s_max_w, s_max_h; /* the box asked for */
static int s_w, s_h;         /* what came out */
static esp_err_t s_err;

static ppa_client_handle_t s_ppa;
static uint16_t *s_buf; /* the PPA's output, RGB565 */
static size_t s_buf_len;

static esp_err_t scale(capture_ctx_t *c, const void *frame, int max_w, int max_h)
{
    if (!s_ppa) {
        const ppa_client_config_t cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
        esp_err_t err = ppa_register_client(&cfg, &s_ppa);
        if (err != ESP_OK) {
            return err;
        }
    }
    /* The PPA scales in sixteenths. The largest that fits, at least one, and
       never more than the frame itself. */
    int s16 = 16 * max_w / (int)c->hres;
    const int sy = 16 * max_h / (int)c->vres;
    s16 = sy < s16 ? sy : s16;
    s16 = s16 < 1 ? 1 : s16 > 16 ? 16 : s16;
    const int w = (int)c->hres * s16 / 16;
    const int h = (int)c->vres * s16 / 16;
    if (w < 1 || h < 1 || w > max_w || h > max_h) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t align = 64;
    (void)esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align);
    align = align ? align : 64;
    const size_t need = ((size_t)max_w * max_h * 2u + align - 1) / align * align;
    if (s_buf_len < need) {
        heap_caps_free(s_buf);
        s_buf_len = 0;
        s_buf = heap_caps_aligned_calloc(align, 1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_buf) {
            return ESP_ERR_NO_MEM;
        }
        s_buf_len = need;
    }

    const capture_pixfmt_t *pf = capture_pixfmt();
    ppa_srm_oper_config_t srm = {
        .in = {.buffer = frame,
               .pic_w = c->hres,
               .pic_h = c->vres,
               .block_w = c->hres,
               .block_h = c->vres,
               .srm_cm = pf->bpp == 16 ? PPA_SRM_COLOR_MODE_YUV422_UYVY : PPA_SRM_COLOR_MODE_RGB888,
               .yuv_range = pf->full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT,
               .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .out = {.buffer = s_buf,
                .buffer_size = s_buf_len,
                .pic_w = w,
                .pic_h = h,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .scale_x = s16 / 16.0f,
        .scale_y = s16 / 16.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &srm);
    if (err == ESP_OK) {
        /* Written by DMA: drop whatever the cache holds of the old picture. */
        err = esp_cache_msync(s_buf, need, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        s_w = w;
        s_h = h;
    }
    return err;
}

void capture_thumb_tick(capture_ctx_t *c, const void *frame)
{
    portENTER_CRITICAL(&s_mu);
    const bool wanted = s_wanted;
    const int max_w = s_max_w, max_h = s_max_h;
    portEXIT_CRITICAL(&s_mu);
    if (!wanted) {
        return;
    }
    const esp_err_t err = scale(c, frame, max_w, max_h);
    if (err != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "display picture: %s", esp_err_to_name(err));
    }
    portENTER_CRITICAL(&s_mu);
    const bool still = s_wanted; /* the caller may have given up meanwhile */
    s_wanted = false;
    s_err = err;
    portEXIT_CRITICAL(&s_mu);
    if (still) {
        xSemaphoreGive(s_done);
    }
}

void capture_thumb_idle(capture_ctx_t *c)
{
    portENTER_CRITICAL(&s_mu);
    const bool wanted = s_wanted;
    portEXIT_CRITICAL(&s_mu);
    if (wanted && c->signal_present) {
        (void)capture_read_newest(c, capture_thumb_tick);
    }
}

esp_err_t capture_thumbnail(uint16_t *dst, int max_w, int max_h, bool swap, int *out_w,
                            int *out_h, uint32_t timeout_ms)
{
    if (!dst || max_w < 1 || max_h < 1 || !out_w || !out_h) {
        return ESP_ERR_INVALID_ARG;
    }
    kvm_video_status_t vs;
    capture_status_get(&vs);
    if (!vs.signal) {
        return ESP_ERR_NOT_FOUND;
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
    s_max_w = max_w;
    s_max_h = max_h;
    s_wanted = true;
    portEXIT_CRITICAL(&s_mu);

    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        err = s_err;
    } else {
        portENTER_CRITICAL(&s_mu);
        s_wanted = false;
        portEXIT_CRITICAL(&s_mu);
    }
    if (err == ESP_OK) {
        /* The buffer is only written while a request is out, and this one
           has been answered, so it holds still for the copy. */
        const int n = s_w * s_h;
        if (swap) {
            for (int i = 0; i < n; i++) {
                dst[i] = (uint16_t)((s_buf[i] >> 8) | (s_buf[i] << 8));
            }
        } else {
            memcpy(dst, s_buf, (size_t)n * 2u);
        }
        *out_w = s_w;
        *out_h = s_h;
    }
    xSemaphoreGive(s_one);
    return err;
}

#else /* CAPTURE_YUV_SWAP */

/* The LT6911D's YUV422 before chip rev 3.0: the PPA cannot read it. */
void capture_thumb_tick(capture_ctx_t *c, const void *frame)
{
    (void)c;
    (void)frame;
}

void capture_thumb_idle(capture_ctx_t *c)
{
    (void)c;
}

esp_err_t capture_thumbnail(uint16_t *dst, int max_w, int max_h, bool swap, int *out_w,
                            int *out_h, uint32_t timeout_ms)
{
    (void)dst;
    (void)max_w;
    (void)max_h;
    (void)swap;
    (void)out_w;
    (void)out_h;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
