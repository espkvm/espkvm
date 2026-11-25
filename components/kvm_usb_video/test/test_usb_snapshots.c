/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define CAPTURE_DIRECT_ENCODE 0
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_NO_MEM 5
#define ESP_ERR_INVALID_STATE 6
#define VIDEO_PAYLOAD_JPEG 1
#define VIDEO_PAYLOAD_H264 2
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(mu) ((void)(mu))
#define portEXIT_CRITICAL(mu) ((void)(mu))

typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef struct { bool signal; } kvm_video_status_t;
typedef struct { bool available; } semaphore_t;

static semaphore_t one, done, stream_mu, stream_done;
static semaphore_t *s_one = &one, *s_done = &done;
static semaphore_t *s_stream_mu = &stream_mu, *s_stream_done = &stream_done;
static int s_mu, payload;
static bool usb, s_wanted, s_stream_active, s_stream_wanted;
static int s_stream_ready = -1, s_stream_front = -1;
static uint8_t *s_jpeg, *s_stream_buf[2];
static size_t s_jpeg_len, s_stream_len[2];
static esp_err_t s_err, s_stream_err;
static void (*while_waiting)(void);
static unsigned waits, hdmi_copies;

static semaphore_t *xSemaphoreCreateMutex(void) { return &one; }
static semaphore_t *xSemaphoreCreateBinary(void) { return &done; }
static TickType_t xTaskGetTickCount(void) { return 0; }
static void xSemaphoreGive(semaphore_t *sem) { sem->available = true; }
static int xSemaphoreTake(semaphore_t *sem, TickType_t ticks)
{
    if (ticks && (sem == &done || sem == &stream_done)) {
        waits++;
        if (while_waiting) {
            void (*event)(void) = while_waiting;
            while_waiting = NULL;
            event();
        }
    }
    const bool available = sem->available;
    sem->available = false;
    return available;
}

static bool capture_source_is_usb(void) { return usb; }
static void capture_status_get(kvm_video_status_t *out) { out->signal = true; }
static int video_frame_payload(void) { return payload; }
static void video_frame_viewer_enter(void) {}
static void video_frame_viewer_leave(void) {}
static void stream_sync_init(void) {}
static esp_err_t copy_published(uint8_t **out, size_t *len, uint32_t ms)
{
    (void)out;
    (void)len;
    assert(ms == 3000);
    hdmi_copies++;
    return ESP_OK;
}

/* Compile the production snapshot entry points and source-change callback. */
#include "usb_snapshot_handlers.inc"

static void start_usb(void)
{
    usb = true;
    payload = VIDEO_PAYLOAD_H264;
    capture_snapshot_usb_started();
}

int main(void)
{
    uint8_t *jpeg;
    const uint8_t *vnc_jpeg;
    size_t len;
    one.available = true;
    payload = VIDEO_PAYLOAD_H264;
    while_waiting = start_usb;
    assert(capture_snapshot_jpeg(&jpeg, &len, 3000) == ESP_ERR_NOT_SUPPORTED);
    assert(waits == 1 && !s_wanted && !jpeg && !len);
    assert(capture_snapshot_jpeg(&jpeg, &len, 3000) == ESP_ERR_NOT_SUPPORTED);
    assert(capture_vnc_snapshot_begin() == ESP_ERR_NOT_SUPPORTED);
    assert(waits == 1);

    usb = false;
    assert(capture_vnc_snapshot_begin() == ESP_OK);
    while_waiting = start_usb;
    assert(capture_vnc_snapshot_next(&vnc_jpeg, &len, 1800) == ESP_ERR_NOT_SUPPORTED);
    assert(waits == 2 && !s_stream_wanted && !vnc_jpeg && !len);
    assert(capture_vnc_snapshot_next(&vnc_jpeg, &len, 1800) == ESP_ERR_NOT_SUPPORTED);
    assert(waits == 2);

    usb = false;
    payload = VIDEO_PAYLOAD_JPEG;
    assert(capture_snapshot_jpeg(&jpeg, &len, 3000) == ESP_OK);
    assert(hdmi_copies == 1 && waits == 2);
}
