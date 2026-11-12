/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "kvm_settings.h"
#include "usb_hid.h"

static const char *TAG = "tools";

/* usb_serial: 0 off, 1 console, 2 tools. Must match s_usb_serial_choices. */
#define USB_SERIAL_TOOLS 2

/* The program escapes <, > and & as <: six bytes for one, at worst. */
#define TOOLS_LINE_MAX (KVM_TOOLS_MAX_TEXT * 6 + 1024)
#define RX_STREAM 16384
#define TASK_STACK 4096
#define TASK_PRIO 4

static StreamBufferHandle_t s_rx;
static char *s_line;
static size_t s_len;
static bool s_overflow;

static SemaphoreHandle_t s_mu;   /* status and the waiting slot */
static SemaphoreHandle_t s_call; /* one request at a time */
static SemaphoreHandle_t s_done; /* the answer has come */
static kvm_tools_status_t s_st;
static int s_next_id = 1;
static int s_wait_id;     /* the id being waited for, 0 for none */
static char *s_answer;    /* its line, handed to the caller */

/* TinyUSB's task: only copy, the parsing happens on our own task so a long
 * line never holds up the keyboard. */
static void on_rx(const uint8_t *data, size_t len)
{
    if (xStreamBufferSend(s_rx, data, len, pdMS_TO_TICKS(20)) != len) {
        ESP_LOGW(TAG, "rx overflow: dropped bytes from the target");
    }
}

static void on_dtr(bool dtr)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!dtr && s_st.connected) {
        ESP_LOGI(TAG, "the program closed the port");
        s_st.connected = false;
    }
    xSemaphoreGive(s_mu);
}

static void take_hello(const cJSON *h)
{
    const cJSON *ver = cJSON_GetObjectItem(h, "tools");
    if (!ver) {
        ver = cJSON_GetObjectItem(h, "agent"); /* versions before the rename */
    }
    const cJSON *os = cJSON_GetObjectItem(h, "os");
    const cJSON *ops = cJSON_GetObjectItem(h, "ops");
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool was = s_st.connected;
    s_st.connected = true;
    snprintf(s_st.version, sizeof(s_st.version), "%s", cJSON_IsString(ver) ? ver->valuestring : "");
    snprintf(s_st.os, sizeof(s_st.os), "%s", cJSON_IsString(os) ? os->valuestring : "");
    s_st.ops[0] = '\0';
    const cJSON *op;
    cJSON_ArrayForEach(op, ops)
    {
        if (cJSON_IsString(op) && strlen(s_st.ops) + strlen(op->valuestring) + 2 < sizeof(s_st.ops)) {
            if (s_st.ops[0]) {
                strcat(s_st.ops, ",");
            }
            strcat(s_st.ops, op->valuestring);
        }
    }
    xSemaphoreGive(s_mu);
    if (!was) {
        ESP_LOGI(TAG, "ESP-KVM Tools %s on %s", s_st.version, s_st.os);
    }
}

static void handle_line(char *line, size_t len)
{
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (!len || line[0] != '{') {
        return;
    }
    cJSON *j = cJSON_ParseWithLength(line, len);
    if (!j) {
        return;
    }
    const cJSON *ev = cJSON_GetObjectItem(j, "ev");
    if (cJSON_IsString(ev) && strcmp(ev->valuestring, "hello") == 0) {
        take_hello(j);
    }
    const cJSON *id = cJSON_GetObjectItem(j, "id");
    if (cJSON_IsNumber(id)) {
        const cJSON *data = cJSON_GetObjectItem(j, "data");
        const cJSON *dev = cJSON_GetObjectItem(data, "ev");
        if (cJSON_IsString(dev) && strcmp(dev->valuestring, "hello") == 0) {
            take_hello(data);
        }
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_wait_id && id->valueint == s_wait_id) {
            s_answer = strndup(line, len);
            s_wait_id = 0;
            xSemaphoreGive(s_done);
        }
        xSemaphoreGive(s_mu);
    }
    cJSON_Delete(j);
}

static void tools_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    for (;;) {
        const size_t n = xStreamBufferReceive(s_rx, buf, sizeof(buf), portMAX_DELAY);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!s_overflow) {
                    s_line[s_len] = '\0';
                    handle_line(s_line, s_len);
                }
                s_len = 0;
                s_overflow = false;
            } else if (s_len < TOOLS_LINE_MAX - 1) {
                s_line[s_len++] = (char)buf[i];
            } else if (!s_overflow) {
                ESP_LOGW(TAG, "line over %d bytes from the target, dropped", TOOLS_LINE_MAX);
                s_overflow = true;
            }
        }
    }
}

esp_err_t kvm_tools_init(void)
{
    if (s_rx) {
        return ESP_OK;
    }
    if (kvm_setting_int("usb_serial") != USB_SERIAL_TOOLS || !usb_hid_serial_present()) {
        return ESP_OK;
    }
    s_line = heap_caps_malloc(TOOLS_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rx = xStreamBufferCreateWithCaps(RX_STREAM, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mu = xSemaphoreCreateMutex();
    s_call = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_line || !s_rx || !s_mu || !s_call || !s_done) {
        return ESP_ERR_NO_MEM;
    }
    /* Stack in PSRAM: the task never runs with the flash cache off. */
    if (xTaskCreateWithCaps(tools_task, "tools", TASK_STACK, NULL, TASK_PRIO, NULL, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_st.port = true;
    usb_hid_serial_set_line_state(on_dtr);
    usb_hid_serial_set_rx(on_rx);
    ESP_LOGI(TAG, "waiting for ESP-KVM Tools on the USB serial port");
    return ESP_OK;
}

void kvm_tools_status(kvm_tools_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mu);
}

esp_err_t kvm_tools_call(const char *op, const char *params_json, int timeout_ms, char **out)
{
    *out = NULL;
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool hello = strcmp(op, "hello") == 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool connected = s_st.connected;
    xSemaphoreGive(s_mu);
    if (!connected && !hello) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_call, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int id = s_next_id++;
    s_wait_id = id;
    free(s_answer);
    s_answer = NULL;
    xSemaphoreGive(s_mu);
    (void)xSemaphoreTake(s_done, 0); /* a late answer to an earlier call */

    const size_t plen = params_json ? strlen(params_json) : 0;
    const size_t cap = strlen(op) + plen + 64;
    char *req = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (req) {
        const int n = params_json ? snprintf(req, cap, "{\"id\":%d,\"op\":\"%s\",\"params\":%s}\n", id, op, params_json)
                                  : snprintf(req, cap, "{\"id\":%d,\"op\":\"%s\"}\n", id, op);
        err = usb_hid_serial_write(req, (size_t)n) == (size_t)n ? ESP_OK : ESP_ERR_TIMEOUT;
        free(req);
    }
    if (err == ESP_OK) {
        const int32_t left = (int32_t)(until - xTaskGetTickCount());
        err = xSemaphoreTake(s_done, left > 0 ? (TickType_t)left : 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_wait_id = 0;
    if (err == ESP_OK) {
        *out = s_answer;
        s_answer = NULL;
    }
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_call);
    return err;
}
