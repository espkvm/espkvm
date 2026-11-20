/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_media.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_cache.h"

static const char *TAG = "kvm_media";

/* The USB stack has one command at a time; one more slot for a write that
 * follows a read before its callback ran. */
#define MEDIA_QUEUE_LEN 2
#define MEDIA_TASK_STACK 4096
/* As the USB task: a target's read waited there before. */
#define MEDIA_TASK_PRIO 5
/* How long without a read before a source hears it is idle. */
#define MEDIA_IDLE_MS 20000

typedef struct {
    bool write;
    uint64_t off;
    void *buf;
    uint32_t len;
    kvm_media_done_t done;
    void *arg;
} media_req_t;

/*
 * The inserted source. The media task holds the lock for the whole of a read,
 * so a source is never closed under a read of it: kvm_media_eject_ops() waits.
 */
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static volatile TickType_t s_last_io; /* when the last request came, 0 before any */
static kvm_media_source_t s_src;
static bool s_present;

/*
 * What is inserted, for those who must not wait for a read: the USB task asks
 * it for every TEST UNIT READY, and an eject needs the ops to cancel a read.
 * Written under s_lock and s_snap_mux, read under s_snap_mux alone.
 */
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static kvm_media_t s_snap;
static const kvm_media_ops_t *s_snap_ops;

static media_chunk_t s_chunks[1];
static media_cache_t s_cache = {.chunks = s_chunks, .count = 1};
static uint32_t s_chunk_cap; /* the size the chunk buffers were made for */

static uint32_t block_size(bool cdrom)
{
    return cdrom ? 2048u : 512u;
}

/* Under s_lock, after s_src or s_present changed. */
static void snap_locked(void)
{
    kvm_media_t m = {.block_size = block_size(false)};
    if (s_present) {
        m.present = true;
        m.writable = s_src.ops->write != NULL;
        m.cdrom = s_src.cdrom;
        m.block_size = block_size(s_src.cdrom);
        m.block_count = s_src.size / m.block_size;
        memcpy(m.name, s_src.name, sizeof(m.name));
    }
    taskENTER_CRITICAL(&s_snap_mux);
    s_snap = m;
    s_snap_ops = s_present ? s_src.ops : NULL;
    taskEXIT_CRITICAL(&s_snap_mux);
}

/* Before taking s_lock to take the medium out: a read waiting on the network
 * holds s_lock, and is told to give up instead of being waited for. Only when
 * @p only is NULL or the inserted ops. */
static void cancel_read(const kvm_media_ops_t *only)
{
    taskENTER_CRITICAL(&s_snap_mux);
    const kvm_media_ops_t *ops = s_snap_ops;
    taskEXIT_CRITICAL(&s_snap_mux);
    if (ops && ops->cancel && (!only || only == ops)) {
        ops->cancel();
    }
}

static void close_locked(void)
{
    if (s_present && s_src.ops->close) {
        s_src.ops->close(s_src.ctx);
    }
    s_present = false;
    memset(&s_src, 0, sizeof(s_src));
    media_cache_reset(&s_cache, 0, 0);
    snap_locked();
}

/* Buffers made for a smaller read-ahead are let go; a bigger one is made on use. */
static void cache_for(uint64_t size, uint32_t read_ahead)
{
    if (read_ahead > s_chunk_cap) {
        for (unsigned i = 0; i < s_cache.count; i++) {
            heap_caps_free(s_chunks[i].buf);
            s_chunks[i].buf = NULL;
        }
        s_chunk_cap = read_ahead;
    }
    media_cache_reset(&s_cache, size, read_ahead);
}

static int32_t do_read(uint64_t off, uint8_t *buf, uint32_t len)
{
    /* A read of the last block can run past the end: the tail is zeros. */
    memset(buf, 0, len);
    if (!s_present) {
        return KVM_MEDIA_ERR_NO_MEDIUM;
    }
    if (off >= s_src.size) {
        return (int32_t)len;
    }
    const uint64_t left = s_src.size - off;
    const uint32_t n = left < len ? (uint32_t)left : len;
    if (media_cache_get(&s_cache, off, buf, n)) {
        return (int32_t)len;
    }
    uint64_t fill_off;
    uint32_t fill_len;
    media_chunk_t *k = media_cache_plan(&s_cache, off, n, &fill_off, &fill_len);
    if (k && !k->buf) {
        k->buf = heap_caps_aligned_alloc(64, s_chunk_cap, MALLOC_CAP_SPIRAM);
    }
    esp_err_t err = ESP_FAIL;
    if (k && k->buf) {
        err = s_src.ops->read(s_src.ctx, fill_off, k->buf, fill_len);
        if (err == ESP_OK) {
            media_cache_filled(&s_cache, k, fill_off, fill_len);
            memcpy(buf, k->buf, n);
            return (int32_t)len;
        }
        k->len = 0; /* try the read itself */
    }
    /* A chunk that failed is tried once more at the read's own size, as a
     * card may fail one big transfer. Not after a source said it is gone or
     * has tried for long enough already. */
    if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_TIMEOUT) {
        err = s_src.ops->read(s_src.ctx, off, buf, n);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "ejected: the %s went away", s_src.ops->kind);
        close_locked();
        return KVM_MEDIA_ERR_NO_MEDIUM;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read from %s failed at %llu", s_src.ops->kind, (unsigned long long)off);
        return KVM_MEDIA_ERR_IO;
    }
    return (int32_t)len;
}

static int32_t do_write(uint64_t off, const uint8_t *buf, uint32_t len)
{
    if (!s_present) {
        return KVM_MEDIA_ERR_NO_MEDIUM;
    }
    if (!s_src.ops->write || off + len > s_src.size) {
        return KVM_MEDIA_ERR_IO;
    }
    media_cache_drop(&s_cache); /* it may hold these bytes */
    if (s_src.ops->write(s_src.ctx, off, buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "write to %s failed at %llu", s_src.ops->kind, (unsigned long long)off);
        return KVM_MEDIA_ERR_IO;
    }
    return (int32_t)len;
}

static void media_task(void *arg)
{
    (void)arg;
    media_req_t r;
    bool busy = false; /* read since the source last heard it is idle */
    for (;;) {
        if (xQueueReceive(s_queue, &r, busy ? pdMS_TO_TICKS(MEDIA_IDLE_MS) : portMAX_DELAY) != pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_present && s_src.ops->idle) {
                s_src.ops->idle(s_src.ctx);
            }
            xSemaphoreGive(s_lock);
            busy = false;
            continue;
        }
        busy = true;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int32_t result = r.write ? do_write(r.off, r.buf, r.len) : do_read(r.off, r.buf, r.len);
        xSemaphoreGive(s_lock);
        r.done(result, r.arg);
    }
}

esp_err_t kvm_media_init(void)
{
    if (s_queue) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    QueueHandle_t q = xQueueCreate(MEDIA_QUEUE_LEN, sizeof(media_req_t));
    if (!s_lock || !q) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = q; /* before the task, which waits on it at once */
    if (xTaskCreate(media_task, "media", MEDIA_TASK_STACK, NULL, MEDIA_TASK_PRIO, NULL) != pdPASS) {
        s_queue = NULL;
        vQueueDelete(q);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t kvm_media_insert(const kvm_media_source_t *src)
{
    if (!s_lock) {
        if (src->ops->close) {
            src->ops->close(src->ctx);
        }
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t bsize = block_size(src->cdrom);
    cancel_read(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    close_locked();
    if (src->size < bsize) {
        xSemaphoreGive(s_lock);
        if (src->ops->close) {
            src->ops->close(src->ctx);
        }
        return ESP_ERR_INVALID_SIZE;
    }
    s_src = *src;
    s_present = true;
    cache_for(src->size, src->ops->read_ahead);
    snap_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "inserted: '%s' (%s) as %s, %llu blocks (%llu MB)", src->name, src->ops->kind,
             src->cdrom ? "CD-ROM" : "disk", (unsigned long long)(src->size / bsize),
             (unsigned long long)(src->size / (1024 * 1024)));
    return ESP_OK;
}

void kvm_media_eject(void)
{
    if (!s_lock) {
        return;
    }
    cancel_read(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was = s_present;
    close_locked();
    xSemaphoreGive(s_lock);
    if (was) {
        ESP_LOGI(TAG, "ejected");
    }
}

void kvm_media_eject_ops(const kvm_media_ops_t *ops)
{
    if (!s_lock) {
        return;
    }
    cancel_read(ops);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool match = s_present && s_src.ops == ops;
    if (match) {
        close_locked();
    }
    xSemaphoreGive(s_lock);
    if (match) {
        ESP_LOGI(TAG, "ejected: the %s went away", ops->kind);
    }
}

bool kvm_media_is(const kvm_media_ops_t *ops)
{
    taskENTER_CRITICAL(&s_snap_mux);
    const bool is = s_snap_ops == ops;
    taskEXIT_CRITICAL(&s_snap_mux);
    return is;
}

void kvm_media_info(kvm_media_t *out)
{
    taskENTER_CRITICAL(&s_snap_mux);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_snap_mux);
    if (!out->block_size) {
        out->block_size = block_size(false); /* before the first insert */
    }
}

static esp_err_t queue(bool write, uint64_t off, void *buf, uint32_t len, kvm_media_done_t done,
                       void *arg)
{
    if (!s_queue || !buf || !done) {
        return ESP_ERR_INVALID_STATE;
    }
    const media_req_t r = {.write = write, .off = off, .buf = buf, .len = len, .done = done, .arg = arg};
    s_last_io = xTaskGetTickCount() | 1; /* never 0 once there was one */
    return xQueueSend(s_queue, &r, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

bool kvm_media_busy(uint32_t ms)
{
    const TickType_t t = s_last_io;
    return t && xTaskGetTickCount() - t < pdMS_TO_TICKS(ms);
}

esp_err_t kvm_media_read_async(uint64_t off, void *buf, uint32_t len, kvm_media_done_t done, void *arg)
{
    return queue(false, off, buf, len, done, arg);
}

esp_err_t kvm_media_write_async(uint64_t off, const void *buf, uint32_t len, kvm_media_done_t done,
                                void *arg)
{
    return queue(true, off, (void *)buf, len, done, arg);
}
