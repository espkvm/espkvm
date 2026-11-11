/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI audio to the browser and to scripts:
 *
 *   WS  /audio                        0x01 subscribe. The device then sends one
 *                                     Opus packet a message (20 ms, 48 kHz
 *                                     stereo): byte 0 is 0x03, byte 1 is 1 on
 *                                     the first packet after a gap, bytes 2-5
 *                                     the 48 kHz timestamp (LE), then the
 *                                     packet. For a console that gets its
 *                                     picture over the WebSocket; over WebRTC
 *                                     the sound rides with the video.
 *   GET /api/v1/audio                 receiver state, rate, level
 *   GET /api/v1/audio/clip.wav?s=N    the next N seconds (1..10) as a WAV file
 *
 * A pump task sends to the sockets, so a slow browser holds up nothing else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "capture.h"
#include "kvm_audio.h"
#include "kvm_auth.h"
#include "web_priv.h"

static const char *TAG = "audio_api";

#define WS_SUBSCRIBE 0x01
#define WS_OPUS 0x03
#define WS_HDR 6
#define WS_MAX_CLIENTS 4
#define WS_DEPTH 10 /* 200 ms */
#define CLIP_MAX_S 10
#define CLIP_STACK 4096
#define CLIP_PRIO 4

static esp_err_t audio_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_audio_status_t st;
    kvm_audio_status(&st);
    kvm_video_status_t vs;
    capture_status_get(&vs);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "running", st.running);
    cJSON_AddNumberToObject(root, "sourceHz", vs.audio_hz);
    cJSON_AddNumberToObject(root, "rate", st.rate);
    cJSON_AddNumberToObject(root, "bclk", st.bclk);
    cJSON_AddNumberToObject(root, "ws", st.ws);
    cJSON_AddNumberToObject(root, "din", st.din);
    cJSON_AddNumberToObject(root, "readers", st.readers);
    cJSON_AddNumberToObject(root, "frames", (double)st.frames);
    cJSON_AddNumberToObject(root, "overruns", st.overruns);
    cJSON_AddNumberToObject(root, "restarts", st.restarts);
    cJSON_AddNumberToObject(root, "isrGapMaxUs", st.isr_gap_max_us);
    cJSON_AddNumberToObject(root, "readGapMaxUs", st.read_gap_max_us);
    cJSON_AddNumberToObject(root, "peak", st.peak);
    cJSON_AddNumberToObject(root, "rms", st.rms);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

typedef struct {
    httpd_req_t *req;
    kvm_audio_reader_t *reader;
    unsigned seconds;
} clip_t;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void wav_header(uint8_t *h, uint32_t rate, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1); /* PCM */
    put16(h + 22, KVM_AUDIO_CHANNELS);
    put32(h + 24, rate);
    put32(h + 28, rate * KVM_AUDIO_CHANNELS * 2);
    put16(h + 32, KVM_AUDIO_CHANNELS * 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

/* Collects the clip in PSRAM, then sends it in one piece with its length. */
static void clip_task(void *arg)
{
    clip_t *c = arg;
    kvm_audio_block_t *b = heap_caps_malloc(sizeof(*b), MALLOC_CAP_SPIRAM);
    uint8_t *wav = NULL;
    size_t used = 44;
    size_t cap = 0;
    uint32_t rate = 0;
    const char *fail = b ? NULL : "no memory";

    while (!fail) {
        if (kvm_audio_read(c->reader, b, rate ? 200 : 1500) != ESP_OK) {
            if (!rate) {
                fail = "no sound from the source";
            }
            break;
        }
        if (!rate) {
            rate = b->rate;
            cap = 44 + (size_t)rate * KVM_AUDIO_CHANNELS * 2 * c->seconds;
            wav = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            if (!wav) {
                fail = "no memory";
                break;
            }
        }
        if (b->rate != rate) {
            break; /* the source changed rate: keep what came before */
        }
        const size_t n = (size_t)b->frames * KVM_AUDIO_CHANNELS * 2;
        if (used + n > cap) {
            break;
        }
        memcpy(wav + used, b->pcm, n);
        used += n;
    }
    kvm_audio_close(c->reader);
    free(b);

    if (fail) {
        send_json_error(c->req, "503 Service Unavailable", fail);
    } else {
        wav_header(wav, rate, (uint32_t)(used - 44));
        httpd_resp_set_type(c->req, "audio/wav");
        httpd_resp_set_hdr(c->req, "Content-Disposition", "attachment; filename=\"hdmi-audio.wav\"");
        httpd_resp_set_hdr(c->req, "Cache-Control", "no-store");
        httpd_resp_send(c->req, (const char *)wav, (ssize_t)used);
    }
    free(wav);
    httpd_req_async_handler_complete(c->req);
    free(c);
    vTaskDelete(NULL);
}

static esp_err_t audio_clip_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char q[24] = {0};
    char v[8] = {0};
    unsigned s = 5;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) {
        s = (unsigned)strtoul(v, NULL, 10);
    }
    if (s < 1 || s > CLIP_MAX_S) {
        return send_json_error(req, "400 Bad Request", "s is 1 to 10 seconds");
    }
    clip_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return send_json_error(req, "503 Service Unavailable", "no memory");
    }
    c->seconds = s;
    /* 200 ms of slack: the clip task only copies, it never falls far behind. */
    const esp_err_t err = kvm_audio_open(40, &c->reader);
    if (err != ESP_OK) {
        free(c);
        return send_json_error(req, "409 Conflict",
                               err == ESP_ERR_NO_MEM ? "no memory" : "HDMI audio is off or busy");
    }
    /* Seconds of waiting would tie up the control task; it goes to a worker. */
    if (httpd_req_async_handler_begin(req, &c->req) != ESP_OK) {
        kvm_audio_close(c->reader);
        free(c);
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    if (xTaskCreate(clip_task, "kvm_audio_clip", CLIP_STACK, c, CLIP_PRIO, NULL) != pdPASS) {
        kvm_audio_close(c->reader);
        httpd_req_async_handler_complete(c->req);
        free(c);
        return send_json_error(req, "503 Service Unavailable", "clip task");
    }
    return ESP_OK;
}

/* ---- the /audio socket ------------------------------------------------------ */

static int s_fds[WS_MAX_CLIENTS] = {-1, -1, -1, -1};
static SemaphoreHandle_t s_mu;
static TaskHandle_t s_pump;

void audio_api_drop(int fd)
{
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_fds[i] == fd) {
            s_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_mu);
}

static bool any_client(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool any = false;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        any |= s_fds[i] >= 0;
    }
    xSemaphoreGive(s_mu);
    return any;
}

static void pump_task(void *arg)
{
    (void)arg;
    kvm_opus_packet_t *in = heap_caps_malloc(sizeof(*in), MALLOC_CAP_SPIRAM);
    uint8_t *msg = heap_caps_malloc(WS_HDR + KVM_OPUS_MAX, MALLOC_CAP_SPIRAM);
    kvm_opus_reader_t *r = NULL;
    for (;;) {
        if (!any_client() || !in || !msg) {
            kvm_opus_close(r);
            r = NULL;
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!r && kvm_opus_open(WS_DEPTH, &r) != ESP_OK) {
            r = NULL;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); /* audio is off */
            continue;
        }
        if (kvm_opus_read(r, in, 200) != ESP_OK) {
            continue;
        }
        msg[0] = WS_OPUS;
        msg[1] = in->resync ? 1 : 0;
        msg[2] = (uint8_t)in->ts;
        msg[3] = (uint8_t)(in->ts >> 8);
        msg[4] = (uint8_t)(in->ts >> 16);
        msg[5] = (uint8_t)(in->ts >> 24);
        memcpy(msg + WS_HDR, in->data, in->len);
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            const int fd = s_fds[i];
            xSemaphoreGive(s_mu);
            if (fd >= 0 && kvm_web_ws_send_binary(fd, msg, WS_HDR + in->len) != ESP_OK) {
                audio_api_drop(fd);
            }
        }
    }
}

static esp_err_t audio_ws_handler(httpd_req_t *req)
{
    if (!kvm_auth_socket_ok(httpd_req_to_sockfd(req))) {
        return kvm_auth_reject_ws(req);
    }
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t pkt = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        audio_api_drop(fd);
        return ESP_OK;
    }
    if (pkt.len == 0) {
        return ESP_OK;
    }
    if (pkt.len > 16) {
        return ESP_FAIL; /* left unread it would desync the socket: close */
    }
    uint8_t data[16];
    pkt.payload = data;
    err = httpd_ws_recv_frame(req, &pkt, sizeof(data));
    if (err != ESP_OK || data[0] != WS_SUBSCRIBE || !s_mu) {
        return err;
    }
    bool ok = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS && !ok; i++) {
        ok = s_fds[i] == fd;
    }
    for (int i = 0; i < WS_MAX_CLIENTS && !ok; i++) {
        if (s_fds[i] < 0) {
            s_fds[i] = fd;
            ok = true;
        }
    }
    xSemaphoreGive(s_mu);
    if (!ok) {
        ESP_LOGW(TAG, "already %d listeners", WS_MAX_CLIENTS);
    }
    xTaskNotifyGive(s_pump);
    return ESP_OK;
}

esp_err_t audio_api_init(void)
{
    if (s_mu) {
        return ESP_OK;
    }
    s_mu = xSemaphoreCreateMutex();
    /* Stack in PSRAM: internal RAM is what TLS sessions need, and this task
     * never runs while the flash cache is off. */
    if (!s_mu || xTaskCreateWithCaps(pump_task, "audio_ws", 3072, NULL, 6, &s_pump,
                                     MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/audio",
     .method = HTTP_GET,
     .handler = audio_ws_handler,
     .is_websocket = true,
     .ws_pre_handshake_cb = kvm_web_ws_pre_handshake},
    {.uri = "/api/v1/audio", .method = HTTP_GET, .handler = audio_get},
    {.uri = "/api/v1/audio/clip.wav", .method = HTTP_GET, .handler = audio_clip_get},
};

const httpd_uri_t *audio_api_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
