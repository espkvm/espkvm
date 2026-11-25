/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_KVM_ENABLE_USB_VIDEO
#include "usb_video.h"
#include "usb_video_h264.h"
#include "kvm_caps.h"
#include "usb_video_info.h"
#include "usb_video_transport.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_settings.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sodium.h"
#include "tusb.h"

enum { RESETTING, READY, ACTIVE, FAULTED };
enum { SESSION = 0, VIDEO = 1, HELLO = 1, READY_MSG = 2, CLOSE = 4, POLICY = 5, INFO = 6 };
enum { CAPABILITIES = 1, CONFIG = 3, STATUS = 4, FRAME = 5, KEYFRAME = 6 };

#define TOKEN_LEN 16
#define HEADER_LEN 8
#define FRAME_HEADER_LEN 12
#define HELLO_LEN 52
#define HELLO_WIRE_LEN (HEADER_LEN + HELLO_LEN)
#define HANDSHAKE_US 15000000
#define ACTIVE_TIMEOUT_US 8000000
#define KEYFRAME_INTERVAL_US 600000
#define CONSOLE_STATE_HEADER 31
#define POLICY_PREFIX "{\"terminal_enabled\":false,\"files_enabled\":false," \
                      "\"scale_capture_to_config\":true,\"capture_display\":\""
#define POLICY_BUFFER_LEN (sizeof(POLICY_PREFIX) + DISPLAY_NAME_MAX * 6 + 2)

_Static_assert(USB_VIDEO_TX_FIFO_SIZE >= HEADER_LEN + POLICY_BUFFER_LEN,
               "The display policy must fit in USB IN");
_Static_assert(INFO_SIZE_MAX < USB_VIDEO_FRAME_CAP, "The display report needs room for a terminator");

static const char *TAG = "usb_video";
static portMUX_TYPE control_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t iface, phase = FAULTED, token[TOKEN_LEN];
static uint32_t generation;
static uint8_t control_reply[24];
static uint8_t public_key[crypto_sign_ed25519_PUBLICKEYBYTES];
static uint8_t secret_key[crypto_sign_ed25519_SECRETKEYBYTES];
static char fingerprint[24];
static atomic_bool initialized;
static void (*set_interface_visible)(bool);
static atomic_bool stop_requested;
static bool transport_ready;
static SemaphoreHandle_t frames_lock;
static SemaphoreHandle_t frame_pending;
static SemaphoreHandle_t usb_io_done;
static TaskHandle_t receiver;
static struct {
    usb_video_status_t status;
    uint8_t *data;
    size_t len;
    bool keyframe;
    bool need_keyframe;
} mailbox;
static struct {
    char selected[DISPLAY_NAME_MAX + 1];
    uint8_t *displays;
    size_t displays_size;
    uint32_t revision, policy_revision;
    bool enabled, connected, oversized;
} console = {.revision = 1};

/* The vendor FIFO helpers can rearm endpoints. Keep those operations on
 * the same task as the DWC2 completion and reset events. */
typedef enum { USB_IO_ATTACH, USB_IO_DETACH, USB_IO_READ, USB_IO_WRITE, USB_IO_CLEAR } usb_io_operation_t;
static struct {
    usb_io_operation_t operation;
    uint8_t rx_fifo[USB_VIDEO_RX_FIFO_SIZE];
    uint8_t tx_fifo[USB_VIDEO_TX_FIFO_SIZE];
    uint8_t input[2048];
    uint8_t output[HEADER_LEN + POLICY_BUFFER_LEN];
    size_t size;
    size_t count;
    bool busy, attached;
} *usb_io;

typedef struct {
    uint32_t generation;
    uint8_t token[TOKEN_LEN];
    uint8_t header[HELLO_WIRE_LEN];
    size_t header_len;
    size_t scanned;
    uint8_t *payload;
    size_t size, used;
    uint16_t service, kind;
    uint32_t max_width, max_height, max_fps;
    uint32_t granted_width, granted_height, granted_fps;
    uint32_t policy_revision;
    int64_t deadline, next_keyframe;
    bool active, configured, discard, enabled;
} session_t;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void put16(uint8_t *p, uint16_t n)
{
    p[0] = n;
    p[1] = n >> 8;
}

static void put32(uint8_t *p, uint32_t n)
{
    put16(p, n);
    put16(p + 2, n >> 16);
}

static bool store_identity(void)
{
    nvs_handle_t nvs;
    if (sodium_init() < 0 || nvs_open("usb_video", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t seed[crypto_sign_ed25519_SEEDBYTES];
    size_t size = sizeof(seed);
    esp_err_t result = nvs_get_blob(nvs, "key", seed, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(seed, sizeof(seed));
        result = nvs_set_blob(nvs, "key", seed, sizeof(seed));
        if (result == ESP_OK) {
            result = nvs_commit(nvs);
        }
    }
    nvs_close(nvs);
    if (result != ESP_OK || size != sizeof(seed)) {
        sodium_memzero(seed, sizeof(seed));
        return false;
    }
    const int key_result = crypto_sign_ed25519_seed_keypair(public_key, secret_key, seed);
    sodium_memzero(seed, sizeof(seed));
    if (key_result != 0) {
        return false;
    }
    uint8_t digest[32];
    size_t len = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, public_key, sizeof(public_key),
                         digest, sizeof(digest), &len) != PSA_SUCCESS || len != sizeof(digest)) {
        return false;
    }
    snprintf(fingerprint, sizeof(fingerprint), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7]);
    ESP_LOGI(TAG, "Pairing fingerprint %s", fingerprint);
    return true;
}

static bool set_phase(uint32_t expected, uint8_t value)
{
    portENTER_CRITICAL(&control_lock);
    bool unchanged = generation == expected;
    if (unchanged) {
        phase = value;
    }
    portEXIT_CRITICAL(&control_lock);
    return unchanged;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                               const tusb_control_request_t *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (!initialized || request->bmRequestType != 0xc1 || request->wIndex != iface ||
        request->wValue != 5 || request->wLength != sizeof(control_reply) ||
        (request->bRequest != 0x40 && request->bRequest != 0x41)) {
        return false;
    }
    uint8_t next[TOKEN_LEN];
    if (request->bRequest == 0x40) {
        esp_fill_random(next, sizeof(next));
    }
    portENTER_CRITICAL(&control_lock);
    if (!transport_ready || !usb_video_transport_mounted() || stop_requested) {
        portEXIT_CRITICAL(&control_lock);
        return false;
    }
    if (request->bRequest == 0x40) {
        generation++;
        memcpy(token, next, sizeof(token));
        phase = RESETTING;
    }
    memcpy(control_reply, "JKV5", 4);
    control_reply[4] = phase;
    memcpy(control_reply + 8, token, sizeof(token));
    TaskHandle_t task = receiver;
    portEXIT_CRITICAL(&control_lock);
    /* The USB task must process DETACH before this handle can be retired. */
    xTaskNotifyGive(task);
    return tud_control_xfer(rhport, request, control_reply, sizeof(control_reply));
}

void usb_video_transport_event(bool reset)
{
    portENTER_CRITICAL(&control_lock);
    if (reset) {
        generation++;
        phase = FAULTED;
    }
    TaskHandle_t task = transport_ready ? receiver : NULL;
    portEXIT_CRITICAL(&control_lock);
    if (task) {
        xTaskNotifyGive(task);
    }
}

static void usb_io_run(void *unused)
{
    (void)unused;
    usb_io->count = 0;
    switch (usb_io->operation) {
    case USB_IO_ATTACH:
        usb_video_transport_attach(usb_io->rx_fifo, usb_io->tx_fifo);
        usb_io->attached = true;
        portENTER_CRITICAL(&control_lock);
        transport_ready = true;
        portEXIT_CRITICAL(&control_lock);
        break;
    case USB_IO_DETACH:
        portENTER_CRITICAL(&control_lock);
        transport_ready = false;
        portEXIT_CRITICAL(&control_lock);
        usb_video_transport_detach();
        usb_io->attached = false;
        break;
    case USB_IO_READ:
        usb_io->count = usb_video_transport_read(usb_io->input, sizeof(usb_io->input));
        break;
    case USB_IO_WRITE:
        usb_io->count = usb_video_transport_write(usb_io->output, usb_io->size);
        break;
    case USB_IO_CLEAR:
        usb_video_transport_reset_session();
        break;
    }
    xSemaphoreGive(usb_io_done);
}

static bool usb_io_ready(void)
{
    if (usb_io->busy && xSemaphoreTake(usb_io_done, 0) != pdTRUE) {
        return false;
    }
    usb_io->busy = false;
    return true;
}

static bool usb_io_call(usb_io_operation_t operation)
{
    if (!usb_io_ready() || !tud_inited()) {
        return false;
    }
    usb_io->operation = operation;
    usb_io->busy = true;
    usbd_defer_func(usb_io_run, NULL, false);
    if (xSemaphoreTake(usb_io_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    usb_io->busy = false;
    return true;
}

static void usb_io_retire(void)
{
    /* Even a timed-out request can still be queued in TinyUSB. Wait for its
     * acknowledgement before detaching the FIFOs or releasing their owner. */
    if (usb_io->busy) {
        xSemaphoreTake(usb_io_done, portMAX_DELAY);
        usb_io->busy = false;
    }
    if (usb_io->attached) {
        usb_io->operation = USB_IO_DETACH;
        usbd_defer_func(usb_io_run, NULL, false);
        xSemaphoreTake(usb_io_done, portMAX_DELAY);
    }
}

static bool send_record(uint16_t service, uint16_t kind, const void *p, uint32_t len)
{
    uint8_t *message = usb_io->output;
    if (len > sizeof(usb_io->output) - HEADER_LEN || !usb_io_ready()) {
        return false;
    }
    put16(message, service);
    put16(message + 2, kind);
    put32(message + 4, len);
    if (len && p != message + HEADER_LEN) {
        memcpy(message + HEADER_LEN, p, len);
    }
    usb_io->size = len + HEADER_LEN;
    return usb_io_call(USB_IO_WRITE) && usb_io->count == usb_io->size;
}

static void clear_video(bool keep_source)
{
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    uint32_t sequence = mailbox.status.sequence + 1;
    bool playing = keep_source && mailbox.status.playing;
    mailbox.status = (usb_video_status_t){.sequence = sequence, .playing = playing};
    mailbox.len = 0;
    mailbox.need_keyframe = true;
    xSemaphoreGive(frames_lock);
    xSemaphoreGive(frame_pending);
}

static bool reserve_payloads(session_t *s)
{
    if (s->payload) {
        return true;
    }
    const size_t bytes = USB_VIDEO_FRAME_CAP + FRAME_HEADER_LEN;
    s->payload = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    mailbox.data = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s->payload && mailbox.data) {
        return true;
    }
    free(s->payload);
    free(mailbox.data);
    s->payload = NULL;
    mailbox.data = NULL;
    return false;
}

static void release_payloads(session_t *s)
{
    /* clear_video has retired the last access unit before releasing storage. */
    free(s->payload);
    free(mailbox.data);
    s->payload = NULL;
    mailbox.data = NULL;
}

static bool send_policy(const char *name)
{
    if (!usb_io_ready()) {
        return false;
    }
    char *const body = (char *)usb_io->output + HEADER_LEN;
    char *p = stpcpy(body, POLICY_PREFIX);
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        if (*c < 32) {
            p += snprintf(p, 7, "\\u%04x", *c);
        } else {
            if (*c == '"' || *c == '\\') {
                *p++ = '\\';
            }
            *p++ = (char)*c;
        }
    }
    p = stpcpy(p, "\"}");
    return send_record(SESSION, POLICY, body, p - body);
}

static void console_connected(bool connected)
{
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    if (console.connected != connected || console.displays_size || console.oversized) {
        console.connected = connected;
        heap_caps_free(console.displays);
        console.displays = NULL;
        console.displays_size = 0;
        console.oversized = false;
        console.revision++;
    }
    xSemaphoreGive(frames_lock);
}

static bool authorize(session_t *s)
{
    const uint8_t *hello = s->header + HEADER_LEN;
    static const char domain[] = "IP-KVM OS Services session.ready v5";
    uint8_t ready[116] = {5, 0, 0, 0};
    uint8_t transcript[sizeof(domain) - 1 + HELLO_LEN + 52];
    memcpy(ready + 4, s->token, TOKEN_LEN);
    memcpy(ready + 20, public_key, sizeof(public_key));
    memcpy(transcript, domain, sizeof(domain) - 1);
    memcpy(transcript + sizeof(domain) - 1, hello, HELLO_LEN);
    memcpy(transcript + sizeof(domain) - 1 + HELLO_LEN, ready, 52);
    if (crypto_sign_ed25519_detached(ready + 52, NULL, transcript, sizeof(transcript), secret_key) != 0 ||
        !send_record(SESSION, READY_MSG, ready, sizeof(ready))) {
        return false;
    }
    return set_phase(s->generation, ACTIVE);
}

static bool seek_hello(session_t *s, uint8_t byte)
{
    const uint8_t prefix[12] = {0, 0, HELLO, 0, HELLO_LEN, 0, 0, 0, 5, 0, 0, 0};
    if (++s->scanned > 8u * 1024u * 1024u + HELLO_WIRE_LEN) {
        return false;
    }
    if (s->header_len == sizeof(s->header)) {
        memmove(s->header, s->header + 1, sizeof(s->header) - 1);
        s->header_len--;
    }
    s->header[s->header_len++] = byte;
    if (s->header_len != sizeof(s->header) || memcmp(s->header, prefix, sizeof(prefix)) ||
        memcmp(s->header + sizeof(prefix), s->token, TOKEN_LEN)) {
        return true;
    }
    if (!authorize(s)) {
        return false;
    }
    s->active = true;
    s->deadline = esp_timer_get_time() + ACTIVE_TIMEOUT_US;
    s->header_len = 0;
    console_connected(true);
    ESP_LOGI(TAG, "OS Services session ready");
    return true;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static bool send_video_config(session_t *s)
{
    const int fps_limit = kvm_setting_int("vid_fps_max");
    uint8_t config[20] = {s->enabled};
    s->granted_width = min_u32(1920, s->max_width);
    s->granted_height = min_u32(1200, s->max_height);
    s->granted_fps = min_u32(s->max_fps, fps_limit > 0 ? min_u32(fps_limit, 30) : 30);
    put32(config + 4, s->granted_width);
    put32(config + 8, s->granted_height);
    put32(config + 12, 4000);
    put32(config + 16, s->granted_fps);
    clear_video(s->enabled);
    if (!send_record(VIDEO, CONFIG, config, sizeof(config))) {
        return false;
    }
    s->configured = s->enabled;
    return true;
}

static bool configure(session_t *s, const uint8_t *p)
{
    if (s->size != 16) {
        return false;
    }
    const uint32_t flags = get32(p);
    s->max_width = get32(p + 4);
    s->max_height = get32(p + 8);
    s->max_fps = get32(p + 12);
    if (!(flags | s->max_width | s->max_height | s->max_fps)) {
        s->configured = false;
        clear_video(false);
        return true;
    }
    if (!(flags & 1) || (flags & ~3u) || s->max_width < 2 || s->max_width > 16384 ||
        s->max_height < 2 || s->max_height > 16384 || !s->max_fps || s->max_fps > 240) {
        return false;
    }
    return send_video_config(s);
}

static void report_size_limit(bool oversized)
{
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    if (console.oversized != oversized) {
        console.oversized = oversized;
        console.revision++;
    }
    xSemaphoreGive(frames_lock);
}

static bool apply_status(session_t *s, const uint8_t *p)
{
    if (s->size < 32 || p[0] > 1 || p[1] > 1 || p[2] != 0 || p[3] > 1) {
        return false;
    }
    if (!p[0]) {
        clear_video(false);
        return true;
    }
    uint32_t width = get32(p + 4);
    uint32_t height = get32(p + 8);
    uint32_t native_fps = get32(p + 12);
    uint32_t maximum_fps = get32(p + 24);
    const uint32_t output_fps = min_u32(native_fps, maximum_fps ? maximum_fps : s->granted_fps);
    if (!width || !height || !native_fps || native_fps > 240 || maximum_fps > 240) {
        return false;
    }
    const bool oversized = width > s->granted_width || height > s->granted_height;
    report_size_limit(oversized);
    if (oversized) {
        clear_video(false);
        return true;
    }
    if (output_fps > s->granted_fps) {
        clear_video(false);
        return true;
    }
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    const bool reset = p[3] || width != mailbox.status.width || height != mailbox.status.height;
    xSemaphoreGive(frames_lock);
    if (reset) {
        clear_video(true);
    }
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    mailbox.status.width = width;
    mailbox.status.height = height;
    mailbox.status.fps = output_fps;
    xSemaphoreGive(frames_lock);
    ESP_LOGI(TAG, "USB video accepted %" PRIu32 "x%" PRIu32, width, height);
    return true;
}

static void receive_frame(session_t *s)
{
    const uint32_t flags = get32(s->payload + 8);
    const bool keyframe = flags == 3;
    if (!usb_video_h264_access_unit(s->payload + FRAME_HEADER_LEN, s->size - FRAME_HEADER_LEN, flags,
                                    s->granted_width, s->granted_height)) {
        usb_video_request_keyframe();
        return;
    }
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    if (mailbox.status.width && (keyframe || (!mailbox.need_keyframe && !mailbox.len))) {
        uint8_t *old = mailbox.data;
        mailbox.data = s->payload;
        s->payload = old;
        mailbox.keyframe = keyframe;
        mailbox.len = s->size - FRAME_HEADER_LEN;
        mailbox.need_keyframe = false;
        mailbox.status.frames++;
        mailbox.status.last_frame_us = esp_timer_get_time();
        mailbox.status.playing = true;
        xSemaphoreGive(frame_pending);
    } else if (mailbox.len) {
        mailbox.need_keyframe = true;
    }
    xSemaphoreGive(frames_lock);
}

static void receive_displays(session_t *s)
{
    size_t size;
    if (!usb_video_parse_displays(s->payload, s->size, &size)) {
        return;
    }

    xSemaphoreTake(frames_lock, portMAX_DELAY);
    if (size != console.displays_size || (size && memcmp(s->payload, console.displays, size))) {
        uint8_t *copy = size ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : NULL;
        if (!size || copy) {
            if (size) {
                memcpy(copy, s->payload, size);
            }
            heap_caps_free(console.displays);
            console.displays = copy;
            console.displays_size = size;
            console.revision++;
        }
    }
    xSemaphoreGive(frames_lock);
}

static bool complete(session_t *s)
{
    if (s->service == SESSION) {
        if (s->kind == INFO) {
            receive_displays(s);
        }
        return s->kind != HELLO && s->kind != CLOSE;
    }
    if (s->service != VIDEO) {
        return true;
    }
    if (s->kind == CAPABILITIES) {
        return configure(s, s->payload);
    }
    if (!s->configured) {
        return true;
    }
    if (s->kind == STATUS) {
        return apply_status(s, s->payload);
    }
    if (s->kind == FRAME && s->size > FRAME_HEADER_LEN) {
        if (s->discard) {
            usb_video_request_keyframe();
        } else {
            receive_frame(s);
        }
    }
    return true;
}

static bool receive_record(session_t *s, const uint8_t **data, size_t *len)
{
    if (s->header_len < HEADER_LEN) {
        size_t n = min_u32(*len, HEADER_LEN - s->header_len);
        memcpy(s->header + s->header_len, *data, n);
        s->header_len += n;
        *data += n;
        *len -= n;
        if (s->header_len < HEADER_LEN) {
            return true;
        }
        s->service = get16(s->header);
        s->kind = get16(s->header + 2);
        s->size = get32(s->header + 4);
        const bool frame = s->service == VIDEO && s->kind == FRAME;
        if (!s->kind || s->size > (frame ? 8u * 1024u * 1024u : 64u * 1024u)) {
            return false;
        }
        s->discard = s->size > USB_VIDEO_FRAME_CAP + FRAME_HEADER_LEN;
    }
    size_t n = min_u32(*len, s->size - s->used);
    if (!s->discard) {
        memcpy(s->payload + s->used, *data, n);
    }
    s->used += n;
    *data += n;
    *len -= n;
    if (s->used != s->size) {
        return true;
    }
    const bool ok = complete(s);
    s->header_len = s->used = 0;
    return ok;
}

static void request_if_due(session_t *s, int64_t now)
{
    if (s->configured && now >= s->next_keyframe) {
        xSemaphoreTake(frames_lock, portMAX_DELAY);
        const bool wanted = mailbox.need_keyframe && mailbox.status.width;
        xSemaphoreGive(frames_lock);
        if (wanted && send_record(VIDEO, KEYFRAME, NULL, 0)) {
            s->next_keyframe = now + KEYFRAME_INTERVAL_US;
        }
    }
}

static bool sync_controls(session_t *s)
{
    char name[DISPLAY_NAME_MAX + 1];
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    uint32_t revision = console.policy_revision;
    const bool changed = s->policy_revision != revision;
    const bool enabled = console.enabled;
    if (changed) {
        memcpy(name, console.selected, sizeof(name));
    }
    xSemaphoreGive(frames_lock);
    if (!changed) {
        return true;
    }

    if (!send_policy(name)) {
        return false;
    }
    s->policy_revision = revision;
    const bool toggled = enabled != s->enabled;
    s->enabled = enabled;
    if (toggled && !enabled) {
        report_size_limit(false);
    }
    return !toggled || !s->max_width || send_video_config(s);
}

static uint8_t sync_session(session_t *s)
{
    uint8_t state;
    portENTER_CRITICAL(&control_lock);
    const uint32_t current = generation;
    state = phase;
    const bool changed = s->generation != current;
    if (changed) {
        uint8_t *buffer = s->payload;
        *s = (session_t){.payload = buffer, .generation = current, .policy_revision = UINT32_MAX};
        memcpy(s->token, token, sizeof(token));
    }
    portEXIT_CRITICAL(&control_lock);
    if (changed) {
        clear_video(false);
        console_connected(false);
    }
    if (state == RESETTING) {
        if (!reserve_payloads(s)) {
            set_phase(s->generation, FAULTED);
            return FAULTED;
        }
        if (!usb_io_call(USB_IO_CLEAR)) {
            set_phase(s->generation, FAULTED);
            return FAULTED;
        }
        s->deadline = esp_timer_get_time() + HANDSHAKE_US;
        state = set_phase(s->generation, READY) ? READY : FAULTED;
    } else if (state == FAULTED && s->deadline) {
        clear_video(false);
        s->deadline = 0;
    }
    if (state == FAULTED && s->payload) {
        release_payloads(s);
    }
    return state;
}

static void receive_session(session_t *s)
{
    while (!stop_requested) {
        const uint8_t state = sync_session(s);
        if (state == READY || state == ACTIVE) {
            const int64_t now = esp_timer_get_time();
            const bool mounted = usb_video_transport_mounted();
            const bool pending = mounted && usb_video_transport_pending();
            const bool read_ok = !pending || usb_io_call(USB_IO_READ);
            size_t len = pending && read_ok ? usb_io->count : 0;
            if (len && s->active) {
                s->deadline = now + ACTIVE_TIMEOUT_US;
            }
            bool ok = mounted && read_ok && now < s->deadline;
            const uint8_t *p = usb_io->input;
            while (ok && len) {
                if (s->active) {
                    ok = receive_record(s, &p, &len);
                } else {
                    ok = seek_hello(s, *p++);
                    len--;
                }
            }
            if (ok && s->active) {
                ok = sync_controls(s);
            }
            if (!ok) {
                ESP_LOGI(TAG, "Session ended (%s)", !mounted ? "USB unmounted" :
                         !read_ok ? "USB task did not respond" :
                         now >= s->deadline ? "input timeout" : "invalid record or peer close");
                clear_video(false);
                console_connected(false);
                set_phase(s->generation, FAULTED);
            } else {
                request_if_due(s, now);
                if (usb_video_transport_pending()) {
                    continue;
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}

static void receive_loop(void *arg)
{
    (void)arg;
    /* The creator publishes the task handle while holding this lock. */
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    xSemaphoreGive(frames_lock);
    for (;;) {
        session_t session = {0};
        if (!stop_requested) {
            if (usb_io_call(USB_IO_ATTACH)) {
                receive_session(&session);
            } else {
                ESP_LOGW(TAG, "USB task did not start screen sharing");
                xSemaphoreTake(frames_lock, portMAX_DELAY);
                console.enabled = false;
                console.revision++;
                set_interface_visible(false);
                xSemaphoreGive(frames_lock);
            }
        }
        clear_video(false);
        console_connected(false);
        release_payloads(&session);
        usb_io_retire();

        xSemaphoreTake(frames_lock, portMAX_DELAY);
        if (console.enabled) {
            /* An enable arrived while the previous session was retiring. */
            stop_requested = false;
            xSemaphoreGive(frames_lock);
            continue;
        }
        portENTER_CRITICAL(&control_lock);
        receiver = NULL;
        portEXIT_CRITICAL(&control_lock);
        vSemaphoreDelete(usb_io_done);
        free(usb_io);
        usb_io = NULL;
        xSemaphoreGive(frames_lock);
        vTaskDelete(NULL);
        return;
    }
}

bool usb_video_init(uint8_t interface_number, void (*set_visible)(bool))
{
    if (initialized) {
        return iface == interface_number;
    }
    iface = interface_number;
    if (!store_identity()) {
        kvm_cap_report(KVM_CAP_USB_VIDEO, false, "USB video identity unavailable");
        return false;
    }
    frames_lock = xSemaphoreCreateMutex();
    frame_pending = xSemaphoreCreateBinary();
    if (frames_lock && frame_pending) {
        set_interface_visible = set_visible;
        initialized = true;
        kvm_cap_report(KVM_CAP_USB_VIDEO, true, NULL);
        return true;
    }
    if (frames_lock) {
        vSemaphoreDelete(frames_lock);
        frames_lock = NULL;
    }
    if (frame_pending) {
        vSemaphoreDelete(frame_pending);
        frame_pending = NULL;
    }
    kvm_cap_report(KVM_CAP_USB_VIDEO, false, "not enough memory to initialize USB video");
    return false;
}

bool usb_video_status(usb_video_status_t *out)
{
    if (!initialized) {
        return false;
    }
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    *out = mailbox.status;
    xSemaphoreGive(frames_lock);
    return true;
}

static bool start_receiver(void)
{
    usb_io = heap_caps_calloc(1, sizeof(*usb_io), MALLOC_CAP_SPIRAM);
    if (!usb_io) {
        return false;
    }
    usb_io_done = xSemaphoreCreateBinary();
    TaskHandle_t task;
    stop_requested = false;
    if (usb_io_done && xTaskCreate(receive_loop, "usb-video", 6144, NULL, 5, &task) == pdPASS) {
        portENTER_CRITICAL(&control_lock);
        receiver = task;
        portEXIT_CRITICAL(&control_lock);
        return true;
    }
    if (usb_io_done) {
        vSemaphoreDelete(usb_io_done);
    }
    free(usb_io);
    usb_io = NULL;
    return false;
}

/* frames_lock serializes requests with allocation and worker retirement. */
static bool set_enabled(bool enabled)
{
    if (console.enabled == enabled) {
        return false;
    }
    if (enabled && !receiver && !start_receiver()) {
        ESP_LOGW(TAG, "Out of memory starting screen sharing");
        return false;
    }
    console.enabled = enabled;
    if (!enabled) {
        portENTER_CRITICAL(&control_lock);
        stop_requested = true;
        generation++;
        phase = FAULTED;
        portEXIT_CRITICAL(&control_lock);
        xTaskNotifyGive(receiver);
    }
    set_interface_visible(enabled);
    return true;
}

void usb_video_console_set(bool enabled, uint8_t display, uint32_t revision)
{
    if (!initialized) {
        return;
    }
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    if (display == UINT8_MAX) {
        if (set_enabled(enabled)) {
            console.revision++;
        }
    } else if (revision == console.revision && display <= DISPLAY_COUNT_MAX) {
        const char *name = "";
        size_t len = 0;
        size_t pos = 0;
        for (; display && pos < console.displays_size; display--) {
            len = get16(console.displays + pos);
            name = (const char *)console.displays + pos + 2;
            pos += len + 2;
        }
        if (!display) {
            const bool toggled = set_enabled(enabled);
            const bool changed = strlen(console.selected) != len || memcmp(console.selected, name, len);
            if (changed) {
                memcpy(console.selected, name, len);
                console.selected[len] = '\0';
                console.policy_revision++;
                if (receiver) {
                    xTaskNotifyGive(receiver);
                }
            }
            if (toggled || changed) {
                console.revision++;
            }
        }
    }
    xSemaphoreGive(frames_lock);
}

uint8_t *usb_video_console_state(uint8_t opcode, uint32_t known_revision, size_t *length)
{
    if (!initialized) {
        return NULL;
    }
    uint8_t *packet = NULL;
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    if (known_revision != console.revision) {
        const size_t name_size = strlen(console.selected);
        const size_t size = CONSOLE_STATE_HEADER + name_size + console.displays_size;
        packet = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (packet) {
            packet[0] = opcode;
            put32(packet + 1, console.revision);
            packet[5] = (console.enabled ? 1 : 0) | (console.connected ? 2 : 0) |
                        (console.oversized ? 4 : 0);
            memcpy(packet + 6, fingerprint, sizeof(fingerprint) - 1);
            put16(packet + 29, name_size);
            memcpy(packet + CONSOLE_STATE_HEADER, console.selected, name_size);
            if (console.displays_size) {
                memcpy(packet + CONSOLE_STATE_HEADER + name_size, console.displays, console.displays_size);
            }
            *length = size;
        }
    }
    xSemaphoreGive(frames_lock);
    return packet;
}

bool usb_video_take(uint32_t sequence, uint8_t *out, size_t capacity,
                    size_t *length, bool *keyframe)
{
    xSemaphoreTake(frames_lock, portMAX_DELAY);
    const bool ready = mailbox.len && mailbox.len <= capacity &&
                       mailbox.status.sequence == sequence;
    if (ready) {
        *length = mailbox.len;
        *keyframe = mailbox.keyframe;
        memcpy(out, mailbox.data + FRAME_HEADER_LEN, mailbox.len);
        mailbox.len = 0;
    }
    xSemaphoreGive(frames_lock);
    return ready;
}

void usb_video_wait(void)
{
    xSemaphoreTake(frame_pending, pdMS_TO_TICKS(50));
}

void usb_video_request_keyframe(void)
{
    if (initialized) {
        xSemaphoreTake(frames_lock, portMAX_DELAY);
        mailbox.need_keyframe = true;
        xSemaphoreGive(frames_lock);
    }
}
#endif
