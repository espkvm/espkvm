/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The dashcam: the last minutes of the screen kept in memory, saved to the card
 * when something happens.
 *
 * What counts as something: the screen staying one flat colour (a stop screen,
 * a blanked output), a watched phrase appearing, the target's power going off,
 * or somebody asking. The clip holds what came before and a little after; an
 * event during a clip becomes a chapter in it and makes it run longer.
 *
 * Independent of notifications: a clip is saved whether or not anyone is told.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kvm_atx.h"
#include "kvm_notify.h"
#include "kvm_record.h"
#include "kvm_settings.h"
#include "kvm_storage.h"
#include "record_priv.h"
#include "screentext_store.h"
#include "ts_to_mp4.h"

#define TAG "dashcam"
#define POLL_MS 1000
/* The same half minute notifications and Home Assistant wait: a state, not a repaint. */
#define FLAT_US (30 * 1000000LL)

/* dashcam_store: 0 keeps the past in PSRAM, 1 on the card. */
static bool on_card(void)
{
    return kvm_setting_int("dashcam_store") == 1;
}

static void apply_setting(void)
{
    /* Only with H.264 selected: MJPEG needs the ring's PSRAM for its own buffers,
     * so the ring is given back the moment the setting changes, before the
     * capture loop switches. vid_codec is an index; 1 is h264. The card's
     * writer is started and stopped by the poll task: stopping waits for the
     * card, which a settings callback should not. */
    record_set_dashcam(kvm_setting_bool("dashcam") && kvm_setting_int("vid_codec") == 1 && !on_card());
}

static void on_setting(const char *key, void *user)
{
    (void)user;
    if (strcmp(key, "dashcam") == 0 || strcmp(key, "vid_codec") == 0 || strcmp(key, "dashcam_store") == 0 ||
        strcmp(key, "*") == 0) {
        apply_setting();
    }
}

bool kvm_record_event(const char *what, char *why, size_t why_cap)
{
    const int64_t now = esp_timer_get_time();
    if (record_event_running()) {
        record_add_chapter(now, what);
        record_extend_event(now + (int64_t)kvm_setting_int("dashcam_post_s") * 1000000);
        ESP_LOGI(TAG, "\"%s\" - added to the clip being saved", what);
        return true;
    }
    if (kvm_record_active()) {
        snprintf(why, why_cap, "a recording is running");
        return false;
    }
    if (!kvm_setting_bool("dashcam")) {
        snprintf(why, why_cap, "the dashcam is off");
        return false;
    }
    if (on_card() && !record_segments_running()) {
        const char *blocked = kvm_record_blocked();
        snprintf(why, why_cap, "the dashcam is not writing to the card%s%s", blocked ? ": " : "",
                 blocked ? blocked : "");
        return false;
    }
    const esp_err_t err = record_start_event(what, (uint32_t)kvm_setting_int("dashcam_pre_s"),
                                             (uint32_t)kvm_setting_int("dashcam_post_s"), why, why_cap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no clip of \"%s\": %s", what, why);
        return false;
    }
    ESP_LOGI(TAG, "saving a clip: %s", what);
    return true;
}

static void poll_task(void *arg)
{
    (void)arg;
    uint32_t alert_seen = 0;
    (void)screentext_alert_get(NULL, 0, &alert_seen);
    bool flat_seen = false;
    bool power_was_on = false;
    char why[96];
    bool card_wanted = false;
    int64_t retry_at = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        /* The dashcam on the card: keep its writer going whenever it can run -
         * after an operator's recording, a card put back, a codec switch. */
        const int64_t now = esp_timer_get_time();
        const bool want = kvm_setting_bool("dashcam") && on_card() && kvm_setting_int("vid_codec") == 1;
        if (want && !record_segments_running() && !kvm_record_active() && now >= retry_at) {
            if (!kvm_record_blocked() && record_start_segments(why, sizeof(why)) != ESP_OK) {
                ESP_LOGW(TAG, "not writing to the card: %s", why);
                retry_at = now + 30 * 1000000LL;
            }
        }
        if (!want && card_wanted) {
            /* Switched off or to memory: stop, and delete the past it kept. With
             * only the codec changed, keep it for when H.264 is back. */
            const bool forget = !kvm_setting_bool("dashcam") || !on_card();
            record_stop_segments("the dashcam was switched off", forget);
        }
        card_wanted = want;

        if (!kvm_setting_bool("dashcam")) {
            continue;
        }

        uint32_t seq = 0;
        char phrase[SCREENTEXT_ALERT_MAX];
        const bool alerting = screentext_alert_get(phrase, sizeof(phrase), &seq);
        if (seq != alert_seen) {
            alert_seen = seq;
            if (alerting && kvm_setting_bool("dashcam_on_watch")) {
                char title[RECORD_CHAPTER_TITLE];
                snprintf(title, sizeof(title), "On screen: %.36s", phrase);
                (void)kvm_record_event(title, why, sizeof(why));
            }
        }

        kvm_video_status_t v;
        capture_status_get(&v);
        /* A black screen is a monitor going to sleep, not news; a coloured one
         * (a stop screen) is. */
        const bool flat = v.signal && v.flat_ms >= FLAT_US / 1000 && !v.flat_dark;
        if (flat && !flat_seen && kvm_setting_bool("dashcam_on_flat")) {
            (void)kvm_record_event("Screen went one colour", why, sizeof(why));
        }
        flat_seen = flat;

        kvm_atx_status_t atx;
        kvm_atx_status(&atx);
        if (atx.enabled && atx.have_led) {
            if (power_was_on && !atx.power_on && kvm_setting_bool("dashcam_on_power")) {
                (void)kvm_record_event("Target power went off", why, sizeof(why));
            }
            power_was_on = atx.power_on;
        }
    }
}

/* ---- turning a clip into an MP4 --------------------------------------------- */

static QueueHandle_t s_jobs;
static volatile uint32_t s_converting; /* queued or running */
static portMUX_TYPE s_last_mu = portMUX_INITIALIZER_UNLOCKED;
static char s_last_clip[64];

uint32_t dashcam_clips_converting(void)
{
    return s_converting;
}

void dashcam_last_clip(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_last_mu);
    snprintf(out, cap, "%s", s_last_clip);
    portEXIT_CRITICAL(&s_last_mu);
}

#define TIMELAPSE_MP4_MAX (256ull * 1024 * 1024)

/* The input is one .ts, or a clip's segments read one after another. */
typedef struct {
    FILE *in;
    FILE *out;
    const record_finished_t *job;
    uint32_t n; /* the segment open in `in` */
    char *inbuf;
} files_t;

enum { IO_BUF = 32 * 1024 };

static FILE *open_input(const char *path, char *buf)
{
    FILE *f = fopen(path, "rb");
    if (f && buf) {
        setvbuf(f, buf, _IOFBF, IO_BUF);
    }
    return f;
}

static size_t io_read(void *io, uint8_t *buf, size_t cap)
{
    files_t *f = io;
    for (;;) {
        if (f->in && cap) {
            const size_t n = fread(buf, 1, cap, f->in);
            if (n || !f->job->segments) {
                return n;
            }
            fclose(f->in);
            f->in = NULL;
            f->n++;
        }
        /* The next segment; one already deleted is passed over. */
        while (!f->in && f->n <= f->job->seg_last) {
            char path[112];
            record_segment_path(f->n, path, sizeof(path));
            f->in = open_input(path, f->inbuf);
            if (!f->in) {
                f->n++;
            }
        }
        if (!f->in || !cap) {
            return 0;
        }
    }
}

static bool io_rewind(void *io)
{
    files_t *f = io;
    if (!f->job->segments) {
        return fseek(f->in, 0, SEEK_SET) == 0;
    }
    if (f->in) {
        fclose(f->in);
        f->in = NULL;
    }
    f->n = f->job->seg_first;
    return true;
}

/* An MP4 could not be made from segments: keep them joined as a .ts instead. */
static void join_segments(const record_finished_t *job, const char *ts_path, char *buf)
{
    FILE *out = fopen(ts_path, "wb");
    if (!out) {
        return;
    }
    files_t f = {.job = job, .n = job->seg_first, .inbuf = NULL};
    size_t n;
    while ((n = io_read(&f, (uint8_t *)buf, IO_BUF)) > 0 && fwrite(buf, 1, n, out) == n) {
    }
    if (f.in) {
        fclose(f.in);
    }
    fflush(out);
    fsync(fileno(out));
    fclose(out);
}

static bool io_write(void *io, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, ((files_t *)io)->out) == len;
}

/* False when it ran out of memory and may work in a moment. */
static bool convert(const record_finished_t *job, bool last_try)
{
    char ts[112], mp4[112], tmp[112], card_mp4[80];
    snprintf(ts, sizeof(ts), "%s/%s", kvm_storage_mount_point(), job->file);
    snprintf(card_mp4, sizeof(card_mp4), "%.*s.mp4", (int)(strlen(job->file) - 3), job->file);
    snprintf(mp4, sizeof(mp4), "%s/%s", kvm_storage_mount_point(), card_mp4);
    /* Written under another name and renamed when whole, so a reboot mid-way
     * leaves no empty .mp4 next to the .ts. */
    snprintf(tmp, sizeof(tmp), "%.*s.tmp", (int)(strlen(mp4) - 4), mp4);

    /* Buffers from PSRAM, for the length of one conversion. As static arrays they
     * sat in internal RAM for good - 128 KB of the ~500 KB there - and TLS then
     * could not set up a session: no web console, no Telegram. */
    char *inbuf = heap_caps_malloc(IO_BUF, MALLOC_CAP_SPIRAM);
    char *outbuf = heap_caps_malloc(IO_BUF, MALLOC_CAP_SPIRAM);
    files_t f = {.out = fopen(tmp, "wb"), .job = job, .n = job->seg_first, .inbuf = inbuf};
    if (job->segments) {
        (void)io_read(&f, NULL, 0); /* opens the first segment there is */
    } else {
        f.in = open_input(ts, inbuf);
    }
    if (f.out && outbuf) {
        setvbuf(f.out, outbuf, _IOFBF, IO_BUF);
    }
    mp4_chapter_t chapters[RECORD_CHAPTERS_MAX];
    for (int i = 0; i < job->chapters; i++) {
        chapters[i].at_us = job->chapter[i].at_us - job->t0_us;
        chapters[i].title = job->chapter[i].title;
    }
    char err[96] = "could not open the files";
    const int64_t t0 = esp_timer_get_time();
    const bool ok = f.in && f.out &&
                    ts_to_mp4(&(mp4_io_t){io_read, io_rewind, io_write, &f}, job->width,
                              job->height, chapters, job->chapters, err, sizeof(err));
    if (f.in) {
        fclose(f.in);
    }
    if (f.out) {
        fflush(f.out);
        fsync(fileno(f.out));
        fclose(f.out);
    }
    heap_caps_free(outbuf);
    if (ok && rename(tmp, mp4) != 0) {
        snprintf(err, sizeof(err), "could not rename the finished file");
    }
    if (!ok || access(mp4, F_OK) != 0) {
        remove(tmp);
        const bool oom = !inbuf || !outbuf || strstr(err, "out of memory") != NULL;
        if (oom && !last_try) {
            ESP_LOGW(TAG, "%s: %s (PSRAM free %u KB, largest %u KB); trying again soon", job->file, err,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
            heap_caps_free(inbuf);
            return false;
        }
        if (job->segments && inbuf) {
            join_segments(job, ts, inbuf);
        }
        ESP_LOGW(TAG, "%s stays a .ts: %s", job->file, err);
        heap_caps_free(inbuf);
        if (job->event) {
            /* Still a clip the operator should hear about, just not an MP4. */
            portENTER_CRITICAL(&s_last_mu);
            snprintf(s_last_clip, sizeof(s_last_clip), "%.63s", job->file);
            portEXIT_CRITICAL(&s_last_mu);
        }
        return true;
    }
    heap_caps_free(inbuf);
    /* The MP4 has everything the .ts had. */
    remove(ts);
    ESP_LOGI(TAG, "%s ready: %s in %lld ms", job->timelapse ? "timelapse" : "clip", card_mp4,
             (long long)((esp_timer_get_time() - t0) / 1000));
    if (job->timelapse) {
        return true; /* nothing happened to tell anyone about */
    }
    portENTER_CRITICAL(&s_last_mu);
    snprintf(s_last_clip, sizeof(s_last_clip), "%.63s", card_mp4);
    portEXIT_CRITICAL(&s_last_mu);

    /* What set it off, for the message: the first chapter after "Before". */
    const char *what = "Dashcam clip";
    for (int i = 0; i < job->chapters; i++) {
        if (strcmp(job->chapter[i].title, "Before") != 0) {
            what = job->chapter[i].title;
            break;
        }
    }
    char caption[160];
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) {
        char when[24];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        snprintf(caption, sizeof(caption), "Dashcam: %s (%s)", what, when);
    } else {
        snprintf(caption, sizeof(caption), "Dashcam: %s", what);
    }
    if (kvm_setting_bool("notify_enable")) {
        kvm_notify_send_clip(mp4, card_mp4, caption);
    }
    return true;
}

static void convert_task(void *arg)
{
    (void)arg;
    static record_finished_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Never alongside a recording: it would share the card with it. And
         * after its ring is given back - a pre-3.0 board has no PSRAM for both. */
        while (kvm_record_active() || !record_ring_idle()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!kvm_storage_writable()) {
            ESP_LOGW(TAG, "%s stays a .ts: the card is not writable now", job.file);
        } else {
            /* Out of memory is often a moment's: a download, a screenshot. */
            for (int attempt = 0; attempt < 4 && !convert(&job, attempt == 3); attempt++) {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
        if (job.segments) {
            record_unpin_segments(job.seg_first);
        }
        if (job.event && s_converting) {
            s_converting--;
        }
    }
}

static void on_finished(const record_finished_t *done)
{
    /* A timelapse becomes an MP4 too, so it plays in the browser - if it is one
     * file and small enough to convert in a few minutes (about 1 MB a second). */
    const bool timelapse = done->timelapse && done->parts == 1 && done->bytes <= TIMELAPSE_MP4_MAX;
    if (!done->event && !timelapse) {
        return;
    }
    ESP_LOGI(TAG, "%s saved: %s (%d chapters)", timelapse ? "timelapse" : "clip", done->file,
             done->chapters);
    s_converting += done->event ? 1 : 0;
    if (!s_jobs || xQueueSend(s_jobs, done, 0) != pdTRUE) {
        s_converting -= done->event ? 1 : 0;
        ESP_LOGW(TAG, "%s stays a .ts: too many clips waiting", done->file);
        if (done->segments) {
            record_unpin_segments(done->seg_first);
        }
    }
}

void kvm_record_dashcam_init(void)
{
    /* PSRAM: a job is a kilobyte, and internal RAM is what TLS needs. */
    s_jobs = xQueueCreateWithCaps(4, sizeof(record_finished_t), MALLOC_CAP_SPIRAM);
    if (!s_jobs || xTaskCreate(convert_task, "clip_mp4", 4608, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the clip converter");
    }
    record_set_finished_cb(on_finished);
    (void)kvm_settings_subscribe(on_setting, NULL);
    apply_setting();
    if (xTaskCreate(poll_task, "dashcam", 3072, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start");
    }
}
