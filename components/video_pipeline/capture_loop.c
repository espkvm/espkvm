/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The capture loop: take the newest frame the CSI receiver finished, hand it to
 * whichever codec is selected, and keep the two apart.
 *
 * Everything that concerns getting pixels - mode switches, a source that went
 * to sleep, the frame rate limit, whether anyone is watching at all - lives
 * here; how those pixels become bytes on the wire lives in the codec modules.
 */
#include "capture_priv.h"

#include "sdkconfig.h"

#include <stdint.h>

#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_thermal.h"
#include "tc358743_hdmi_debug.h"
#include "video_frame.h"

/** vid_codec enum, in the order of s_codec_choices in the settings table. */
enum { CODEC_CHOICE_MJPEG = 0, CODEC_CHOICE_H264 = 1 };

static const capture_codec_t *codec_wanted(void)
{
    if (kvm_setting_int("vid_codec") == CODEC_CHOICE_H264 && kvm_cap_available(KVM_CAP_H264)) {
        return capture_codec_h264();
    }
    return capture_codec_mjpeg();
}

/**
 * Swap codecs. The store is emptied first so the outgoing codec can free its
 * buffers - at 1080p neither path leaves room for the other to stay allocated.
 * @return the codec now running, which is the old one when the switch failed.
 */
static const capture_codec_t *codec_switch(const capture_codec_t *from, const capture_codec_t *to)
{
    if (from) {
        /* A viewer sending a large frame over a slow link can hold a slot for
         * seconds. Waiting beats freeing memory out from under it. */
        if (!video_frame_quiesce(5000)) {
            return from;
        }
        from->close();
    }
    esp_err_t err = to->open();
    if (err == ESP_OK) {
        ESP_LOGI(CAPTURE_LOG_TAG, "codec: %s", to->name);
        return to;
    }
    ESP_LOGE(CAPTURE_LOG_TAG, "%s codec failed to start (%s)", to->name, esp_err_to_name(err));
    if (to != capture_codec_mjpeg()) {
        /* Falling back is better than a black screen, and the reason is
         * already in the log. */
        const capture_codec_t *mjpeg = capture_codec_mjpeg();
        if (mjpeg->open() == ESP_OK) {
            ESP_LOGW(CAPTURE_LOG_TAG, "codec: %s (fallback)", mjpeg->name);
            (void)kvm_setting_set_int("vid_codec", CODEC_CHOICE_MJPEG);
            return mjpeg;
        }
    }
    return NULL;
}

void capture_loop_run(capture_ctx_t *c)
{
    video_frame_store_init();

    const capture_codec_t *codec = codec_switch(NULL, codec_wanted());
    if (!codec) {
        ESP_LOGE(CAPTURE_LOG_TAG, "no codec could be started");
        vTaskDelete(NULL);
        return;
    }

    const unsigned bpp = capture_csi_bpp();
    int64_t hdmi_recover_cooldown_until_us = 0;
    int64_t last_encode_us = 0;
    /* Set after anything that invalidates what clients are holding. */
    bool force_publish = true;

    while (1) {
        /* The monitor task asks for mode switches here rather than doing them
         * itself, so the CSI receiver is never rebuilt under a running encode. */
        if (c->mode_change_pending) {
            const uint32_t want_h = c->pending_hres;
            const uint32_t want_v = c->pending_vres;
            c->mode_change_pending = false;
            esp_err_t me = capture_hw_apply_mode(c, want_h, want_v);
            if (me != ESP_OK) {
                ESP_LOGE(CAPTURE_LOG_TAG, "mode switch to %ux%u failed: %s", want_h, want_v,
                         esp_err_to_name(me));
            }
            hdmi_recover_cooldown_until_us = 0;
            /* After a mode switch every client is holding a frame of the wrong
             * size, so the next one must go out whatever it looks like. */
            force_publish = true;
            video_frame_request_keyframe();
            continue;
        }

        if (xSemaphoreTake(c->csi_done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            /* No signal is a normal state, not a fault: the target may simply be
             * off or asleep. Waiting quietly beats hammering the bridge with
             * hotplug cycles it cannot act on. */
            if (!c->signal_present) {
                continue;
            }
#if CONFIG_KVM_TC358743_ADV_DEBUG
            static uint32_t s_csi_timeout_logs;
#endif
            ESP_LOGW(CAPTURE_LOG_TAG, "csi frame wait timeout (dma_done_irqs=%lu)",
                     (unsigned long)c->csi_dma_done_irqs);
            capture_debug_csi_timeout(c, bpp, c->frame_bytes);
#if CONFIG_KVM_TC358743_ADV_DEBUG
            tc358743_debug_stall_extras(c->tc);
            if ((s_csi_timeout_logs++ % 8u) == 0u) {
                tc358743_debug_status(c->tc);
                tc358743_debug_bridge(c->tc);
            }
#endif
            int64_t now = (int64_t)esp_timer_get_time();
            if (now >= hdmi_recover_cooldown_until_us) {
                (void)capture_hw_hdmi_recover(c);
                hdmi_recover_cooldown_until_us = now + (int64_t)8 * 1000000;
            }
            continue;
        }
        hdmi_recover_cooldown_until_us = 0;
        while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
            /* Drop stale completions; done_fb always points at the newest completed frame. */
        }

        void *src = (void *)c->done_fb;
        if (!src) {
            continue;
        }

        /*
         * Nobody watching, nothing to encode. The encoder would otherwise keep
         * chewing through a 1080p frame every 50 ms, and the PSRAM bandwidth
         * that goes with it, for output no one reads. A viewer that arrives is
         * handed the last published frame at once and gets a fresh one within a
         * frame period.
         */
        if (video_frame_viewer_count() == 0) {
            continue;
        }

        const capture_codec_t *want = codec_wanted();
        if (want != codec) {
            const capture_codec_t *now_running = codec_switch(codec, want);
            if (!now_running) {
                ESP_LOGE(CAPTURE_LOG_TAG, "codec switch left nothing running");
                break;
            }
            if (now_running == codec) {
                continue; /* still busy; try again on the next frame */
            }
            codec = now_running;
            force_publish = true;
            continue;
        }

        /*
         * Frame rate limit, thermal state folded in: encoding is the only
         * thing here that makes real heat, so it is the only thing given up.
         * Dropping before the encode is what saves the time, so this is
         * checked here rather than at publish.
         */
        const int32_t fps_max = kvm_thermal_fps_limit((int)kvm_setting_int("vid_fps_max"));
        if (fps_max == 0) {
            /* Too hot to encode. Input and the web interface carry on; the
             * console reads the reason from the system status. */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (fps_max > 0) {
            const int64_t min_interval_us = 1000000 / fps_max;
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_encode_us < min_interval_us) {
                continue;
            }
            last_encode_us = now_us;
        }
        ESP_ERROR_CHECK(esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C));

        esp_err_t ee = codec->encode(c, src, force_publish);
        if (ee == ESP_OK) {
            force_publish = false;
        }
    }
    vTaskDelete(NULL);
}
