/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "card_writer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "card_wr";

#define CHUNK (256 * 1024)
#define CHUNK_MIN (32 * 1024)
#define BUFS 4
/* In PSRAM like the tasks that feed it: a default stack could land in the
 * chip's slow LP RAM. */
#define STACK (4 * 1024)
#define PRIO (tskIDLE_PRIORITY + 5)

typedef struct {
    char *data;
    size_t len;
} buf_t;

struct card_writer {
    int fd;
    size_t chunk;
    QueueHandle_t full;  /* to the writer; a NULL data pointer ends it */
    QueueHandle_t empty; /* back from the writer */
    SemaphoreHandle_t done;
    volatile bool failed;
    int64_t write_us;
    bool running;
};

static void writer_task(void *arg)
{
    card_writer_t *w = arg;
    buf_t b;
    while (xQueueReceive(w->full, &b, portMAX_DELAY) == pdTRUE && b.data) {
        if (!w->failed) {
            const int64_t t0 = esp_timer_get_time();
            if (write(w->fd, b.data, b.len) != (ssize_t)b.len) {
                ESP_LOGE(TAG, "write failed: %s", strerror(errno));
                w->failed = true;
            }
            w->write_us += esp_timer_get_time() - t0;
        }
        xQueueSend(w->empty, &b, portMAX_DELAY);
    }
    xSemaphoreGive(w->done);
    vTaskDeleteWithCaps(NULL);
}

static void free_all(card_writer_t *w)
{
    buf_t b;
    while (w->empty && xQueueReceive(w->empty, &b, 0) == pdTRUE) {
        free(b.data);
    }
    if (w->full) {
        vQueueDelete(w->full);
    }
    if (w->empty) {
        vQueueDelete(w->empty);
    }
    if (w->done) {
        vSemaphoreDelete(w->done);
    }
    free(w);
}

card_writer_t *card_writer_start(int fd, const char *tag)
{
    card_writer_t *w = calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->fd = fd;
    w->full = xQueueCreate(BUFS + 1, sizeof(buf_t));
    w->empty = xQueueCreate(BUFS, sizeof(buf_t));
    w->done = xSemaphoreCreateBinary();
    if (!w->full || !w->empty || !w->done) {
        free_all(w);
        return NULL;
    }
    /* PSRAM is mostly taken by video, and what is left can be in pieces: take
     * smaller buffers, or fewer, rather than refuse. */
    size_t chunk = CHUNK;
    int bufs = 0;
    while (bufs < BUFS) {
        buf_t b = {.data = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_SPIRAM)};
        if (b.data) {
            xQueueSend(w->empty, &b, 0);
            bufs++;
        } else if (bufs < 2 && chunk > CHUNK_MIN) {
            chunk /= 2;
            while (xQueueReceive(w->empty, &b, 0) == pdTRUE) { /* all one size */
                free(b.data);
            }
            bufs = 0;
        } else {
            break;
        }
    }
    if (!bufs) {
        ESP_LOGE(TAG, "%s: no buffers (PSRAM free %u KB, largest %u KB)", tag,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        free_all(w);
        return NULL;
    }
    if (chunk != CHUNK || bufs != BUFS) {
        ESP_LOGW(TAG, "%s: %d buffers of %u KB (PSRAM largest %u KB)", tag, bufs,
                 (unsigned)(chunk / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    }
    w->chunk = chunk;
    if (xTaskCreateWithCaps(writer_task, tag, STACK, w, PRIO, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        free_all(w);
        return NULL;
    }
    w->running = true;
    return w;
}

size_t card_writer_chunk(const card_writer_t *w)
{
    return w->chunk;
}

char *card_writer_get(card_writer_t *w)
{
    buf_t b = {0};
    xQueueReceive(w->empty, &b, portMAX_DELAY);
    return b.data;
}

void card_writer_put(card_writer_t *w, char *buf, size_t len)
{
    const buf_t b = {.data = buf, .len = len};
    xQueueSend(len ? w->full : w->empty, &b, portMAX_DELAY);
}

bool card_writer_failed(const card_writer_t *w)
{
    return w->failed;
}

bool card_writer_finish(card_writer_t *w, int64_t *write_us)
{
    if (w->running) {
        const buf_t end = {0};
        xQueueSend(w->full, &end, portMAX_DELAY);
        xSemaphoreTake(w->done, portMAX_DELAY);
    }
    const bool ok = !w->failed;
    if (write_us) {
        *write_us = w->write_us;
    }
    free_all(w);
    return ok;
}
