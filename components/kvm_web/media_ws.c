/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual media from a console tab (kvm_browser_media.h).
 *
 *   WS /ws/media, binary, little-endian. Operator sign-ins only.
 *
 *   tab -> device
 *     0x01 attach   cdrom u8, size u64, token u8[16], name (UTF-8, rest of the message)
 *     0x02 data     id u32, then the bytes asked for
 *     0x03 failed   id u32: the tab could not read that part of the file
 *     0x04 detach   the tab stops serving
 *   device -> tab
 *     0x81 read     id u32, offset u64, length u32 (at most 64 KB)
 *     0x82 state    u8: 0 serving, 1 another tab took over, 2 virtual media is
 *                   off, 3 view-only sign-in, 4 ejected
 *
 * The media task's read is split into parts of 64 KB, all asked for at once,
 * and it waits until the tab has answered each. 64 KB keeps the web server's
 * task busy for a few milliseconds per part, so other requests do not queue
 * behind a big one. While the target reads straight on, the next two 256 KB
 * chunks are asked for too, each into a buffer of its own, so the tab is
 * sending while the target takes a chunk over USB.
 *
 * The token names the file, not the socket. When the socket drops (a network
 * blip, a reload) reads wait up to GRACE_MS for a tab with the same token, and
 * the parts not answered yet are asked for again. After that the medium is
 * gone and the target sees an empty drive.
 */
#include "kvm_browser_media.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kvm_auth.h"
#include "kvm_media.h"
#include "kvm_settings.h"
#include "usb_hid.h"
#include "video_frame.h"
#include "web_priv.h"

static const char *TAG = "media_ws";

#define OP_ATTACH 0x01
#define OP_DATA 0x02
#define OP_FAILED 0x03
#define OP_DETACH 0x04
#define OP_READ 0x81
#define OP_STATE 0x82

enum { ST_SERVING, ST_REPLACED, ST_MEDIA_OFF, ST_VIEW_ONLY, ST_EJECTED };

#define PART_MAX (64 * 1024)
#define PARTS 4 /* the media layer reads 256 KB at most */
#define READ_AHEAD (PARTS * PART_MAX)
/* Chunks asked for ahead. With one, the network stood still while the tab
 * turned each request round: two keep it busy. */
#define AHEAD 2
#define TOKEN_LEN 16
#define GRACE_MS 15000
/* A tab that is connected but does not answer: a stuck read, a sleeping tab.
 * Well inside the host's own 30 s, after which it resets the drive. */
#define ANSWER_MS 10000

typedef struct {
    uint32_t id;
    uint64_t off;
    uint32_t len;
    uint8_t *dst;
    bool asked;
    bool done;
    bool failed;
} part_t;

static SemaphoreHandle_t s_mu;
static int s_fd = -1;             /* the tab serving, -1 while it is away */
static bool s_have;               /* a file is attached */
static bool s_gone;               /* ...and it went away: reads answer "no medium" */
static uint8_t s_token[TOKEN_LEN];
/* The file that ended last. Its tab may still be trying to come back: it is
 * told the drive was ejected, and does not take the drive back. */
static uint8_t s_ended[TOKEN_LEN];
static bool s_ended_set;
static uint64_t s_size;
static bool s_cdrom;
static char s_name[64];
static int64_t s_lost_us;         /* when the tab dropped, 0 while connected */
/* The parts of one read. Group 0 is a read the media task asked for, landing
 * in its buffer. The others are chunks asked for ahead, in buffers of their own. */
typedef struct {
    uint64_t off;
    uint32_t len;
    uint8_t *buf;
    part_t parts[PARTS];
    int np; /* 0: not in use */
} group_t;
#define GROUPS (1 + AHEAD)
static group_t s_g[GROUPS];
static TaskHandle_t s_waiter;
static uint32_t s_next_id = 1;
static uint8_t *s_rx;             /* one message, PSRAM; only the web server task uses it */
static int s_asm_fd = -1;         /* the socket whose message s_rx is gathering */
static size_t s_asm_len;
static esp_timer_handle_t s_grace;

/* While the target reads, video drops to VIDEO_UPLOAD_FPS, as for an upload:
 * at full frame rate the encoder takes the bus from the Ethernet MAC and the
 * reads slow down. Back to full rate 2 s after the last read. */
#define SLOW_IDLE_US (2 * 1000 * 1000)
static esp_timer_handle_t s_slow_timer;
static portMUX_TYPE s_slow_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_slow;
static int64_t s_last_read_us;

/* video_frame_upload_begin/end inside the lock (a spinlock of their own
   nests), so a read and the timer never leave the count off by one. */
static void slow_video(void)
{
    taskENTER_CRITICAL(&s_slow_mux);
    s_last_read_us = esp_timer_get_time();
    if (!s_slow) {
        s_slow = true;
        video_frame_upload_begin();
    }
    taskEXIT_CRITICAL(&s_slow_mux);
}

/* From the timer, or with @p now set when the medium goes. */
static void slow_check(bool now)
{
    taskENTER_CRITICAL(&s_slow_mux);
    if (s_slow && (now || esp_timer_get_time() - s_last_read_us > SLOW_IDLE_US)) {
        s_slow = false;
        video_frame_upload_end();
    }
    taskEXIT_CRITICAL(&s_slow_mux);
}

static void slow_tick(void *arg)
{
    (void)arg;
    slow_check(false);
}

static const kvm_media_ops_t s_ops;

static void send_state(int fd, uint8_t st)
{
    const uint8_t msg[2] = {OP_STATE, st};
    if (fd >= 0) {
        (void)kvm_web_ws_send_binary(fd, msg, sizeof(msg));
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Ask the tab for the parts not asked yet. Called without s_mu: a send may
 * wait on the socket, and the data handler needs s_mu meanwhile. */
static void ask_parts(void)
{
    uint8_t msgs[GROUPS * PARTS][17];
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int fd = s_fd;
    for (int i = 0; fd >= 0 && i < GROUPS * PARTS; i++) {
        group_t *g = &s_g[i / PARTS];
        if (i % PARTS >= g->np) {
            continue;
        }
        part_t *p = &g->parts[i % PARTS];
        if (p->asked || p->done) {
            continue;
        }
        p->asked = true;
        uint8_t *m = msgs[n++];
        m[0] = OP_READ;
        put32(m + 1, p->id);
        memcpy(m + 5, &p->off, 8);
        put32(m + 13, p->len);
    }
    xSemaphoreGive(s_mu);
    for (int i = 0; i < n; i++) {
        if (kvm_web_ws_send_binary(fd, msgs[i], sizeof(msgs[i])) == ESP_OK) {
            continue;
        }
        /* Not sent: ask again on the next round, the socket may still be fine. */
        xSemaphoreTake(s_mu, portMAX_DELAY);
        for (int k = i; k < n; k++) {
            const uint32_t id = get32(msgs[k] + 1);
            for (int g = 0; g < GROUPS; g++) {
                for (int j = 0; j < s_g[g].np; j++) {
                    if (s_g[g].parts[j].id == id) {
                        s_g[g].parts[j].asked = false;
                    }
                }
            }
        }
        xSemaphoreGive(s_mu);
        break;
    }
}

/* ---- the source, as the media task sees it ------------------------------ */

/* Under s_mu: split [off, off + len) into @p g's parts, landing in @p dst. */
static void plan_group(group_t *g, uint64_t off, uint8_t *dst, uint32_t len)
{
    g->off = off;
    g->len = len;
    g->np = 0;
    for (uint32_t done = 0; done < len && g->np < PARTS; g->np++) {
        const uint32_t n = len - done < PART_MAX ? len - done : PART_MAX;
        g->parts[g->np] = (part_t){.id = s_next_id++, .off = off + done, .len = n, .dst = dst + done};
        done += n;
    }
}

static esp_err_t wait_group(const group_t *g)
{
    int64_t asked_us = esp_timer_get_time();
    /* Look before sleeping: a chunk asked for ahead is often all in by now. */
    for (bool first = true;; first = false) {
        if (!first) {
            ask_parts();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
        xSemaphoreTake(s_mu, portMAX_DELAY);
        int done = 0;
        bool failed = false;
        for (int i = 0; i < g->np; i++) {
            done += g->parts[i].done;
            failed |= g->parts[i].failed;
        }
        esp_err_t result = ESP_ERR_NOT_FINISHED; /* still waiting */
        if (s_gone) {
            result = ESP_ERR_NOT_FOUND;
        } else if (failed) {
            result = ESP_FAIL;
        } else if (done == g->np) {
            result = ESP_OK;
        } else if (s_fd < 0) {
            asked_us = esp_timer_get_time(); /* waiting for the tab, not for an answer */
        } else if (esp_timer_get_time() - asked_us > (int64_t)ANSWER_MS * 1000) {
            ESP_LOGW(TAG, "the tab did not answer a read at %" PRIu64, g->off);
            result = ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(s_mu);
        if (result != ESP_ERR_NOT_FINISHED) {
            return result;
        }
    }
}

/* Under s_mu: keep AHEAD chunks asked for past the read that just ended at @p end. */
static void top_up(uint64_t end)
{
    for (int k = 1; k < GROUPS; k++) {
        if (s_g[k].np && s_g[k].off + s_g[k].len > end) {
            end = s_g[k].off + s_g[k].len;
        }
    }
    for (int k = 1; k < GROUPS && end < s_size; k++) {
        if (s_g[k].np || !s_g[k].buf) {
            continue;
        }
        const uint32_t n = s_size - end < READ_AHEAD ? (uint32_t)(s_size - end) : READ_AHEAD;
        plan_group(&s_g[k], end, s_g[k].buf, n);
        end += n;
    }
}

static esp_err_t src_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_gone || !s_have) {
        xSemaphoreGive(s_mu);
        return ESP_ERR_NOT_FOUND;
    }
    slow_video();
    /* A chunk asked for ahead, or a read of its own. A read elsewhere lets
     * the chunks ahead go: an answer for them now finds no part and is dropped. */
    int k = 0;
    for (int i = 1; i < GROUPS && !k; i++) {
        if (s_g[i].np && s_g[i].off == off && s_g[i].len == len) {
            k = i;
        }
    }
    if (!k) {
        for (int i = 1; i < GROUPS; i++) {
            s_g[i].np = 0;
        }
        plan_group(&s_g[0], off, buf, len);
    }
    s_waiter = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_mu);

    const esp_err_t result = wait_group(&s_g[k]);
    if (k && result == ESP_OK) {
        memcpy(buf, s_g[k].buf, len);
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_g[k].np = 0;
    const bool ahead = result == ESP_OK && len == READ_AHEAD;
    if (ahead) {
        top_up(off + len);
    }
    xSemaphoreGive(s_mu);
    if (ahead) {
        ask_parts();
    }
    return result;
}

static void src_close(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int fd = s_fd;
    const bool told = s_have && !s_gone;
    if (s_have) {
        memcpy(s_ended, s_token, TOKEN_LEN);
        s_ended_set = true;
    }
    s_have = false;
    s_gone = true;
    s_fd = -1;
    xSemaphoreGive(s_mu);
    esp_timer_stop(s_grace);
    slow_check(true);
    if (told) {
        send_state(fd, ST_EJECTED); /* another medium was picked */
    }
}

/* The medium is about to be taken out: a read waiting for the tab gives up. */
static void src_cancel(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_gone = true;
    TaskHandle_t w = s_waiter;
    xSemaphoreGive(s_mu);
    if (w) {
        xTaskNotifyGive(w);
    }
}

static const kvm_media_ops_t s_ops = {
    .kind = "browser",
    .read = src_read,
    .close = src_close,
    .cancel = src_cancel,
    .read_ahead = READ_AHEAD,
};

/* The tab stayed away: the medium is gone. A read waiting now gives up within
 * its 100 ms tick, so the eject waits no longer than that. */
static void grace_over(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool over = s_have && s_fd < 0;
    if (over) {
        s_gone = true;
    }
    xSemaphoreGive(s_mu);
    if (over) {
        ESP_LOGI(TAG, "the tab did not come back");
        kvm_media_eject_ops(&s_ops);
    }
}

/* ---- the socket --------------------------------------------------------- */

static void attach(int fd, const uint8_t *m, size_t len)
{
    if (len < 1 + 1 + 8 + TOKEN_LEN) {
        return;
    }
    const bool cdrom = m[1] != 0;
    uint64_t size;
    memcpy(&size, m + 2, 8);
    const uint8_t *token = m + 10;
    char name[64];
    const size_t nlen = len - 26 < sizeof(name) - 1 ? len - 26 : sizeof(name) - 1;
    memcpy(name, m + 26, nlen);
    name[nlen] = '\0';

    if (kvm_auth_socket_view_only(fd)) {
        send_state(fd, ST_VIEW_ONLY);
        return;
    }
    if (!kvm_setting_bool("msc_enable")) {
        send_state(fd, ST_MEDIA_OFF);
        return;
    }

    /* The same file again: the tab is back after a drop or a reload. */
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_ended_set && memcmp(s_ended, token, TOKEN_LEN) == 0) {
        xSemaphoreGive(s_mu);
        send_state(fd, ST_EJECTED); /* it ended while the tab was away */
        return;
    }
    if (s_have && !s_gone && memcmp(s_token, token, TOKEN_LEN) == 0) {
        const int old = s_fd;
        s_fd = fd;
        s_lost_us = 0;
        for (int g = 0; g < GROUPS; g++) {
            for (int i = 0; i < s_g[g].np; i++) {
                s_g[g].parts[i].asked = s_g[g].parts[i].done; /* ask again what is still open */
            }
        }
        TaskHandle_t w = s_waiter;
        xSemaphoreGive(s_mu);
        esp_timer_stop(s_grace);
        if (w) {
            xTaskNotifyGive(w);
        }
        if (old >= 0 && old != fd) {
            send_state(old, ST_REPLACED);
        }
        send_state(fd, ST_SERVING);
        ESP_LOGI(TAG, "the tab is back");
        return;
    }
    const int prev = s_fd;
    xSemaphoreGive(s_mu);

    /* Another file: out with the one before, if any. */
    kvm_media_eject_ops(&s_ops);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(s_token, token, TOKEN_LEN);
    s_size = size;
    s_cdrom = cdrom;
    strlcpy(s_name, name, sizeof(s_name));
    s_fd = fd;
    s_have = true;
    s_gone = false;
    s_lost_us = 0;
    for (int g = 0; g < GROUPS; g++) {
        s_g[g].np = 0;
        if (g && !s_g[g].buf) { /* without one, that chunk is not read ahead */
            s_g[g].buf = heap_caps_malloc(READ_AHEAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    xSemaphoreGive(s_mu);

    /* The setting says what the drive holds, as for every medium; it also
     * keeps the choice in the console in step. Then the file goes in. */
    const char *active = kvm_setting_str("msc_image");
    if (!active || strcmp(active, "@browser") != 0) {
        (void)kvm_setting_set_str("msc_image", "@browser");
    }
    kvm_media_source_t src = {.ops = &s_ops, .size = size, .cdrom = cdrom};
    strlcpy(src.name, name, sizeof(src.name));
    if (kvm_media_insert(&src) != ESP_OK) {
        send_state(fd, ST_EJECTED); /* too small to be a disk */
        return;
    }
    usb_hid_msc_set_type(cdrom);
    if (prev >= 0 && prev != fd) {
        send_state(prev, ST_REPLACED);
    }
    send_state(fd, ST_SERVING);
}

static void detach(int fd)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool mine = s_have && s_fd == fd;
    xSemaphoreGive(s_mu);
    if (mine) {
        /* Through the setting, as picking "No medium" does. */
        (void)kvm_setting_set_str("msc_image", "");
        kvm_media_eject_ops(&s_ops);
    }
}

static void data(int fd, const uint8_t *m, size_t len, bool failed)
{
    if (len < 5) {
        return;
    }
    const uint32_t id = get32(m + 1);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    TaskHandle_t w = NULL;
    for (int i = 0; fd == s_fd && i < GROUPS * PARTS; i++) {
        if (i % PARTS >= s_g[i / PARTS].np) {
            continue;
        }
        part_t *p = &s_g[i / PARTS].parts[i % PARTS];
        if (p->id != id || p->done) {
            continue;
        }
        if (failed || len - 5 != p->len) {
            p->failed = true;
        } else {
            memcpy(p->dst, m + 5, p->len);
        }
        p->done = true;
        w = s_waiter;
        break;
    }
    xSemaphoreGive(s_mu);
    if (w) {
        xTaskNotifyGive(w);
    }
}

static esp_err_t media_ws_handler(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (!kvm_auth_socket_ok(fd)) {
        return kvm_auth_reject_ws(req);
    }
    httpd_ws_frame_t pkt = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        return ESP_OK;
    }
    /*
     * A browser may send a big message as several frames: the first binary,
     * the rest continuations. Each continuation goes after the one before,
     * and the message is read once the last is in. Read as messages of their
     * own, a continuation's first data byte was taken for a command. Anything
     * else, or a message over the biggest there is, closes the socket: left
     * unread, its payload would be read as the next frame.
     */
    if (pkt.type == HTTPD_WS_TYPE_BINARY) {
        s_asm_fd = fd;
        s_asm_len = 0;
    } else if (pkt.type != HTTPD_WS_TYPE_CONTINUE || s_asm_fd != fd) {
        return ESP_FAIL;
    }
    if (!s_rx || pkt.len > PART_MAX + 5 - s_asm_len) {
        s_asm_fd = -1;
        return ESP_FAIL;
    }
    if (pkt.len) {
        pkt.payload = s_rx + s_asm_len;
        err = httpd_ws_recv_frame(req, &pkt, pkt.len);
        if (err != ESP_OK) {
            s_asm_fd = -1;
            return err;
        }
        s_asm_len += pkt.len;
    }
    if (!pkt.final) {
        return ESP_OK;
    }
    s_asm_fd = -1;
    const uint8_t *buf = s_rx;
    const size_t len = s_asm_len;
    if (!len) {
        return ESP_OK;
    }
    /*
     * A tab sends without pause while the target reads, and taking it in is
     * all decryption: the web server's task would hold its core for as long
     * as the target reads, and the task watchdog restarts the device after
     * 10 s without the idle task. A tick off every 100 ms keeps that from
     * happening, for about a tenth of the speed.
     */
    static int64_t s_ran_since;
    const int64_t now = esp_timer_get_time();
    if (!s_ran_since || now - s_ran_since > 1000000) {
        s_ran_since = now; /* a pause: start counting again */
    } else if (now - s_ran_since > 100000) {
        vTaskDelay(1);
        s_ran_since = esp_timer_get_time();
    }
    switch (buf[0]) {
    case OP_ATTACH:
        attach(fd, buf, len);
        break;
    case OP_DATA:
    case OP_FAILED:
        data(fd, buf, len, buf[0] == OP_FAILED);
        break;
    case OP_DETACH:
        detach(fd);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void media_ws_drop(int fd)
{
    if (!s_mu) {
        return;
    }
    if (s_asm_fd == fd) {
        s_asm_fd = -1;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ours = s_have && !s_gone && s_fd == fd;
    if (ours) {
        s_fd = -1;
        s_lost_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_mu);
    if (ours) {
        ESP_LOGI(TAG, "the tab dropped; waiting %d s for it", GRACE_MS / 1000);
        esp_timer_start_once(s_grace, (uint64_t)GRACE_MS * 1000);
    }
}

esp_err_t media_ws_init(void)
{
    if (s_mu) {
        return ESP_OK;
    }
    s_mu = xSemaphoreCreateMutex();
    s_rx = heap_caps_malloc(PART_MAX + 5, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_timer_create_args_t t = {.callback = grace_over, .name = "media_grace"};
    const esp_timer_create_args_t st = {.callback = slow_tick, .name = "media_slow"};
    if (!s_mu || !s_rx || esp_timer_create(&t, &s_grace) != ESP_OK ||
        esp_timer_create(&st, &s_slow_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    esp_timer_start_periodic(s_slow_timer, SLOW_IDLE_US / 2);
    return ESP_OK;
}

bool kvm_browser_media_inserted(void)
{
    return kvm_media_is(&s_ops);
}

void kvm_browser_media_status(kvm_browser_media_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = "off";
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_have && !s_gone) {
        out->state = s_fd >= 0 ? "serving" : "waiting";
        strlcpy(out->name, s_name, sizeof(out->name));
        out->size = s_size;
    }
    xSemaphoreGive(s_mu);
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/ws/media",
     .method = HTTP_GET,
     .handler = media_ws_handler,
     .is_websocket = true,
     .ws_pre_handshake_cb = kvm_web_ws_pre_handshake},
};

const httpd_uri_t *media_ws_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
