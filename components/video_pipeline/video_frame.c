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
    /** Created once: the store is usable when this is non-NULL. */
    SemaphoreHandle_t ready;
    uint8_t *buf[VIDEO_SLOT_COUNT];
    size_t len[VIDEO_SLOT_COUNT];
    /** Readers currently sending from this slot; the encoder must not reuse it. */
    uint8_t ref[VIDEO_SLOT_COUNT];
    bool key[VIDEO_SLOT_COUNT];
    /** When the frame was published: the time a recording gives it. */
    int64_t at_us[VIDEO_SLOT_COUNT];
    int64_t src_us[VIDEO_SLOT_COUNT];
    int slots;
    int front;
    size_t cap;
    video_payload_t payload;
    volatile uint32_t seq;
    volatile bool keyframe_wanted;
} s;

/*
 * Waking the senders. Each waiter takes a slot with its own binary semaphore,
 * and a publish gives every occupied slot's. The store used to give one token
 * per viewer into a shared counting semaphore, and a waiter that woke drained
 * all of them to skip frames it had fallen behind on - taking the other
 * viewers' tokens with its own. With two viewers, whichever woke first (the
 * recorder, which runs at a higher priority) got every frame and the other got
 * one at each 500 ms timeout: a live view at 2 fps while a recording ran.
 */
#define WAITER_SLOTS 16
static struct {
    SemaphoreHandle_t sem;
    bool used;
} s_waiters[WAITER_SLOTS];
static portMUX_TYPE s_waiters_mu = portMUX_INITIALIZER_UNLOCKED;

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
    bool slots_ok = true;
    for (int i = 0; i < WAITER_SLOTS; i++) {
        s_waiters[i].sem = xSemaphoreCreateBinary();
        slots_ok = slots_ok && s_waiters[i].sem;
    }
    /* Only a flag now: set once every slot exists. */
    s.ready = slots_ok ? s.mutex : NULL;
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
            for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
                if (s.ref[i]) {
                    busy = true;
                }
            }
            /* Only invalidate once no reader still holds a slot. Zeroing front /
             * payload on every poll corrupted the store on a timed-out quiesce: a
             * reader holding a slot past the deadline made this return false, the
             * caller kept the old codec running, and its next publish restored
             * `front` but not `payload` - so frames went out tagged NONE and
             * clients mis-decoded until the next successful switch. */
            if (!busy) {
                s.front = -1;
                s.payload = VIDEO_PAYLOAD_NONE;
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
        /* At least one full tick: pdMS_TO_TICKS(1) is 0 ticks at 100 Hz, and
         * vTaskDelay(0) never yields to a lower-priority task - a busy loop. */
        vTaskDelay(1);
    }
}

static int64_t s_next_src_us;

void video_frame_set_source_time(int64_t landed_us)
{
    s_next_src_us = landed_us;
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
    s.at_us[slot] = esp_timer_get_time();
    s.src_us[slot] = s_next_src_us;
    s.front = slot;
    s.seq++;
    xSemaphoreGive(s.mutex);

    if (!s.ready) {
        return;
    }
    for (int i = 0; i < WAITER_SLOTS; i++) {
        portENTER_CRITICAL(&s_waiters_mu);
        const bool used = s_waiters[i].used;
        portEXIT_CRITICAL(&s_waiters_mu);
        if (used) {
            (void)xSemaphoreGive(s_waiters[i].sem); /* already given is fine */
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
        out->at_us = s.at_us[f];
        out->src_us = s.src_us[f];
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
    int slot = -1;
    portENTER_CRITICAL(&s_waiters_mu);
    for (int i = 0; i < WAITER_SLOTS; i++) {
        if (!s_waiters[i].used) {
            s_waiters[i].used = true;
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_waiters_mu);
    if (slot < 0) {
        /* More waiters than slots: poll, a tick at a time. */
        const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while (s.seq == seen && xTaskGetTickCount() < until) {
            vTaskDelay(1);
        }
        return s.seq != seen;
    }
    /* A give left from whoever had the slot before means nothing now. The frame
     * may also have landed between the check above and taking the slot. */
    (void)xSemaphoreTake(s_waiters[slot].sem, 0);
    if (s.seq == seen) {
        /* Waking once is enough: the caller takes the newest frame, so one that
         * fell behind sends the latest rather than every frame it missed. */
        (void)xSemaphoreTake(s_waiters[slot].sem, pdMS_TO_TICKS(timeout_ms));
    }
    portENTER_CRITICAL(&s_waiters_mu);
    s_waiters[slot].used = false;
    portEXIT_CRITICAL(&s_waiters_mu);
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

static int s_uploads;

void video_frame_upload_begin(void)
{
    taskENTER_CRITICAL(&s_viewer_mu);
    s_uploads++;
    taskEXIT_CRITICAL(&s_viewer_mu);
}

void video_frame_upload_end(void)
{
    taskENTER_CRITICAL(&s_viewer_mu);
    if (s_uploads > 0) {
        s_uploads--;
    }
    taskEXIT_CRITICAL(&s_viewer_mu);
}

bool video_frame_upload_active(void)
{
    taskENTER_CRITICAL(&s_viewer_mu);
    const bool active = s_uploads > 0;
    taskEXIT_CRITICAL(&s_viewer_mu);
    return active;
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
