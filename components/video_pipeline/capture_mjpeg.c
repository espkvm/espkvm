/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * MJPEG: one hardware-encoded JPEG per frame.
 *
 * Every browser can display it and it survives any packet loss, which is why it
 * stays the default and the fallback. What it cannot do is exploit a screen
 * that does not change - a still desktop costs the same 82 KB per frame as a
 * moving one - so this module drops a re-encode that came out byte-identical.
 */
#include "capture_priv.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "video_frame.h"

static jpeg_encoder_handle_t s_enc;
static uint8_t *s_buf[VIDEO_SLOT_COUNT];
static uint8_t s_quality = 60;

/*
 * A still screen still needs an occasional frame so a viewer can tell the link
 * is alive. New viewers do not depend on it - they are handed the published
 * frame on connect - so this can be long.
 */
#define STATIC_KEEPALIVE_US (5 * 1000 * 1000)

static int64_t s_last_publish_us;

static void mjpeg_free_buffers(void)
{
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
        if (s_buf[i]) {
            free(s_buf[i]);
            s_buf[i] = NULL;
        }
    }
}

static esp_err_t mjpeg_open(void)
{
    jpeg_encode_engine_cfg_t jcfg = {.intr_priority = 0, .timeout_ms = 120};
    esp_err_t err = jpeg_new_encoder_engine(&jcfg, &s_enc);
    kvm_cap_report(KVM_CAP_MJPEG, err == ESP_OK, "hardware JPEG encoder open failed (%s)",
                   esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    /* Sized for the largest mode, not the one we happened to boot into: the
     * target can switch from a 640x480 firmware screen to 1080p at any moment,
     * and a short output buffer would fail every encode from then on. */
    const size_t want = (size_t)CAPTURE_MAX_H_RES * (size_t)CAPTURE_MAX_V_RES + 384u * 1024u;
    jpeg_encode_memory_alloc_cfg_t jmem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    size_t smallest = SIZE_MAX;
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
        size_t got = 0;
        s_buf[i] = jpeg_alloc_encoder_mem(want, &jmem, &got);
        if (!s_buf[i]) {
            ESP_LOGE(CAPTURE_LOG_TAG, "jpeg buffer %d of %zu bytes failed", i, want);
            mjpeg_free_buffers();
            jpeg_del_encoder_engine(s_enc);
            s_enc = NULL;
            return ESP_ERR_NO_MEM;
        }
        if (got < smallest) {
            smallest = got;
        }
    }
    s_quality = (uint8_t)kvm_setting_int("jpg_quality");
    s_last_publish_us = 0;
    video_frame_install(VIDEO_PAYLOAD_JPEG, s_buf, VIDEO_SLOT_COUNT, smallest);
    return ESP_OK;
}

static void mjpeg_close(void)
{
    mjpeg_free_buffers();
    if (s_enc) {
        jpeg_del_encoder_engine(s_enc);
        s_enc = NULL;
    }
}

static esp_err_t mjpeg_encode(capture_ctx_t *c, const void *src, bool force_publish)
{
    if (!s_enc) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t q = s_quality;
    if (q < 1u) {
        q = 1u;
    } else if (q > 100u) {
        q = 100u;
    }
    jpeg_encode_cfg_t enc = {.width = c->hres,
                             .height = c->vres,
                             .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
                             .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
                             .image_quality = q};

    int slot = -1;
    uint8_t *dst = NULL;
    size_t cap = 0;
    esp_err_t err = video_frame_begin_write(&slot, &dst, &cap, 1000);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t out_sz = 0;
    const int64_t started_us = esp_timer_get_time();
    err = jpeg_encoder_process(s_enc, &enc, src, (uint32_t)c->frame_bytes, dst, (uint32_t)cap,
                               &out_sz);
    capture_status_add_encode_time((uint32_t)(esp_timer_get_time() - started_us));
    if (err != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "jpeg %s", esp_err_to_name(err));
        return err;
    }

    /*
     * MJPEG has no concept of an unchanged frame: it would re-send the same
     * 82 KB thirty times a second. The hardware encoder is deterministic, so an
     * identical source frame produces a byte-identical JPEG - comparing the
     * 82 KB output is exact and roughly seventy times cheaper than comparing
     * the 6 MB source, which would cost more than the encode itself.
     */
    const int64_t now_us = esp_timer_get_time();
    bool publish = true;
    if (kvm_setting_bool("vid_adapt") && !force_publish) {
        publish = !video_frame_front_matches(dst, out_sz);
    }
    /* Publish periodically even when nothing moves: a client that connects to a
     * still screen would otherwise wait forever for its first frame. */
    if (!publish && now_us - s_last_publish_us >= STATIC_KEEPALIVE_US) {
        publish = true;
    }
    if (!publish) {
        capture_status_add_skipped();
        return ESP_ERR_INVALID_STATE;
    }
    s_last_publish_us = now_us;
    video_frame_publish(slot, out_sz, true);
    capture_status_add_frame(out_sz);
    return ESP_OK;
}

static const capture_codec_t s_mjpeg = {
    .name = "mjpeg",
    .payload = VIDEO_PAYLOAD_JPEG,
    .open = mjpeg_open,
    .close = mjpeg_close,
    .encode = mjpeg_encode,
};

const capture_codec_t *capture_codec_mjpeg(void)
{
    return &s_mjpeg;
}

static void on_setting_changed(const char *key, void *user)
{
    (void)user;
    if (strcmp(key, "jpg_quality") == 0 || strcmp(key, "*") == 0) {
        s_quality = (uint8_t)kvm_setting_int("jpg_quality");
    }
}

void capture_mjpeg_bind_settings(void)
{
    s_quality = (uint8_t)kvm_setting_int("jpg_quality");
    ESP_ERROR_CHECK(kvm_settings_subscribe(on_setting_changed, NULL));
}
