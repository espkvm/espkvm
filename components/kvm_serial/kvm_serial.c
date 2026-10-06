/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_serial.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "kvm_caps.h"
#include "kvm_settings.h"

static const char *TAG = "serial";

/* UART0 carries this device's own log; the target gets the next one. */
#define SER_PORT UART_NUM_1
#define SER_RX_BUF 4096
#define SER_TX_BUF 1024
#define SER_TASK_STACK 3072
#define SER_TASK_PRIO 5

/* Must match s_baud_choices in kvm_settings_table.c. */
static const uint32_t k_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

static uint8_t *s_ring;
static uint64_t s_head; /* bytes ever received; the newest is s_head - 1 */
static uint64_t s_tx_bytes;
static SemaphoreHandle_t s_ring_mu;
/* Held by the reader while it is inside the driver, and by apply while it tears
 * the driver down, so the two never meet. */
static SemaphoreHandle_t s_uart_mu;
static volatile bool s_running;
static int s_tx = -1;
static int s_rx = -1;
static uint32_t s_baud = 115200;
static TaskHandle_t s_task;

/* 64 bits are two words here: read them under the lock or get half of each. */
static uint64_t head_now(void)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    const uint64_t h = s_head;
    xSemaphoreGive(s_ring_mu);
    return h;
}

static void ring_append(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        s_ring[(s_head + i) % KVM_SERIAL_RING_BYTES] = data[i];
    }
    s_head += len;
    xSemaphoreGive(s_ring_mu);
}

static void reader_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        if (!s_running) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        int n = 0;
        if (xSemaphoreTake(s_uart_mu, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_running) {
                n = uart_read_bytes(SER_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
            }
            xSemaphoreGive(s_uart_mu);
        }
        if (n > 0) {
            ring_append(buf, (size_t)n);
        } else {
            vTaskDelay(1); /* let apply in between two reads */
        }
    }
}

esp_err_t kvm_serial_init(void)
{
    if (s_ring) {
        return ESP_OK;
    }
    s_ring = heap_caps_calloc(1, KVM_SERIAL_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring_mu = xSemaphoreCreateMutex();
    s_uart_mu = xSemaphoreCreateMutex();
    if (!s_ring || !s_ring_mu || !s_uart_mu) {
        return ESP_ERR_NO_MEM;
    }
    /* Stack in PSRAM: internal RAM is what TLS sessions need, and this task
     * never runs while the flash cache is off. */
    if (xTaskCreateWithCaps(reader_task, "serial", SER_TASK_STACK, NULL, SER_TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void uart_close(void)
{
    if (!s_running) {
        return;
    }
    xSemaphoreTake(s_uart_mu, portMAX_DELAY);
    s_running = false;
    (void)uart_driver_delete(SER_PORT);
    xSemaphoreGive(s_uart_mu);
}

void kvm_serial_apply(void)
{
    if (!s_ring) {
        return;
    }
    uart_close();

    const bool enabled = kvm_setting_bool("ser_enable");
    s_tx = (int)kvm_setting_int("ser_tx");
    s_rx = (int)kvm_setting_int("ser_rx");
    const int32_t b = kvm_setting_int("ser_baud");
    s_baud = (b >= 0 && b < (int32_t)(sizeof(k_bauds) / sizeof(k_bauds[0]))) ? k_bauds[b] : 115200;

    if (!enabled) {
        kvm_cap_report(KVM_CAP_SERIAL, false, "switched off in settings");
        return;
    }
    if (s_tx < 0 || s_rx < 0 || s_tx == s_rx) {
        kvm_cap_report(KVM_CAP_SERIAL, false, "pick the TX and RX pins in Settings");
        return;
    }

    const uart_config_t cfg = {
        .baud_rate = (int)s_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(SER_PORT, SER_RX_BUF, SER_TX_BUF, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(SER_PORT, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(SER_PORT, s_tx, s_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        (void)uart_driver_delete(SER_PORT);
        ESP_LOGE(TAG, "UART on TX %d / RX %d: %s", s_tx, s_rx, esp_err_to_name(err));
        kvm_cap_report(KVM_CAP_SERIAL, false, "UART setup failed: %s", esp_err_to_name(err));
        return;
    }
    /* A line nobody drives reads as a stream of breaks; idle is high. */
    (void)gpio_pullup_en((gpio_num_t)s_rx);
    s_running = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "serial console on TX %d / RX %d at %lu 8N1", s_tx, s_rx, (unsigned long)s_baud);
    kvm_cap_report(KVM_CAP_SERIAL, true, NULL);
}

void kvm_serial_status(kvm_serial_status_t *out)
{
    if (!out) {
        return;
    }
    out->running = s_running;
    out->tx_gpio = s_tx;
    out->rx_gpio = s_rx;
    out->baud = s_baud;
    out->rx_bytes = s_ring_mu ? head_now() : 0;
    out->tx_bytes = s_tx_bytes;
}

esp_err_t kvm_serial_write(const void *data, size_t len)
{
    if (!s_running || !s_uart_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_running) {
        const int n = uart_write_bytes(SER_PORT, data, len);
        err = n == (int)len ? ESP_OK : ESP_FAIL;
        if (n > 0) {
            s_tx_bytes += (uint64_t)n;
        }
    }
    xSemaphoreGive(s_uart_mu);
    return err;
}

uint64_t kvm_serial_oldest(void)
{
    const uint64_t head = s_ring_mu ? head_now() : 0;
    return head > KVM_SERIAL_RING_BYTES ? head - KVM_SERIAL_RING_BYTES : 0;
}

size_t kvm_serial_read_from(uint64_t *pos, uint8_t *out, size_t max)
{
    if (!s_ring || !pos || !out || !max) {
        return 0;
    }
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    const uint64_t oldest = s_head > KVM_SERIAL_RING_BYTES ? s_head - KVM_SERIAL_RING_BYTES : 0;
    if (*pos < oldest) {
        *pos = oldest;
    }
    if (*pos > s_head) {
        *pos = s_head; /* a stale position from before a restart */
    }
    size_t n = (size_t)(s_head - *pos);
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(*pos + i) % KVM_SERIAL_RING_BYTES];
    }
    *pos += n;
    xSemaphoreGive(s_ring_mu);
    return n;
}

bool kvm_serial_wait(uint64_t seen, uint32_t timeout_ms)
{
    /* Polled: the readers are a WebSocket pump and a runbook wait, and a tick
     * or two of latency on a serial console is nothing. */
    if (!s_ring_mu) {
        return false;
    }
    TickType_t left = pdMS_TO_TICKS(timeout_ms);
    while (head_now() <= seen) {
        if (left == 0) {
            return false;
        }
        const TickType_t step = left < 2 ? left : 2;
        vTaskDelay(step);
        left -= step;
    }
    return true;
}
