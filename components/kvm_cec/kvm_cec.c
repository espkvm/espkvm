/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI-CEC: the protocol on top of the bridge's three CEC operations.
 *
 * One task owns the line. It polls the bridge for frames, answers what a TV is
 * asked, keeps the table of devices, and runs the frames the API queues. Sending
 * is one frame at a time and waits for the line's verdict; frames heard while
 * waiting are held and handled after.
 */
#include "kvm_cec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "capture.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_caps.h"
#include "kvm_settings.h"

static const char *TAG = "cec";

/* Opcodes this file speaks. */
#define OP_FEATURE_ABORT 0x00
#define OP_IMAGE_VIEW_ON 0x04
#define OP_TEXT_VIEW_ON 0x0d
#define OP_STANDBY 0x36
#define OP_USER_PRESSED 0x44
#define OP_USER_RELEASED 0x45
#define OP_SET_OSD_NAME 0x47
#define OP_GIVE_OSD_NAME 0x46
#define OP_ACTIVE_SOURCE 0x82
#define OP_GIVE_PHYS_ADDR 0x83
#define OP_REPORT_PHYS_ADDR 0x84
#define OP_REQUEST_ACTIVE 0x85
#define OP_SET_STREAM_PATH 0x86
#define OP_VENDOR_ID 0x87
#define OP_GIVE_VENDOR_ID 0x8c
#define OP_GIVE_POWER 0x8f
#define OP_REPORT_POWER 0x90
#define OP_GET_MENU_LANG 0x91
#define OP_SET_MENU_LANG 0x32
#define OP_INACTIVE_SOURCE 0x9d
#define OP_CEC_VERSION 0x9e
#define OP_GET_CEC_VERSION 0x9f
#define OP_ABORT 0xff

#define ABORT_UNRECOGNIZED 0x00
#define ABORT_REFUSED 0x04

#define BROADCAST 0x0f
#define POLL_MS 15
#define TX_TIMEOUT_MS 1000
#define RESCAN_MS 60000
#define HELD_MAX 8

/* Someone waiting for a frame's verdict. On the heap: a waiter that gives up
 * leaves it behind for the task to write into, rather than its own stack. */
typedef struct {
    SemaphoreHandle_t done;
    uint8_t result;
} waiter_t;

typedef struct {
    kvm_bridge_cec_frame_t f;
    waiter_t *w; /* NULL: nobody waits */
} tx_req_t;

static const kvm_bridge_t *s_bridge;
static QueueHandle_t s_txq;
static SemaphoreHandle_t s_mu; /* the table and the log */
static kvm_cec_device_t s_dev[KVM_CEC_DEVICES];
static int8_t s_active = -1;
static uint8_t s_own = 0;
static bool s_running;
static volatile bool s_rescan;
static kvm_cec_log_t s_log[KVM_CEC_LOG];
static unsigned s_log_n;
static uint16_t s_ask;               /* logical addresses to ask who they are */
static uint32_t s_asked_ms[KVM_CEC_DEVICES];
static kvm_bridge_cec_frame_t s_held[HELD_MAX];
static int s_held_n;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void log_frame(bool tx, uint8_t result, const kvm_bridge_cec_frame_t *f)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    kvm_cec_log_t *e = &s_log[s_log_n % KVM_CEC_LOG];
    e->ms = now_ms();
    e->tx = tx;
    e->result = result;
    e->len = f->len;
    memcpy(e->data, f->data, f->len);
    s_log_n++;
    xSemaphoreGive(s_mu);
}

static void forget(kvm_cec_device_t *d)
{
    memset(d, 0, sizeof(*d));
    d->phys_addr = 0xffff;
    d->type = -1;
    d->vendor = 0xffffffff;
    d->power = KVM_CEC_POWER_UNKNOWN;
    d->version = -1;
}

/* ---------------------------------------------------------------- the line */

/* Send one frame and wait for the verdict: 0 ok, 1 nack, 2 arbitration lost,
 * 3 error, 4 timeout. Frames heard meanwhile are held for the main loop. */
static uint8_t tx_now(const kvm_bridge_cec_frame_t *f)
{
    uint8_t result = 4;
    for (int attempt = 0; attempt < 3; attempt++) {
        /* A retry waits longer for a free line, as the spec asks. */
        if (s_bridge->ops->cec_transmit(s_bridge->dev, f, attempt ? 7 : 5) != ESP_OK) {
            result = 3;
            break;
        }
        result = 4;
        for (int t = 0; t < TX_TIMEOUT_MS / POLL_MS; t++) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS) ? pdMS_TO_TICKS(POLL_MS) : 1);
            kvm_bridge_cec_frame_t rx = {0};
            bool got = false;
            kvm_bridge_cec_tx_t v = KVM_BRIDGE_CEC_TX_NONE;
            s_bridge->ops->cec_poll(s_bridge->dev, &rx, &got, &v);
            if (got && s_held_n < HELD_MAX) {
                s_held[s_held_n++] = rx;
            }
            if (v != KVM_BRIDGE_CEC_TX_NONE) {
                result = v == KVM_BRIDGE_CEC_TX_OK         ? 0
                         : v == KVM_BRIDGE_CEC_TX_NACK     ? 1
                         : v == KVM_BRIDGE_CEC_TX_ARB_LOST ? 2
                                                           : 3;
                break;
            }
        }
        /* Only a collision or a glitch is worth another go; a NACK is an answer. */
        if (result != 2 && result != 3) {
            break;
        }
    }
    log_frame(true, result, f);
    return result;
}

static uint8_t send2(uint8_t dest, uint8_t op)
{
    kvm_bridge_cec_frame_t f = {.len = 2, .data = {(uint8_t)(s_own << 4 | dest), op}};
    return tx_now(&f);
}

static void feature_abort(uint8_t dest, uint8_t op, uint8_t reason)
{
    kvm_bridge_cec_frame_t f = {.len = 4,
                                .data = {(uint8_t)(s_own << 4 | dest), OP_FEATURE_ABORT, op, reason}};
    (void)tx_now(&f);
}

/* ---------------------------------------------------------------- hearing */

static void handle(const kvm_bridge_cec_frame_t *f)
{
    log_frame(false, 0, f);
    if (f->len < 1) {
        return;
    }
    const uint8_t from = f->data[0] >> 4;
    const uint8_t to = f->data[0] & 0x0f;
    if (f->len == 1 || from == BROADCAST) {
        return; /* a poll, or an unregistered device */
    }
    const uint8_t op = f->data[1];
    const uint8_t *arg = &f->data[2];
    const int n = f->len - 2;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    kvm_cec_device_t *d = (from < KVM_CEC_DEVICES) ? &s_dev[from] : NULL;
    if (d && from != s_own) {
        d->present = true;
        d->seen_ms = now_ms();
        /* A device we know nothing about spoke - waking up, or just plugged in:
           ask who it is now rather than at the next scan. */
        if (!d->name[0] && op != OP_SET_OSD_NAME && now_ms() - s_asked_ms[from] > 10000) {
            s_ask |= (uint16_t)(1u << from);
        }
    }
    switch (op) {
    case OP_REPORT_PHYS_ADDR:
        /* Broadcast unasked, this is a device that has just taken the
           address: maybe another one than before (a Switch where a TV stick
           was), so what we knew about the address is dropped and asked again.
           Our own Give Physical Address gets the same broadcast back. */
        if (d && from != s_own && to == BROADCAST && now_ms() - s_asked_ms[from] > 5000) {
            d->name[0] = '\0';
            d->vendor = 0xffffffff;
            d->version = -1;
            s_ask |= (uint16_t)(1u << from);
        }
        if (d && n >= 3) {
            d->phys_addr = (uint16_t)(arg[0] << 8 | arg[1]);
            d->type = (int8_t)arg[2];
        }
        break;
    case OP_VENDOR_ID:
        if (d && n >= 3) {
            d->vendor = (uint32_t)arg[0] << 16 | (uint32_t)arg[1] << 8 | arg[2];
        }
        break;
    case OP_SET_OSD_NAME:
        if (d && n >= 1) {
            const int k = n < (int)sizeof(d->name) - 1 ? n : (int)sizeof(d->name) - 1;
            memcpy(d->name, arg, k);
            d->name[k] = '\0';
        }
        break;
    case OP_REPORT_POWER:
        if (d && n >= 1) {
            d->power = (int8_t)arg[0];
        }
        break;
    case OP_CEC_VERSION:
        if (d && n >= 1) {
            d->version = (int8_t)arg[0];
        }
        break;
    case OP_ACTIVE_SOURCE:
    case OP_IMAGE_VIEW_ON:
    case OP_TEXT_VIEW_ON:
        s_active = (int8_t)from;
        if (d) {
            d->power = KVM_CEC_POWER_ON;
        }
        break;
    case OP_INACTIVE_SOURCE:
        if (s_active == from) {
            s_active = -1;
        }
        break;
    case OP_STANDBY:
        /* A device that says Standby to everyone is going to sleep itself. */
        if (d && to == BROADCAST) {
            d->power = KVM_CEC_POWER_STANDBY;
        }
        break;
    default:
        break;
    }
    xSemaphoreGive(s_mu);

    if (to != s_own) {
        return; /* broadcasts are heard, not answered */
    }
    /* What a TV is asked. */
    switch (op) {
    case OP_GIVE_PHYS_ADDR: {
        kvm_bridge_cec_frame_t r = {
            .len = 5, .data = {(uint8_t)(s_own << 4 | BROADCAST), OP_REPORT_PHYS_ADDR, 0x00, 0x00, 0x00}};
        (void)tx_now(&r);
        break;
    }
    case OP_GIVE_OSD_NAME: {
        static const char name[] = "ESP-KVM";
        kvm_bridge_cec_frame_t r = {.len = 2 + sizeof(name) - 1, .data = {(uint8_t)(s_own << 4 | from), OP_SET_OSD_NAME}};
        memcpy(&r.data[2], name, sizeof(name) - 1);
        (void)tx_now(&r);
        break;
    }
    case OP_GET_CEC_VERSION: {
        kvm_bridge_cec_frame_t r = {.len = 3, .data = {(uint8_t)(s_own << 4 | from), OP_CEC_VERSION, 0x05}};
        (void)tx_now(&r);
        break;
    }
    case OP_GIVE_POWER: {
        kvm_bridge_cec_frame_t r = {.len = 3, .data = {(uint8_t)(s_own << 4 | from), OP_REPORT_POWER, 0x00}};
        (void)tx_now(&r);
        break;
    }
    case OP_GET_MENU_LANG: {
        kvm_bridge_cec_frame_t r = {
            .len = 5, .data = {(uint8_t)(s_own << 4 | BROADCAST), OP_SET_MENU_LANG, 'e', 'n', 'g'}};
        (void)tx_now(&r);
        break;
    }
    case OP_ABORT:
        feature_abort(from, op, ABORT_REFUSED);
        break;
    /* Answers and announcements: nothing to say back. */
    case OP_FEATURE_ABORT:
    case OP_REPORT_PHYS_ADDR:
    case OP_VENDOR_ID:
    case OP_SET_OSD_NAME:
    case OP_REPORT_POWER:
    case OP_CEC_VERSION:
    case OP_ACTIVE_SOURCE:
    case OP_INACTIVE_SOURCE:
    case OP_IMAGE_VIEW_ON:
    case OP_TEXT_VIEW_ON:
    case OP_STANDBY:
    case OP_SET_MENU_LANG:
        break;
    default:
        feature_abort(from, op, ABORT_UNRECOGNIZED);
        break;
    }
}

static void drain_held(void)
{
    /* handle() may send, and a send may hold more frames: take a copy first. */
    while (s_held_n > 0) {
        kvm_bridge_cec_frame_t f = s_held[0];
        memmove(&s_held[0], &s_held[1], (size_t)(s_held_n - 1) * sizeof(s_held[0]));
        s_held_n--;
        handle(&f);
    }
}

/* ---------------------------------------------------------------- looking */

static void ask(uint8_t la)
{
    s_asked_ms[la] = now_ms();
    (void)send2(la, OP_GIVE_PHYS_ADDR);
    (void)send2(la, OP_GIVE_OSD_NAME);
    (void)send2(la, OP_GIVE_VENDOR_ID);
    (void)send2(la, OP_GIVE_POWER);
    (void)send2(la, OP_GET_CEC_VERSION);
}

static void scan(void)
{
    for (uint8_t la = 1; la < 15; la++) {
        if (la == s_own) {
            continue;
        }
        kvm_bridge_cec_frame_t poll = {.len = 1, .data = {(uint8_t)(s_own << 4 | la)}};
        const uint8_t r = tx_now(&poll);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        const bool was = s_dev[la].present;
        const bool known = s_dev[la].name[0] && s_dev[la].phys_addr != 0xffff;
        if (r == 0) {
            s_dev[la].present = true;
            s_dev[la].seen_ms = now_ms();
        } else if (r == 1) {
            forget(&s_dev[la]);
            if (s_active == la) {
                s_active = -1;
            }
        }
        xSemaphoreGive(s_mu);
        if (r == 0 && (!was || !known)) {
            ask(la);
        } else if (r == 0) {
            (void)send2(la, OP_GIVE_POWER);
        }
        drain_held();
    }
}

/* Take logical address 0, or 14 when a real TV already answers at 0. */
static esp_err_t claim(void)
{
    esp_err_t err = s_bridge->ops->cec_enable(s_bridge->dev, true, 15);
    if (err != ESP_OK) {
        return err;
    }
    s_own = 0;
    kvm_bridge_cec_frame_t self = {.len = 1, .data = {0x00}};
    if (tx_now(&self) == 0) {
        s_own = 14;
        ESP_LOGW(TAG, "a TV already holds logical address 0; using 14");
    }
    return s_bridge->ops->cec_enable(s_bridge->dev, true, s_own);
}

static void cec_task(void *arg)
{
    (void)arg;
    /* Capture brings the bridge up in its own time. */
    while (!(s_bridge = capture_bridge())) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!kvm_bridge_has_cec(s_bridge)) {
        kvm_cap_report(KVM_CAP_CEC, false, "the %s on this board has no CEC controller",
                       s_bridge->name);
        vTaskDelete(NULL);
        return;
    }
    kvm_cap_report(KVM_CAP_CEC, true, NULL);

    int64_t next_scan = 0;
    for (;;) {
        if (!kvm_setting_bool("cec_enable")) {
            if (s_running) {
                (void)s_bridge->ops->cec_enable(s_bridge->dev, false, 15);
                s_running = false;
                ESP_LOGI(TAG, "off");
            }
            tx_req_t req;
            while (xQueueReceive(s_txq, &req, 0) == pdTRUE) {
                if (req.w) {
                    req.w->result = 4;
                    xSemaphoreGive(req.w->done);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (!s_running || s_rescan || now >= next_scan) {
            /* Every round re-arms the controller too: a chip reset after a mode
             * change would otherwise leave CEC silently off. */
            if (claim() == ESP_OK) {
                if (!s_running) {
                    ESP_LOGI(TAG, "on, logical address %u", s_own);
                }
                s_running = true;
                s_rescan = false;
                scan();
            }
            next_scan = now + (int64_t)RESCAN_MS * 1000;
        }

        tx_req_t req;
        if (xQueueReceive(s_txq, &req, 0) == pdTRUE) {
            const uint8_t r = tx_now(&req.f);
            if (req.w) {
                req.w->result = r;
                xSemaphoreGive(req.w->done);
            }
        }
        kvm_bridge_cec_frame_t rx = {0};
        bool got = false;
        kvm_bridge_cec_tx_t v;
        s_bridge->ops->cec_poll(s_bridge->dev, &rx, &got, &v);
        if (got && s_held_n < HELD_MAX) {
            s_held[s_held_n++] = rx;
        }
        drain_held();
        for (uint8_t la = 1; s_ask && la < KVM_CEC_DEVICES; la++) {
            if (s_ask & (1u << la)) {
                s_ask &= (uint16_t)~(1u << la);
                ask(la);
                drain_held();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS) ? pdMS_TO_TICKS(POLL_MS) : 1);
    }
}

/* ---------------------------------------------------------------- the API */

void kvm_cec_init(void)
{
    if (s_txq) {
        return;
    }
    for (int i = 0; i < KVM_CEC_DEVICES; i++) {
        forget(&s_dev[i]);
    }
    s_mu = xSemaphoreCreateMutex();
    s_txq = xQueueCreate(8, sizeof(tx_req_t));
    xTaskCreate(cec_task, "cec", 4096, NULL, 3, NULL);
}

void kvm_cec_status(kvm_cec_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mu) {
        out->active = -1;
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    out->running = s_running;
    out->own_addr = s_own;
    out->active = s_active;
    memcpy(out->dev, s_dev, sizeof(s_dev));
    const unsigned n = s_log_n < KVM_CEC_LOG ? s_log_n : KVM_CEC_LOG;
    for (unsigned i = 0; i < n; i++) {
        out->log[i] = s_log[(s_log_n - n + i) % KVM_CEC_LOG];
    }
    out->log_count = (uint8_t)n;
    xSemaphoreGive(s_mu);
}

int kvm_cec_target(void)
{
    if (!s_mu) {
        return -1;
    }
    int t = -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_active >= 0 && s_active != s_own) {
        t = s_active;
    }
    static const uint8_t playback[] = {4, 8, 11, 9, 1, 2, 3, 5};
    for (size_t i = 0; t < 0 && i < sizeof(playback); i++) {
        if (s_dev[playback[i]].present) {
            t = playback[i];
        }
    }
    for (int la = 1; t < 0 && la < 15; la++) {
        if (s_dev[la].present && la != s_own) {
            t = la;
        }
    }
    xSemaphoreGive(s_mu);
    return t;
}

/* Queue a frame for the task and, when @p wait, block until the line answered. */
static esp_err_t queue_frame(const kvm_bridge_cec_frame_t *f, bool wait, uint8_t *result)
{
    if (!s_txq || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_req_t req = {.f = *f, .w = NULL};
    if (wait) {
        req.w = calloc(1, sizeof(*req.w));
        if (!req.w || !(req.w->done = xSemaphoreCreateBinary())) {
            free(req.w);
            return ESP_ERR_NO_MEM;
        }
    }
    if (xQueueSend(s_txq, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
        if (req.w) {
            vSemaphoreDelete(req.w->done);
            free(req.w);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (!wait) {
        return ESP_OK;
    }
    /* Callers are web handlers, which must not stall the server for long. */
    if (xSemaphoreTake(req.w->done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        if (result) {
            *result = 4;
        }
        return ESP_ERR_TIMEOUT; /* the task still owns req.w; a few bytes leak */
    }
    const uint8_t r = req.w->result;
    vSemaphoreDelete(req.w->done);
    free(req.w);
    if (result) {
        *result = r;
    }
    return r == 0 ? ESP_OK : ESP_FAIL;
}

static int pick(int la)
{
    return la < 0 ? kvm_cec_target() : la;
}

static esp_err_t send_to(int la, const uint8_t *body, int n, bool wait)
{
    la = pick(la);
    if (la < 0 || la > 15) {
        return ESP_ERR_NOT_FOUND;
    }
    kvm_bridge_cec_frame_t f = {.len = (uint8_t)(1 + n)};
    f.data[0] = (uint8_t)(s_own << 4 | la);
    memcpy(&f.data[1], body, n);
    return queue_frame(&f, wait, NULL);
}

esp_err_t kvm_cec_standby(int la)
{
    const uint8_t body[] = {OP_STANDBY};
    esp_err_t err = send_to(la, body, 1, false);
    if (err == ESP_OK && s_mu) {
        la = pick(la);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (la >= 0 && la < KVM_CEC_DEVICES) {
            s_dev[la].power = KVM_CEC_POWER_TO_STANDBY;
        }
        xSemaphoreGive(s_mu);
    }
    return err;
}

esp_err_t kvm_cec_wake(int la)
{
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    la = pick(la);
    if (la < 0 || la >= KVM_CEC_DEVICES) {
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t pa = 0xffff;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    pa = s_dev[la].phys_addr;
    xSemaphoreGive(s_mu);
    /* What a TV does when its input is chosen: route to the source by address. */
    if (pa != 0xffff) {
        const uint8_t route[] = {OP_SET_STREAM_PATH, (uint8_t)(pa >> 8), (uint8_t)pa};
        (void)send_to(BROADCAST, route, 3, false);
    }
    /* And the remote's Power On key, which many boxes obey from standby. */
    const uint8_t on[] = {OP_USER_PRESSED, 0x6d};
    const uint8_t up[] = {OP_USER_RELEASED};
    esp_err_t err = send_to(la, on, 2, false);
    (void)send_to(la, up, 1, false);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_dev[la].power = KVM_CEC_POWER_TO_ON;
        xSemaphoreGive(s_mu);
    }
    return err;
}

esp_err_t kvm_cec_key(int la, uint8_t code)
{
    const uint8_t press[] = {OP_USER_PRESSED, code};
    const uint8_t up[] = {OP_USER_RELEASED};
    /* Queued, not waited for: the result shows in the frame log. */
    esp_err_t err = send_to(la, press, 2, false);
    if (err == ESP_OK) {
        err = send_to(la, up, 1, false);
    }
    return err;
}

esp_err_t kvm_cec_send(const uint8_t *msg, int len, const char **result)
{
    static const char *words[] = {"ok", "nack", "arbitration lost", "error", "timeout"};
    if (len < 1 || len > 16) {
        *result = "bad length";
        return ESP_ERR_INVALID_ARG;
    }
    kvm_bridge_cec_frame_t f = {.len = (uint8_t)len};
    memcpy(f.data, msg, len);
    uint8_t r = 4;
    esp_err_t err = queue_frame(&f, true, &r);
    *result = err == ESP_ERR_INVALID_STATE ? "CEC is off" : words[r < 5 ? r : 4];
    return err;
}

bool kvm_cec_source_id(char *name, int name_len, uint32_t *vendor)
{
    name[0] = '\0';
    *vendor = 0xffffffff;
    const int la = s_running ? kvm_cec_target() : -1;
    if (la < 0) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(name, name_len, "%s", s_dev[la].name);
    *vendor = s_dev[la].vendor;
    xSemaphoreGive(s_mu);
    return true;
}

bool kvm_cec_source(char *name, int name_len, const char **power)
{
    *power = "unknown";
    name[0] = '\0';
    const int la = s_running ? kvm_cec_target() : -1;
    if (la < 0) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const kvm_cec_device_t *d = &s_dev[la];
    if (d->name[0]) {
        snprintf(name, name_len, "%s", d->name);
    } else {
        snprintf(name, name_len, "logical address %d", la);
    }
    switch (d->power) {
    case KVM_CEC_POWER_ON:
    case KVM_CEC_POWER_TO_ON:
        *power = "on";
        break;
    case KVM_CEC_POWER_STANDBY:
    case KVM_CEC_POWER_TO_STANDBY:
        *power = "standby";
        break;
    default:
        break;
    }
    xSemaphoreGive(s_mu);
    return true;
}

void kvm_cec_rescan(void)
{
    s_rescan = true;
}

static const struct {
    const char *name;
    uint8_t code;
} s_keys[] = {
    {"select", 0x00}, {"ok", 0x00},       {"up", 0x01},        {"down", 0x02},
    {"left", 0x03},   {"right", 0x04},    {"home", 0x09},      {"menu", 0x0a},
    {"contents", 0x0b}, {"favorites", 0x0c}, {"back", 0x0d},  {"exit", 0x0d},
    {"0", 0x20},      {"1", 0x21},        {"2", 0x22},         {"3", 0x23},
    {"4", 0x24},      {"5", 0x25},        {"6", 0x26},         {"7", 0x27},
    {"8", 0x28},      {"9", 0x29},        {"enter", 0x2b},     {"clear", 0x2c},
    {"channel_up", 0x30}, {"channel_down", 0x31}, {"info", 0x35}, {"guide", 0x53},
    {"power", 0x40},  {"volume_up", 0x41}, {"volume_down", 0x42}, {"mute", 0x43},
    {"play", 0x44},   {"stop", 0x45},     {"pause", 0x46},     {"record", 0x47},
    {"rewind", 0x48}, {"fast_forward", 0x49}, {"eject", 0x4a}, {"next", 0x4b},
    {"previous", 0x4c}, {"power_toggle", 0x6b}, {"power_off", 0x6c}, {"power_on", 0x6d},
    {"blue", 0x71},   {"red", 0x72},      {"green", 0x73},     {"yellow", 0x74},
};

bool kvm_cec_key_from_name(const char *name, uint8_t *code)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        if (strcasecmp(name, s_keys[i].name) == 0) {
            *code = s_keys[i].code;
            return true;
        }
    }
    return false;
}
