/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The target's serial console, to the browser and to scripts.
 *
 *   WS   /serial             0x01 subscribe; 0x02 + bytes to type. The device
 *                            answers with raw bytes from the target, starting
 *                            with everything still in the ring.
 *   GET  /api/v1/serial      state: on, pins, speed, byte counts
 *   GET  /api/v1/serial/log  the last ?bytes= (default 4096) as text
 *   POST /api/v1/serial/send {"text":"root","enter":true} - behind agent_api
 *
 * A pump task sends to the sockets, so a slow browser holds up neither the
 * UART reader nor the web server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include "kvm_auth.h"
#include "kvm_serial.h"
#include "kvm_settings.h"
#include "web_priv.h"

static const char *TAG = "serial_api";

#define SER_WS_SUBSCRIBE 0x01
#define SER_WS_INPUT 0x02
#define SER_MAX_CLIENTS 4
#define SER_CHUNK 2048
#define SER_LOG_MAX KVM_SERIAL_RING_BYTES

typedef struct {
    int fd;       /* -1 when the slot is free */
    uint64_t pos; /* the next byte this socket has not been sent */
} ser_client_t;

static ser_client_t s_clients[SER_MAX_CLIENTS] = {{-1, 0}, {-1, 0}, {-1, 0}, {-1, 0}};
static SemaphoreHandle_t s_mu;
static TaskHandle_t s_pump;

static void ensure_started(void);

void serial_api_drop(int fd)
{
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < SER_MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i].fd = -1;
        }
    }
    xSemaphoreGive(s_mu);
}

static bool add_client(int fd)
{
    bool ok = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < SER_MAX_CLIENTS && !ok; i++) {
        if (s_clients[i].fd == fd) {
            ok = true; /* subscribed again: start over from the ring */
            s_clients[i].pos = kvm_serial_oldest();
        }
    }
    for (int i = 0; i < SER_MAX_CLIENTS && !ok; i++) {
        if (s_clients[i].fd < 0) {
            s_clients[i].fd = fd;
            s_clients[i].pos = kvm_serial_oldest();
            ok = true;
        }
    }
    xSemaphoreGive(s_mu);
    if (ok) {
        xTaskNotifyGive(s_pump);
    }
    return ok;
}

static void pump_task(void *arg)
{
    (void)arg;
    static uint8_t buf[SER_CHUNK];
    for (;;) {
        bool any = false;
        uint64_t lowest = UINT64_MAX;
        for (int i = 0; i < SER_MAX_CLIENTS; i++) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            const int fd = s_clients[i].fd;
            uint64_t pos = s_clients[i].pos;
            xSemaphoreGive(s_mu);
            if (fd < 0) {
                continue;
            }
            any = true;
            const size_t n = kvm_serial_read_from(&pos, buf, sizeof(buf));
            if (n > 0 && kvm_web_ws_send_binary(fd, buf, n) != ESP_OK) {
                serial_api_drop(fd);
                continue;
            }
            xSemaphoreTake(s_mu, portMAX_DELAY);
            if (s_clients[i].fd == fd) {
                s_clients[i].pos = pos;
            }
            xSemaphoreGive(s_mu);
            if (pos < lowest) {
                lowest = pos;
            }
        }
        if (!any) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            (void)kvm_serial_wait(lowest, 200);
        }
    }
}

static void ensure_started(void)
{
    if (s_mu) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
    /* Stack in PSRAM: internal RAM is what TLS sessions need, and this task
     * never runs while the flash cache is off. */
    if (xTaskCreateWithCaps(pump_task, "ser_pump", 4096, NULL, 4, &s_pump, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        ESP_LOGE(TAG, "no memory for the serial pump");
    }
}

static esp_err_t serial_ws_handler(httpd_req_t *req)
{
    if (!kvm_auth_socket_ok(httpd_req_to_sockfd(req))) {
        return kvm_auth_reject_ws(req);
    }
    ensure_started();
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t pkt = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        serial_api_drop(fd);
        return ESP_OK;
    }
    if (pkt.len == 0) {
        return ESP_OK;
    }
    uint8_t small[260];
    uint8_t *data = pkt.len <= sizeof(small) ? small : malloc(pkt.len);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = data;
    err = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (err == ESP_OK) {
        if (data[0] == SER_WS_SUBSCRIBE) {
            if (!add_client(fd)) {
                ESP_LOGW(TAG, "serial console: already %d viewers", SER_MAX_CLIENTS);
            }
        } else if (data[0] == SER_WS_INPUT && pkt.len > 1) {
            (void)kvm_serial_write(data + 1, pkt.len - 1);
        }
    }
    if (data != small) {
        free(data);
    }
    return err;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t serial_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_serial_status_t st;
    kvm_serial_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", kvm_setting_bool("ser_enable"));
    cJSON_AddBoolToObject(root, "running", st.running);
    cJSON_AddNumberToObject(root, "tx", st.tx_gpio);
    cJSON_AddNumberToObject(root, "rx", st.rx_gpio);
    cJSON_AddNumberToObject(root, "baud", st.baud);
    cJSON_AddNumberToObject(root, "rxBytes", (double)st.rx_bytes);
    cJSON_AddNumberToObject(root, "txBytes", (double)st.tx_bytes);
    return send_json(req, root);
}

static esp_err_t serial_log_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    size_t want = 4096;
    char q[32] = {0};
    char v[12] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "bytes", v, sizeof(v)) == ESP_OK) {
        const long n = strtol(v, NULL, 10);
        if (n > 0) {
            want = n > SER_LOG_MAX ? SER_LOG_MAX : (size_t)n;
        }
    }
    kvm_serial_status_t st;
    kvm_serial_status(&st);
    uint64_t pos = st.rx_bytes > want ? st.rx_bytes - want : 0;
    char *buf = malloc(want);
    if (!buf) {
        return send_json_error(req, "500 Internal Server Error", "no memory");
    }
    const size_t n = kvm_serial_read_from(&pos, (uint8_t *)buf, want);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return r;
}

static esp_err_t serial_send_post(httpd_req_t *req)
{
    esp_err_t gate;
    if (!kvm_web_agent_allowed(req, &gate)) {
        return gate;
    }
    char body[512];
    const int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        const int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return send_json_error(req, "400 Bad Request", "could not read the body");
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    const cJSON *text = j ? cJSON_GetObjectItem(j, "text") : NULL;
    if (!cJSON_IsString(text)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "expected {\"text\":\"...\"}");
    }
    esp_err_t err = kvm_serial_write(text->valuestring, strlen(text->valuestring));
    if (err == ESP_OK && cJSON_IsTrue(cJSON_GetObjectItem(j, "enter"))) {
        err = kvm_serial_write("\r", 1);
    }
    cJSON_Delete(j);
    if (err != ESP_OK) {
        return send_json_error(req, "409 Conflict", "the serial console is not running");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/serial",
     .method = HTTP_GET,
     .handler = serial_ws_handler,
     .is_websocket = true,
     .ws_pre_handshake_cb = kvm_web_ws_pre_handshake},
    {.uri = "/api/v1/serial", .method = HTTP_GET, .handler = serial_get},
    {.uri = "/api/v1/serial/log", .method = HTTP_GET, .handler = serial_log_get},
    {.uri = "/api/v1/serial/send", .method = HTTP_POST, .handler = serial_send_post},
};

const httpd_uri_t *serial_api_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
