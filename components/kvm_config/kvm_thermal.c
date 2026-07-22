/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_thermal.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kvm_settings.h"

#define TAG "thermal"

/*
 * The sensor is configured for a wider range than the device is expected to
 * reach. Its accuracy is specified per range, and a reading outside the one it
 * was installed with is not to be trusted - which would be exactly the reading
 * that matters here.
 */
#define SENSOR_RANGE_MIN 20
#define SENSOR_RANGE_MAX 100

#define SAMPLE_INTERVAL_MS 1000

/*
 * Coming back down takes more than crossing the line again: without hysteresis
 * a device sitting on a threshold would flip between full rate and half rate
 * every second, which looks like a fault and is worse for the operator than
 * either state.
 */
#define HYSTERESIS_C 4

static temperature_sensor_handle_t s_sensor;
static volatile float s_celsius;
static volatile kvm_thermal_state_t s_state = KVM_THERMAL_UNKNOWN;

float kvm_thermal_celsius(void)
{
    return s_celsius;
}

kvm_thermal_state_t kvm_thermal_state(void)
{
    return s_state;
}

const char *kvm_thermal_state_name(kvm_thermal_state_t state)
{
    switch (state) {
    case KVM_THERMAL_NORMAL:
        return "normal";
    case KVM_THERMAL_HOT:
        return "hot";
    case KVM_THERMAL_CRITICAL:
        return "critical";
    default:
        return "unknown";
    }
}

int kvm_thermal_fps_limit(int configured_fps)
{
    switch (s_state) {
    case KVM_THERMAL_HOT:
        return configured_fps > 1 ? configured_fps / 2 : 1;
    case KVM_THERMAL_CRITICAL:
        return 0;
    default:
        return configured_fps;
    }
}

static void thermal_task(void *arg)
{
    (void)arg;
    kvm_thermal_state_t state = KVM_THERMAL_NORMAL;

    for (;;) {
        float c = 0.0f;
        if (temperature_sensor_get_celsius(s_sensor, &c) == ESP_OK) {
            s_celsius = c;

            if (kvm_setting_bool("therm_guard")) {
                const int warn = kvm_setting_int("therm_warn");
                const int stop = kvm_setting_int("therm_stop");
                const kvm_thermal_state_t before = state;

                if (c >= (float)stop) {
                    state = KVM_THERMAL_CRITICAL;
                } else if (c >= (float)warn) {
                    /* Only ever step down one level at a time, and only once
                     * the reading has fallen clear of the threshold. */
                    if (state == KVM_THERMAL_CRITICAL && c > (float)(stop - HYSTERESIS_C)) {
                        /* still cooling from critical */
                    } else {
                        state = KVM_THERMAL_HOT;
                    }
                } else if (c < (float)(warn - HYSTERESIS_C)) {
                    if (state != KVM_THERMAL_CRITICAL || c < (float)(stop - HYSTERESIS_C)) {
                        state = KVM_THERMAL_NORMAL;
                    }
                }

                if (state != before) {
                    /* A warning, not a debug line: the operator will want to
                     * know why the picture changed, and so will we. */
                    ESP_LOGW(TAG, "%.1f C: %s -> %s", c, kvm_thermal_state_name(before),
                             kvm_thermal_state_name(state));
                }
            } else if (state != KVM_THERMAL_NORMAL) {
                ESP_LOGW(TAG, "thermal guard switched off at %.1f C", c);
                state = KVM_THERMAL_NORMAL;
            }
            s_state = state;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}

void kvm_thermal_init(void)
{
    if (s_sensor) {
        return;
    }
    const temperature_sensor_config_t cfg =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(SENSOR_RANGE_MIN, SENSOR_RANGE_MAX);
    esp_err_t err = temperature_sensor_install(&cfg, &s_sensor);
    if (err != ESP_OK) {
        /* Installed once at start-up rather than on first use: a failure here
         * is visible in the boot log, where one inside a request handler is
         * swallowed and the reading silently stays at zero. */
        ESP_LOGW(TAG, "sensor install: %s", esp_err_to_name(err));
        s_sensor = NULL;
        return;
    }
    err = temperature_sensor_enable(s_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sensor enable: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_sensor);
        s_sensor = NULL;
        return;
    }
    s_state = KVM_THERMAL_NORMAL;
    xTaskCreate(thermal_task, "kvm_therm", 3072, NULL, 3, NULL);
}
