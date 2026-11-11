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

#include <inttypes.h>
#include "kvm_regions.h"
#include "capture.h"
#include "fps_gate.h"

#include "sdkconfig.h"

#include <stdint.h>

#include "esp_cache.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_thermal.h"
#include "esp_heap_caps.h"
#include "kvm_bridge.h"
#include "video_frame.h"

/*
 * Encoding a frame while it is still coming in (rev 3.x, H.264).
 *
 * A frame takes a whole source period to arrive - 33 ms at 30 Hz - and the
 * encoder used to start only after its last line. The encoder reads the frame
 * top to bottom, one macroblock row at a time, and at 1080p it is slower than
 * the capture (36-40 ms against 20-33). So it can start on the frame the DMA is
 * still writing, a few rows behind it, and never catch up. That takes most of a
 * source period off the delay.
 *
 * The lead is two macroblock rows, or more where the encoder is not much slower
 * than the capture (720p60: 16 ms each): enough that an encoder 25 % faster
 * than its average still finishes behind the last line. Without an encode time
 * measured yet, or with an interlaced source, frames are taken whole.
 *
 * Not on the M5Stack: its frames go through a byte reorder and a frame gate
 * first. Not for MJPEG: the JPEG engine is faster than the capture.
 */
#define CAPTURE_EARLY_ENCODE (CAPTURE_DIRECT_ENCODE && !CAPTURE_YUV_SWAP)

#if CAPTURE_EARLY_ENCODE
#define EARLY_LEAD_LINES 32


static esp_timer_handle_t s_wake_timer;
static TaskHandle_t s_loop_task;

static void wake_loop(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_loop_task);
}

/* The tick is 10 ms; this waits to the microsecond. */
static void sleep_until(int64_t t_us)
{
    const int64_t d = t_us - esp_timer_get_time();
    if (d <= 50) {
        return;
    }
    if (!s_wake_timer) {
        s_loop_task = xTaskGetCurrentTaskHandle();
        const esp_timer_create_args_t args = {.callback = wake_loop, .name = "cap_wake"};
        if (esp_timer_create(&args, &s_wake_timer) != ESP_OK) {
            s_wake_timer = NULL;
            return;
        }
    }
    (void)ulTaskNotifyTake(pdTRUE, 0); /* a stale wake from before */
    if (esp_timer_start_once(s_wake_timer, (uint64_t)d) == ESP_OK) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

/**
 * When the encoder may start on the frame going into slot @p idx, and when the
 * frame will be complete (@p landing_us); 0 when it should wait for the whole
 * frame. The blanking is taken as 1/25 of the lines, which all the usual modes
 * are close to.
 */
typedef struct {
    int64_t period_us, active_us, blank_us, line_ns;
} frame_timing_t;

/* The mode's timing, from the status. The blanking is taken as 1/25 of the
 * lines, which all the usual modes are close to. */
static bool frame_timing(const capture_ctx_t *c, const kvm_video_status_t *st, frame_timing_t *t)
{
    if (st->input_hz == 0 || st->interlaced || c->vres == 0) {
        return false;
    }
    const int64_t period_ns = 1000000000LL / st->input_hz;
    t->period_us = period_ns / 1000;
    t->line_ns = period_ns * 24 / ((int64_t)c->vres * 25);
    t->active_us = t->line_ns * c->vres / 1000;
    t->blank_us = t->period_us - t->active_us;
    return true;
}

/*
 * When the frame coming in now started arriving: the one after the frame that
 * landed last. With the gate closed in between, the bridge waited for a frame
 * start after it was opened again, so it is the first one from then. 0 when
 * frames have stopped.
 */
static int64_t next_data_us(const frame_timing_t *t)
{
    int64_t d = capture_hw_last_landed_us() + t->blank_us;
    const int64_t opened = capture_hw_gate_opened_us();
    if (opened - d > 1000000) {
        return 0;
    }
    while (d < opened) {
        d += t->period_us;
    }
    return d;
}

/**
 * When the encoder may start on the frame going into slot @p idx, when it
 * started arriving (@p data_us) and when it will be complete (@p landing_us);
 * 0 when it should wait for the whole frame.
 */
static int64_t early_start_us(const capture_ctx_t *c, int idx, int64_t *data_us, int64_t *landing_us,
                              size_t *lead_bytes)
{
    kvm_video_status_t st;
    capture_status_get(&st);
    frame_timing_t t;
    if (idx < 0 || st.encode_us == 0 || !frame_timing(c, &st, &t)) {
        return 0;
    }
    int64_t lead_us = t.line_ns * EARLY_LEAD_LINES / 1000;
    const int64_t catch_up_us = t.active_us - (int64_t)st.encode_us * 3 / 4;
    if (catch_up_us > lead_us) {
        lead_us = catch_up_us;
    }
    const int64_t d = next_data_us(&t);
    if (lead_us >= t.active_us || d == 0) {
        return 0;
    }
    *data_us = d;
    *landing_us = d + t.active_us;
    *lead_bytes = (size_t)(lead_us * 1000 / t.line_ns) * (c->frame_bytes / c->vres);
    return d + lead_us;
}

/* A frame is coming in that has not been encoded yet: the loop need not wait
 * for it to land. */
static bool early_frame_coming(const capture_ctx_t *c, int64_t last_data_us)
{
    kvm_video_status_t st;
    capture_status_get(&st);
    frame_timing_t t;
    return c->write_fb_idx >= 0 && frame_timing(c, &st, &t) && next_data_us(&t) > last_data_us;
}

/*
 * The frame gate for early encode. When the source is so fast that the encoder
 * takes every other frame anyway - 1080p50: 20 ms frames, 36 ms encodes - the
 * frames in between only take memory time from it, and with them in the way it
 * starts on a frame at whatever point it gets free. With the gate the bridge
 * stays closed while it encodes and opens as it finishes, so it starts every
 * frame it takes from the top, and no longer competes with the capture for
 * PSRAM. On when half the source's rate is at least most of what the encoder
 * manages; 1080p30 and 720p60 run without it.
 */
static void early_gate(bool want)
{
    static bool s_on;
    if (want != s_on) {
        s_on = want;
        capture_hw_frame_gate_set(want);
    }
}

static bool early_gate_wanted(void)
{
    kvm_video_status_t st;
    capture_status_get(&st);
    return (uint64_t)st.input_hz * st.encode_us >= 1700000u;
}
#endif

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
        if (from == capture_codec_h264()) {
            /* The dashcam only runs on H.264; its ring is dead weight now and
             * sits in the space the next codec wants. */
            (void)capture_release_memory(false);
        }
    }
    esp_err_t err = to->open();
    if (err == ESP_OK) {
        ESP_LOGI(CAPTURE_LOG_TAG, "codec: %s", to->name);
        return to;
    }
    ESP_LOGE(CAPTURE_LOG_TAG, "%s codec failed to start (%s); PSRAM %u KB free, largest block %u KB",
             to->name, esp_err_to_name(err),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u));
    if (to != capture_codec_mjpeg()) {
        /* Falling back is better than a black screen, and the reason is
         * already in the log. The setting is left alone, for the same reason
         * the runtime fallback below leaves it alone: this is one boot's
         * failure, not a new preference, and a persisted "MJPEG" is exactly
         * what would stop H.264 being tried on the next boot when the memory
         * is back. */
        const capture_codec_t *mjpeg = capture_codec_mjpeg();
        if (mjpeg->open() == ESP_OK) {
            ESP_LOGW(CAPTURE_LOG_TAG, "codec: %s (fallback)", mjpeg->name);
            return mjpeg;
        }
    }
    /*
     * Both refused, and the one that was running is now closed - which is how
     * the device ended up with no picture at all until it was restarted, seen
     * twice while measuring on the M5Stack board. Each codec wants several
     * megabytes of PSRAM in a few large pieces, and after a close and a failed
     * open the heap is not always the shape it was.
     *
     * Going back is the one thing likely to work: the same buffers were there a
     * moment ago. If even that fails there is nothing left to try, and the
     * caller idles and retries rather than pretending a codec is running.
     */
    if (from && from != to) {
        if (from->open() == ESP_OK) {
            ESP_LOGW(CAPTURE_LOG_TAG, "codec: %s again - %s could not be started", from->name,
                     to->name);
            return from;
        }
        ESP_LOGE(CAPTURE_LOG_TAG, "no codec could be started; the picture stops until memory frees up");
    }
    return NULL;
}

/* The screen areas look at the same frame, at the same moment, as the text
   reader and the flat-colour check: held, in cache, still in capture order.
   24-bit frames land blue first. */
static void regions_tick(capture_ctx_t *c, const void *src)
{
    const region_frame_t f = {
        .px = src,
        .w = (uint16_t)c->hres,
        .h = (uint16_t)c->vres,
        .bytes_per_px = (uint8_t)capture_pixfmt_bytes(),
        .luma_off = capture_pixfmt()->luma_first ? 0u : 1u,
        .bgr = true,
    };
    kvm_regions_scan(&f);
}

bool capture_read_newest(capture_ctx_t *c, capture_frame_fn_t fn)
{
    portENTER_CRITICAL(&c->fb_lock);
    const int hidx = c->ready_fb_idx;
    c->held_fb_idx = hidx;
    portEXIT_CRITICAL(&c->fb_lock);
    if (hidx < 0) {
        return false;
    }
    void *src = c->fb[hidx];
    const bool ok = esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK;
    if (ok) {
        fn(c, src);
    }
    portENTER_CRITICAL(&c->fb_lock);
    c->held_fb_idx = -1;
    portEXIT_CRITICAL(&c->fb_lock);
    return ok;
}

/* With nobody watching nothing is encoded. An area that reports somewhere
   still gets its look: hold the newest frame, at the areas' own pace. */
static void regions_idle(capture_ctx_t *c)
{
    if (!c->signal_present || !kvm_regions_due_unwatched()) {
        return;
    }
    (void)capture_read_newest(c, regions_tick);
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

    int64_t hdmi_recover_cooldown_until_us = 0;
    int64_t switch_retry_us = 0; /* after a refused codec switch */
    int64_t next_encode_us = 0; /* frame rate limit: when the next encode is due */
    int64_t last_mjpeg_us = 0;  /* when an MJPEG encode was last let through */
#if CAPTURE_EARLY_ENCODE
    /* When the frame last encoded early started arriving, so it is not taken
     * twice; and whether the turn after an encode may skip waiting for a frame. */
    int64_t early_data_us = 0;
    bool early_next = false;
#endif
    /* Set after anything that invalidates what clients are holding. */
    bool force_publish = true;

    while (1) {
        esp_task_wdt_reset();
        /*
         * No codec is open: every path out of here closed one and could not
         * open another, which on this device means memory. Keep the loop alive
         * and keep trying rather than killing capture outright - the block that
         * could not be had may well be free again in a few seconds, and a
         * pipeline that gave up needs a reboot to come back.
         */
        if (!codec) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            /* Opening a codec waits for clients to let go of their frames and
             * then asks for megabytes; between the two, one turn through here
             * can outlast the task watchdog, which reboots a device that is
             * only short of memory. */
            esp_task_wdt_reset();
            codec = codec_switch(NULL, codec_wanted());
            if (codec) {
                ESP_LOGW(CAPTURE_LOG_TAG, "codec recovered: %s", codec->name);
                force_publish = true;
            }
            continue;
        }

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

        /* Right after an early encode the next frame is usually coming in
         * already; start on it instead of waiting for it to land. Once per
         * encode, so the turns that skip a frame still wait for one. */
        TickType_t frame_wait = pdMS_TO_TICKS(2000);
#if CAPTURE_EARLY_ENCODE
        if (early_next && early_frame_coming(c, early_data_us)) {
            frame_wait = 0;
        }
        early_next = false;
#endif
        if (xSemaphoreTake(c->csi_done_sem, frame_wait) != pdTRUE && frame_wait != 0) {
            /* No signal is a normal state, not a fault: the target may simply be
             * off or asleep. Waiting quietly beats hammering the bridge with
             * hotplug cycles it cannot act on. */
            if (!c->signal_present || c->mode_too_fast) {
                continue; /* too fast was said once, by the monitor */
            }
            /* The link state says whether the bridge drives the lanes at all:
               a working link between frames reads every lane stopped (LP-11);
               no lane stopped and no high-speed clock means nothing holds them. */
            capture_csi_link_t link;
            capture_hw_csi_link(&link);
            ESP_LOGW(CAPTURE_LOG_TAG,
                     "csi frame wait timeout (dma_done_irqs=%lu, clk_hs=%u, stop d0=%u d1=%u clk=%u, "
                     "phy_err=0x%08" PRIx32 ")",
                     (unsigned long)c->csi_dma_done_irqs, link.clk_hs, link.stop_d0, link.stop_d1,
                     link.stop_clk, link.phy_err);
#if CONFIG_KVM_TC358743_ADV_DEBUG
            static uint32_t s_csi_timeout_logs;
            capture_debug_csi_timeout(c, capture_csi_bpp(), c->frame_bytes);
            kvm_bridge_debug_stall_extras(&c->bridge);
            if ((s_csi_timeout_logs++ % 8u) == 0u) {
                kvm_bridge_debug_status(&c->bridge);
                kvm_bridge_debug_bridge(&c->bridge);
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
        capture_hw_frames_flowing();
        /* A reference for the timeout line: what a working link reads. */
        static int64_t s_link_sampled_us;
        if (esp_timer_get_time() - s_link_sampled_us > 1000000) {
            capture_csi_link_t link;
            capture_hw_csi_link(&link);
            s_link_sampled_us = esp_timer_get_time();
        }
        while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
            /* Drop stale completions; done_fb always points at the newest completed frame. */
        }
        /* The next frame lands while this one is worked on (M5Stack; a no-op
         * elsewhere). */
        capture_hw_frame_gate_open();

        if (c->ready_fb_idx < 0) {
            continue; /* no frame has completed yet */
        }

        /*
         * The codec is settled before the "nobody is watching" check, not after.
         * The MJPEG paths - the stream, a single frame, the viewing token - all
         * refuse to serve while H.264 is running, so a device left on H.264 with
         * no viewer would never switch: the only way to become a viewer is
         * through a path that is closed until the switch happens. Switching with
         * nobody watching costs one open and one close.
         */
        const capture_codec_t *want = codec_wanted();
        if (want != codec && esp_timer_get_time() >= switch_retry_us) {
            const capture_codec_t *now_running = codec_switch(codec, want);
            if (!now_running) {
                /* Usually memory. The loop's top retries every two seconds; leaving
                 * the loop instead ended the capture task, and the watchdog then
                 * restarted the whole device. */
                ESP_LOGE(CAPTURE_LOG_TAG, "codec switch left nothing running; retrying");
                /*
                 * Retrying alone is not enough when the setting is the reason:
                 * codec_wanted() would ask for H.264 again every two seconds
                 * for ever, and MJPEG - which had just been closed to make room
                 * for it - would never come back. Take H.264 off the table for
                 * this run and the next turn settles on a picture.
                 */
                if (want == capture_codec_h264()) {
                    kvm_cap_report(KVM_CAP_H264, false,
                                   "the encoder could not be built at this resolution; using "
                                   "MJPEG until the next restart");
                }
                codec = NULL;
                continue;
            }
            if (now_running == codec) {
                /* Refused, and the old codec is back. Trying again on the next
                 * frame closed and reopened codecs twice a second until the
                 * task watchdog reset the board (M5Stack, 2026-09-23). */
                switch_retry_us = esp_timer_get_time() + 10 * 1000 * 1000;
                if (want == capture_codec_h264()) {
                    /* Nearly always memory, and it will not get better on its
                     * own; say so instead of retrying every 10 s. */
                    kvm_cap_report(KVM_CAP_H264, false,
                                   "not enough memory to switch to H.264 at this resolution; "
                                   "it starts after a restart");
                }
                continue;
            }
            codec = now_running;
            force_publish = true;
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
            /* Nobody to encode for - but a watch set to look for words on the
             * screen exists for exactly this state, so give it the frame. */
            capture_screentext_idle(c);
            regions_idle(c);
            capture_thumb_idle(c);
            continue;
        }

        /*
         * Frame rate limit, thermal state folded in: encoding is the only
         * thing here that makes real heat, so it is the only thing given up.
         * Dropping before the encode is what saves the time, so this is
         * checked here rather than at publish.
         */
        int32_t fps_max = kvm_thermal_fps_limit((int)kvm_setting_int("vid_fps_max"));
        if (fps_max != 0 && video_frame_upload_active() &&
            (fps_max < 0 || fps_max > VIDEO_UPLOAD_FPS)) {
            fps_max = VIDEO_UPLOAD_FPS; /* see video_frame_upload_begin */
        }
        if (fps_max == 0) {
            /* Too hot to encode. Input and the web interface carry on; the
             * console reads the reason from the system status. */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (fps_max > 0 &&
            !fps_gate_take(&next_encode_us, esp_timer_get_time(), 1000000 / fps_max)) {
            continue;
        }
        /*
         * MJPEG only: encode when a viewer can take the frame soon. The encoder
         * made 50 fps while the senders, held back by TLS, took 10, and the
         * spare encodes cost the PSRAM bandwidth and CPU the sends need: capped
         * at 15 fps a viewer got 12% more. H.264 keeps its steady pace: its
         * early encode is what keeps the delay down, and a recording needs
         * every frame. A frame still goes out every 250 ms whatever the answer.
         */
        if (codec == capture_codec_mjpeg()) {
            kvm_video_status_t st;
            capture_status_get(&st);
            const int64_t now = esp_timer_get_time();
            const int64_t lead_us = (int64_t)st.encode_us + (st.input_hz ? 1000000 / st.input_hz : 20000);
            if (now - last_mjpeg_us < 250000 && !video_frame_wanted(lead_us)) {
                capture_screentext_idle(c);
                regions_idle(c);
                capture_thumb_idle(c);
                continue;
            }
            last_mjpeg_us = now;
        }
        int64_t landed_us = 0;
        bool early = false;
#if CAPTURE_EARLY_ENCODE
        int64_t early_at_us = 0;
        size_t early_lead_bytes = 0;
        early_gate(codec == capture_codec_h264() && early_gate_wanted());
        if (codec == capture_codec_h264()) {
            /* The frame the DMA is writing now, if it may be started on. */
            const int w = c->write_fb_idx;
            int64_t data_us = 0;
            early_at_us = early_start_us(c, w, &data_us, &landed_us, &early_lead_bytes);
            if (early_at_us > 0 && data_us <= early_data_us) {
                early_at_us = 0; /* that one was encoded already */
            }
            if (early_at_us > 0) {
                portENTER_CRITICAL(&c->fb_lock);
                early = (c->write_fb_idx == w || c->ready_fb_idx == w);
                if (early) {
                    c->held_fb_idx = w;
                }
                portEXIT_CRITICAL(&c->fb_lock);
                if (early) {
                    early_data_us = data_us;
                }
            }
        }
#endif
        int hidx;
        if (early) {
            hidx = c->held_fb_idx;
        } else {
            /*
             * Take the newest complete frame and hold it for the whole encode:
             * marking it held keeps the producer's get_new from ever targeting
             * it, so the free-running CSI DMA cannot overwrite it mid-read.
             */
            portENTER_CRITICAL(&c->fb_lock);
            hidx = c->ready_fb_idx;
            c->held_fb_idx = hidx;
            portEXIT_CRITICAL(&c->fb_lock);
            if (hidx < 0) {
                continue;
            }
            landed_us = capture_hw_fb_landed_us(hidx);
        }
        if (!capture_park_frame_begin()) {
            /* A restart is coming: start nothing the DMA would be cut off in. */
            portENTER_CRITICAL(&c->fb_lock);
            c->held_fb_idx = -1;
            portEXIT_CRITICAL(&c->fb_lock);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        void *src = c->fb[hidx];
#if CAPTURE_EARLY_ENCODE
        if (early) {
            sleep_until(early_at_us);
            /* The timing above is worked out, not seen: blanking is taken as
             * 1/25 of the lines. The DMA's own write position settles it - the
             * lead must be in the buffer before the encoder reads any of it. A
             * frame period at most. */
            const int64_t give_up_us = esp_timer_get_time() + (landed_us - early_at_us);
            while (capture_hw_dma_written(hidx) < early_lead_bytes && esp_timer_get_time() < give_up_us) {
                sleep_until(esp_timer_get_time() + 300);
            }
        }
#endif
        video_frame_set_source_time(landed_us);
        if (landed_us > 0) {
            /* Early frames start before they land: no wait at all. */
            const int64_t waited = esp_timer_get_time() - landed_us;
            capture_status_add_wait(waited > 0 ? (uint32_t)waited : 0u);
        }

        /* The frame is held and in cache: the one moment it can be read as text
         * without paying for either. Costs a few hundred reads unless this is a
         * text mode that has stopped moving. A frame still coming in is read
         * after its encode instead. */
        if (!early) {
            ESP_ERROR_CHECK(esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C));
            capture_screentext_tick(c, src);
            capture_flat_tick(c, src);
            regions_tick(c, src);
            capture_thumb_tick(c, src);
        }

#if CAPTURE_YUV_SWAP
        /*
         * The readers above want the captured bytes as they came. The JPEG
         * engine wants them reordered; the H.264 path wants them as they came
         * too, because its own rearrangement starts from the wire order. So the
         * pass runs for one codec and not the other, and a snapshot - which is
         * a JPEG whatever is streaming - pays for the pass itself on the rare
         * frame someone asks for a picture of.
         */
        const bool h264_running = (codec == capture_codec_h264());
        if (!h264_running) {
            void *ordered = capture_yuv_swap(c, src);
            if (!ordered) {
                capture_park_frame_end();
                continue; /* it said why; a green frame helps nobody */
            }
            src = ordered;
        }
#endif

#if CAPTURE_YUV_SWAP
        if (h264_running && capture_snapshot_wanted()) {
            void *ordered = capture_yuv_swap(c, src);
            if (ordered) {
                capture_snapshot_tick(c, ordered);
            }
            /* Only for this picture: kept, its 4 MB is what MJPEG can no
             * longer find in one piece when the operator switches back. */
            capture_yuv_swap_release();
        } else {
            capture_snapshot_tick(c, src);
        }
#else
        if (!early) {
            capture_snapshot_tick(c, src);
        }
#endif

        esp_err_t ee = codec->encode(c, src, force_publish);
#if CAPTURE_EARLY_ENCODE
        if (early) {
            /* Nearly always landed by now: the encoder is the slower of the
             * two. If not, the readers skip this one. */
            portENTER_CRITICAL(&c->fb_lock);
            const bool landed = c->write_fb_idx != hidx;
            portEXIT_CRITICAL(&c->fb_lock);
            if (landed) {
                ESP_ERROR_CHECK(esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C));
                capture_screentext_tick(c, src);
                capture_flat_tick(c, src);
                regions_tick(c, src);
                capture_thumb_tick(c, src);
                capture_snapshot_tick(c, src);
            }
            early_next = true;
            /* Let the next frame through from its start while the loop comes
             * round; it must not wait behind a closed gate. */
            capture_hw_frame_gate_open();
        }
#endif
        if (ee == ESP_OK) {
            force_publish = false;
        } else if (codec != capture_codec_mjpeg() &&
                   (ee == ESP_ERR_NO_MEM || capture_h264_encoder_failed())) {
            /*
             * The H.264 encoder could not be built - almost always because its
             * reference frame wants one contiguous block that PSRAM can no
             * longer hand out, after hours of uptime, at the moment the input
             * resolution changed and forced a rebuild.
             *
             * There is nothing to retry: the block needed does not depend on
             * when it is asked for. So say why, and carry on in MJPEG - which
             * needs no such buffer - because a working picture and an
             * explanation beat a black rectangle and a log filling at thirty
             * lines a second.
             *
             * The codec test above is not decoration. Without it any failed
             * MJPEG frame - once already switched - matched this branch too and
             * "fell back" from MJPEG to MJPEG, which closes and reopens the
             * codec: the status line flips between mjpeg and none forever.
             */
            kvm_cap_report(KVM_CAP_H264, false,
                           "the encoder could not get memory for this resolution; using MJPEG "
                           "until the next restart");
            /*
             * The setting is deliberately NOT written. Falling back is this
             * run's decision, not a new preference: the memory that could not
             * be had is most likely to be available again on a fresh boot,
             * which is exactly when a persisted "MJPEG" would stop it being
             * tried. The capability being marked unavailable already keeps
             * codec_wanted() on MJPEG for the rest of this run.
             */
            codec = codec_switch(codec, capture_codec_mjpeg());
            if (codec) {
                force_publish = true;
            } else {
                /* Nothing is open now - codec_switch closed the old one before
                 * finding it could not open MJPEG either. Encoding through a
                 * closed codec is not an option, so idle and let the retry at
                 * the top of the loop have another go when memory frees up. */
                ESP_LOGE(CAPTURE_LOG_TAG, "no codec left running; retrying");
            }
        }

        capture_park_frame_end();
        portENTER_CRITICAL(&c->fb_lock);
        c->held_fb_idx = -1;
        portEXIT_CRITICAL(&c->fb_lock);
    }
    vTaskDelete(NULL);
}
