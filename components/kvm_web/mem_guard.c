/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * When internal RAM runs out, new TLS sessions fail and the console says it
 * cannot reach the device, while the picture itself would have gone on. So
 * when it runs low, give some back: close the idle web connections that only carry
 * requests (a browser opens a new one for its next request) and leave every
 * stream alone - the WebSockets, the MJPEG stream, downloads in progress.
 * Each time is logged, and /api/v1/video/status says so while it lasts, for
 * the console to show.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "web_priv.h"

static const char *TAG = "mem";

/* Below this much free internal RAM, or after a small allocation failed, the
 * device is short. TLS needs a few KB at once to open a session. */
#define LOW_FREE (16 * 1024)
/* A failed allocation this small is a real shortage, not a probe for a big
 * buffer with a fallback behind it. */
#define SMALL_ALLOC 4096
/* Over for good: this much free, and nothing failing, for this long. */
#define OK_FREE (24 * 1024)
#define OK_FOR_US (30LL * 1000 * 1000)
/* Connections are closed at most this often while it lasts. */
#define SHED_EVERY_US (5LL * 1000 * 1000)

/* From the allocator's failure hook: counts only, it may run anywhere. */
static atomic_uint s_failed_small;
static atomic_uint s_failed_any;
static volatile size_t s_last_size;
static volatile uint32_t s_last_caps;
static const char *volatile s_last_fn;

/* When each socket last moved anything, in ms; indexed from LWIP_SOCKET_OFFSET.
 * Only a session quiet for IDLE_MS is closed: one in the middle of a request
 * would answer with a cut connection. */
#define IDLE_MS 3000
static volatile uint32_t s_io_ms[CONFIG_LWIP_MAX_SOCKETS];

void web_io_note(int fd)
{
    const int i = fd - LWIP_SOCKET_OFFSET;
    if (i >= 0 && i < CONFIG_LWIP_MAX_SOCKETS) {
        const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_io_ms[i] = ms ? ms : 1;
    }
}

void web_io_forget(int fd)
{
    const int i = fd - LWIP_SOCKET_OFFSET;
    if (i >= 0 && i < CONFIG_LWIP_MAX_SOCKETS) {
        s_io_ms[i] = 0;
    }
}

/* 0 is a socket not open yet: the server lists a session while its TLS
 * handshake runs, before anything is noted, and the number may still hold the
 * time of the connection that had it before. */
static bool idle(int fd, uint32_t now_ms)
{
    const int i = fd - LWIP_SOCKET_OFFSET;
    return i >= 0 && i < CONFIG_LWIP_MAX_SOCKETS && s_io_ms[i] != 0 &&
           now_ms - s_io_ms[i] >= IDLE_MS;
}

/* Sockets with a response being sent from a task of its own: never closed. */
#define ASYNC_MAX 8
static int s_async_fds[ASYNC_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1};
static portMUX_TYPE s_async_mux = portMUX_INITIALIZER_UNLOCKED;

/* The episode in progress; s_low_since 0 = none. */
static int64_t s_low_since;
static int64_t s_ok_since;
static int64_t s_last_shed;
static uint32_t s_closed;
static uint32_t s_failed_at_start;
static uint32_t s_low_free;

static void on_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    atomic_fetch_add(&s_failed_any, 1);
    if (size <= SMALL_ALLOC) {
        atomic_fetch_add(&s_failed_small, 1);
    }
    s_last_size = size;
    s_last_caps = caps;
    s_last_fn = function_name;
}

static void async_mark(int fd, bool on)
{
    if (fd < 0) {
        return;
    }
    taskENTER_CRITICAL(&s_async_mux);
    for (int i = 0; i < ASYNC_MAX; i++) {
        if (on ? s_async_fds[i] < 0 : s_async_fds[i] == fd) {
            s_async_fds[i] = on ? fd : -1;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_async_mux);
}

static bool async_has(int fd)
{
    bool found = false;
    taskENTER_CRITICAL(&s_async_mux);
    for (int i = 0; i < ASYNC_MAX; i++) {
        if (s_async_fds[i] == fd) {
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_async_mux);
    return found;
}

esp_err_t web_async_begin(httpd_req_t *req, httpd_req_t **out)
{
    const esp_err_t err = httpd_req_async_handler_begin(req, out);
    if (err == ESP_OK) {
        async_mark(httpd_req_to_sockfd(*out), true);
    }
    return err;
}

esp_err_t web_async_complete(httpd_req_t *req)
{
    async_mark(httpd_req_to_sockfd(req), false);
    return httpd_req_async_handler_complete(req);
}

/* Close every idle session that only carries requests. Returns how many. */
static uint32_t shed(void)
{
    extern httpd_handle_t s_httpd;
    httpd_handle_t hd = s_httpd;
    if (!hd) {
        return 0;
    }
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(hd, &n, fds) != ESP_OK) {
        return 0;
    }
    uint32_t closed = 0;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(hd, fds[i]) == HTTPD_WS_CLIENT_HTTP && !async_has(fds[i]) &&
            idle(fds[i], now_ms) &&
            httpd_sess_trigger_close(hd, fds[i]) == ESP_OK) {
            closed++;
        }
    }
    return closed;
}

static void guard_task(void *arg)
{
    (void)arg;
    unsigned seen_small = 0;
    unsigned seen_any = 0;
    int64_t last_log = 0;
    int64_t last_shed_log = 0;
    uint32_t unlogged = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_low_since ? 250 : 500));
        const int64_t now = esp_timer_get_time();
        const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const unsigned small = atomic_load(&s_failed_small);
        const unsigned any = atomic_load(&s_failed_any);
        const bool new_small = small != seen_small;

        if (any != seen_any && now - last_log >= 10LL * 1000 * 1000) {
            /* The hook itself cannot log; this says it at most every 10 s. */
            last_log = now;
            ESP_LOGW(TAG, "%u allocation(s) failed, the last %u bytes caps 0x%" PRIx32 " in %s; "
                          "internal %u KB free, largest %u KB",
                     any - seen_any, (unsigned)s_last_size, s_last_caps,
                     s_last_fn ? s_last_fn : "?", (unsigned)(free_now / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
            seen_any = any;
        }
        seen_small = small;

        const bool short_now = free_now < LOW_FREE || new_small;
        if (short_now) {
            if (!s_low_since) {
                s_low_since = now;
                s_closed = 0;
                s_failed_at_start = any;
                s_low_free = (uint32_t)free_now;
                ESP_LOGW(TAG, "internal RAM low: %u KB free; closing connections that only carry "
                              "requests, streams stay",
                         (unsigned)(free_now / 1024));
            }
            if (free_now < s_low_free) {
                s_low_free = (uint32_t)free_now;
            }
            s_ok_since = 0;
            if (now - s_last_shed >= SHED_EVERY_US) {
                s_last_shed = now;
                const uint32_t n = shed();
                s_closed += n;
                unlogged += n;
            }
            /* A summary, not a line per pass: the log ring is small. */
            if (unlogged && now - last_shed_log >= 30LL * 1000 * 1000) {
                ESP_LOGW(TAG, "closed %" PRIu32 " idle connection(s) to free memory; internal %u KB free",
                         unlogged, (unsigned)(free_now / 1024));
                unlogged = 0;
                last_shed_log = now;
            }
        } else if (s_low_since) {
            if (free_now < OK_FREE) {
                s_ok_since = 0;
            } else if (!s_ok_since) {
                s_ok_since = now;
            } else if (now - s_ok_since >= OK_FOR_US) {
                ESP_LOGI(TAG, "internal RAM back: %u KB free after %" PRId64 " s; lowest %" PRIu32
                              " KB, %" PRIu32 " connection(s) closed, %u allocation(s) failed",
                         (unsigned)(free_now / 1024), (now - s_low_since) / 1000000,
                         s_low_free / 1024, s_closed, (unsigned)(any - s_failed_at_start));
                s_low_since = 0;
            }
        }
    }
}

void web_mem_guard_start(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    (void)heap_caps_register_failed_alloc_callback(on_alloc_failed);
    if (xTaskCreateWithCaps(guard_task, "mem_guard", 3072, NULL, 7, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no guard task");
    }
}

int web_mem_guard_json(char *out, size_t cap)
{
    const int64_t since = s_low_since;
    if (!since) {
        return snprintf(out, cap, "null");
    }
    return snprintf(out, cap,
                    "{\"seconds\":%" PRId64 ",\"freeKb\":%u,\"lowestKb\":%" PRIu32
                    ",\"closed\":%" PRIu32 ",\"failed\":%u}",
                    (esp_timer_get_time() - since) / 1000000,
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                    s_low_free / 1024, s_closed,
                    (unsigned)(atomic_load(&s_failed_any) - s_failed_at_start));
}
