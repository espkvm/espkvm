/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "video_frame.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "video"

static struct {
    SemaphoreHandle_t mutex;
    /** Given once per viewer per publish, so a waiting sender wakes up. */
    SemaphoreHandle_t ready;
    uint8_t *buf[VIDEO_SLOT_COUNT];
    size_t len[VIDEO_SLOT_COUNT];
    /** Readers currently sending from this slot; the encoder must not reuse it. */
    uint8_t ref[VIDEO_SLOT_COUNT];
    bool key[VIDEO_SLOT_COUNT];
    int slots;
    int front;
    size_t cap;
    video_payload_t payload;
    volatile uint32_t seq;
    volatile bool keyframe_wanted;
} s;

/* Viewer counting is done under a spinlock rather than the store mutex: it is
 * read from HTTP handlers on every status request and must never wait behind
 * an encode. */
static portMUX_TYPE s_viewer_mu = portMUX_INITIALIZER_UNLOCKED;
static int s_viewers;

void video_frame_store_init(void)
{
    if (s.mutex) {
        return;
    }
    s.front = -1;
    s.payload = VIDEO_PAYLOAD_NONE;
    s.mutex = xSemaphoreCreateMutex();
    /* Counting, because several senders may be waiting for the same frame. */
    s.ready = xSemaphoreCreateCounting(128, 0);
    if (!s.mutex || !s.ready) {
        ESP_LOGE(TAG, "frame store init failed");
    }
}

bool video_frame_store_ready(void)
{
    return s.mutex && s.ready && s.payload != VIDEO_PAYLOAD_NONE;
}

void video_frame_install(video_payload_t payload, uint8_t *const *bufs, int count, size_t cap)
{
    if (!s.mutex || count <= 0 || count > VIDEO_SLOT_COUNT) {
        return;
    }
    if (xSemaphoreTake(s.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
        s.buf[i] = i < count ? bufs[i] : NULL;
        s.len[i] = 0;
        s.ref[i] = 0;
        s.key[i] = false;
    }
    s.slots = count;
    s.cap = cap;
    s.payload = payload;
    s.front = -1;
    /* Readers track the sequence number; moving it on tells them the frame
     * they were about to send is gone rather than merely unchanged. */
    s.seq++;
    xSemaphoreGive(s.mutex);
}

bool video_frame_quiesce(uint32_t timeout_ms)
{
    if (!s.mutex) {
        return true;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        bool busy = false;
        if (xSemaphoreTake(s.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            s.front = -1;
            s.payload = VIDEO_PAYLOAD_NONE;
            for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
                if (s.ref[i]) {
                    busy = true;
                }
            }
            if (!busy) {
                for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
                    s.buf[i] = NULL;
                    s.len[i] = 0;
                }
                s.slots = 0;
                s.cap = 0;
            }
            xSemaphoreGive(s.mutex);
        } else {
            busy = true;
        }
        if (!busy) {
            return true;
        }
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "frame store still in use, cannot release buffers");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t video_frame_begin_write(int *out_slot, uint8_t **out_buf, size_t *out_cap,
                                  uint32_t timeout_ms)
{
    if (!s.mutex || !out_slot || !out_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int found = -1;
        if (xSemaphoreTake(s.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (s.slots == 0) {
                xSemaphoreGive(s.mutex);
                return ESP_ERR_INVALID_STATE;
            }
            for (int i = 0; i < s.slots; i++) {
                if (!s.buf[i] || s.ref[i] || i == s.front) {
                    continue;
                }
                found = i;
                break;
            }
            if (found >= 0) {
                *out_slot = found;
                *out_buf = s.buf[found];
                if (out_cap) {
                    *out_cap = s.cap;
                }
            }
            xSemaphoreGive(s.mutex);
        }
        if (found >= 0) {
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void video_frame_publish(int slot, size_t len, bool keyframe)
{
    if (!s.mutex || slot < 0 || slot >= VIDEO_SLOT_COUNT) {
        return;
    }
    if (xSemaphoreTake(s.mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s.len[slot] = len;
    s.key[slot] = keyframe;
    s.front = slot;
    s.seq++;
    xSemaphoreGive(s.mutex);

    int n = video_frame_viewer_count();
    if (!s.ready || n <= 0) {
        return;
    }
    const int cap = 16;
    if (n > cap) {
        n = cap;
    }
    for (int i = 0; i < n; i++) {
        if (xSemaphoreGive(s.ready) != pdTRUE) {
            break;
        }
    }
}

bool video_frame_front_matches(const uint8_t *data, size_t len)
{
    if (!s.mutex || !data) {
        return false;
    }
    bool same = false;
    if (xSemaphoreTake(s.mutex, portMAX_DELAY) == pdTRUE) {
        const int f = s.front;
        if (f >= 0 && s.buf[f] && s.len[f] == len && len > 0) {
            same = memcmp(s.buf[f], data, len) == 0;
        }
        xSemaphoreGive(s.mutex);
    }
    return same;
}

bool video_frame_acquire(video_frame_ref_t *out)
{
    if (!s.mutex || !out) {
        return false;
    }
    bool got = false;
    if (xSemaphoreTake(s.mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    const int f = s.front;
    if (f >= 0 && s.buf[f] && s.len[f] > 0 && s.len[f] <= s.cap) {
        s.ref[f]++;
        out->slot = f;
        out->data = s.buf[f];
        out->len = s.len[f];
        out->seq = s.seq;
        out->payload = s.payload;
        out->keyframe = s.key[f];
        got = true;
    }
    xSemaphoreGive(s.mutex);
    return got;
}

void video_frame_release(const video_frame_ref_t *ref)
{
    if (!s.mutex || !ref || ref->slot < 0 || ref->slot >= VIDEO_SLOT_COUNT) {
        return;
    }
    if (xSemaphoreTake(s.mutex, portMAX_DELAY) == pdTRUE) {
        if (s.ref[ref->slot] > 0) {
            s.ref[ref->slot]--;
        }
        xSemaphoreGive(s.mutex);
    }
}

uint32_t video_frame_seq(void)
{
    return s.seq;
}

video_payload_t video_frame_payload(void)
{
    return s.payload;
}

bool video_frame_wait_new(uint32_t seen, uint32_t timeout_ms)
{
    if (s.seq != seen) {
        return true;
    }
    if (!s.ready) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms < 5 ? 5 : timeout_ms));
        return s.seq != seen;
    }
    if (xSemaphoreTake(s.ready, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        while (xSemaphoreTake(s.ready, 0) == pdTRUE) {
            /* Coalesce: a sender that fell behind sends the newest frame, not
             * every frame it missed. */
        }
    }
    return s.seq != seen;
}

void video_frame_viewer_enter(void)
{
    taskENTER_CRITICAL(&s_viewer_mu);
    s_viewers++;
    taskEXIT_CRITICAL(&s_viewer_mu);
    /* Whoever just arrived has nothing to decode from. */
    video_frame_request_keyframe();
}

void video_frame_viewer_leave(void)
{
    taskENTER_CRITICAL(&s_viewer_mu);
    if (s_viewers > 0) {
        s_viewers--;
    }
    taskEXIT_CRITICAL(&s_viewer_mu);
}

int video_frame_viewer_count(void)
{
    int n;
    taskENTER_CRITICAL(&s_viewer_mu);
    n = s_viewers;
    taskEXIT_CRITICAL(&s_viewer_mu);
    return n;
}

void video_frame_request_keyframe(void)
{
    s.keyframe_wanted = true;
}

bool video_frame_take_keyframe_request(void)
{
    if (!s.keyframe_wanted) {
        return false;
    }
    s.keyframe_wanted = false;
    return true;
}
