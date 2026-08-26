/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Where the last reading of the screen lives, between the capture task that
 * produces it and the web server that hands it out.
 *
 * Two buffers, both in PSRAM: one the capture task scans into without holding
 * anything, one the readers see. Publishing is a copy under a mutex - a few
 * microseconds - so a scan never blocks a request and a request never delays a
 * frame. They are allocated on first use and kept: the pair is under 10 KB, and
 * a feature that frees and reallocates in the capture path is a feature that
 * fails after an hour of uptime, which is a lesson this project already paid for.
 */
#include "screentext.h"
#include "screentext_store.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "screentext";

static SemaphoreHandle_t s_lock;
static screentext_grid_t *s_scratch;
static screentext_grid_t *s_latest;
static int64_t s_latest_us;
/* Bumped whenever what a reader would see changes. One 32-bit word, written
   under the lock and read without it: a reader that catches an old value simply
   sends the reading one moment later. */
static volatile uint32_t s_seq;

void screentext_store_init(void)
{
    if (s_lock) {
        return; /* already set up */
    }
    s_lock = xSemaphoreCreateMutex();
    s_scratch = heap_caps_calloc(1, sizeof(*s_scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_latest = heap_caps_calloc(1, sizeof(*s_latest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lock || !s_scratch || !s_latest) {
        ESP_LOGW(TAG, "no memory for the screen text buffers (%u bytes each); "
                      "the screen will not be read",
                 (unsigned)sizeof(*s_scratch));
    }
}

/*
 * How long an ask stands. The console polls the reading every two seconds while
 * its text layer is up, so anything comfortably over that keeps the request
 * alive without the console having to know it exists - and lets it lapse a few
 * seconds after the operator moves on.
 */
#define REQUEST_HOLD_US (6 * 1000 * 1000)

/* Written by whichever task serves the request, read by the capture task. One
   64-bit timestamp, and neither side needs the other to be exact - a request
   that lands a frame late simply reads the next frame. */
static volatile int64_t s_requested_us;

/* Subscribers being sent every reading. One writer (the web server, under its
   own lock), read by the capture task. */
static volatile int s_streaming;

void screentext_request(void)
{
    s_requested_us = esp_timer_get_time();
}

bool screentext_requested(void)
{
    /* A subscriber is somebody sitting in front of the screen right now, which
       is a stronger version of the same statement - so it never lapses. */
    if (s_streaming > 0) {
        return true;
    }
    const int64_t asked = s_requested_us;
    return asked != 0 && esp_timer_get_time() - asked < REQUEST_HOLD_US;
}

void screentext_stream_enter(void)
{
    s_streaming++;
    screentext_request();
}

void screentext_stream_leave(void)
{
    if (s_streaming > 0) {
        s_streaming--;
    }
}

bool screentext_streaming(void)
{
    return s_streaming > 0;
}

/** Everything below is a no-op until init has succeeded. */
static bool ready(void)
{
    return s_lock && s_scratch && s_latest;
}

screentext_grid_t *screentext_scratch(void)
{
    return ready() ? s_scratch : NULL;
}

void screentext_publish(const screentext_grid_t *grid)
{
    if (!ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (grid) {
        memcpy(s_latest, grid, sizeof(*s_latest));
        s_latest_us = esp_timer_get_time();
        s_seq++;
    } else if (s_latest->rows != 0 || s_latest->cols != 0) {
        /* Only on the way from text to no text: the scanner says "not text" for
           every settled picture on a desktop, and a sequence that moved for each
           of those would have a subscriber re-reading nothing all day. */
        s_latest->rows = 0;
        s_latest->cols = 0;
        s_latest_us = 0;
        s_seq++;
    }
    xSemaphoreGive(s_lock);
}

uint32_t screentext_seq(void)
{
    return s_seq;
}

static char s_alert[SCREENTEXT_ALERT_MAX];
static uint32_t s_alert_seq;

void screentext_alert_set(const char *phrase)
{
    if (!ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool had = s_alert[0] != '\0';
    if (phrase && *phrase) {
        if (!had || strncmp(s_alert, phrase, sizeof(s_alert)) != 0) {
            snprintf(s_alert, sizeof(s_alert), "%s", phrase);
            s_alert_seq++;
            ESP_LOGW(TAG, "screen alert: %s", s_alert);
        }
    } else if (had) {
        s_alert[0] = '\0';
        s_alert_seq++;
        ESP_LOGI(TAG, "screen alert cleared");
    }
    xSemaphoreGive(s_lock);
}

bool screentext_alert_get(char *buf, size_t cap, uint32_t *seq)
{
    if (!ready()) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (buf && cap) {
        snprintf(buf, cap, "%s", s_alert);
    }
    if (seq) {
        *seq = s_alert_seq;
    }
    const bool raised = s_alert[0] != '\0';
    xSemaphoreGive(s_lock);
    return raised;
}

bool screentext_latest(screentext_grid_t *out, uint32_t *age_ms)
{
    if (!out || !ready()) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = s_latest->rows != 0 && s_latest->cols != 0;
    if (have) {
        memcpy(out, s_latest, sizeof(*out));
        if (age_ms) {
            *age_ms = (uint32_t)((esp_timer_get_time() - s_latest_us) / 1000);
        }
    }
    xSemaphoreGive(s_lock);
    return have;
}
