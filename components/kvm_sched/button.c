/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The button on the box. One small task polls a GPIO every 20 ms - a button is
 * slow, and polling needs no interrupt and no debounce timer - and tells a
 * short press from a long hold. The hold fires as soon as it has been held long
 * enough, so the person knows it took; a press fires on release.
 *
 * NOT tried with real hardware yet.
 */
#include "kvm_sched.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "kvm_settings.h"

static const char *TAG = "button";

#define POLL_MS 20
#define DEBOUNCE_MS 40
#define HOLD_MS 1500

/* Index = the btn_press / btn_hold choice; must match s_btn_action_choices in
   kvm_settings_table.c. */
static const char *const k_actions[] = {"", "power", "poweroff", "reset", "wol",
                                        "runbook", "clip", "screenshot"};

static const char *action_of(const char *key)
{
    const int32_t i = kvm_setting_int(key);
    return (i >= 0 && (size_t)i < sizeof(k_actions) / sizeof(k_actions[0])) ? k_actions[i] : "";
}

static void fire(const char *key)
{
    const char *action = action_of(key);
    if (action[0]) {
        ESP_LOGI(TAG, "%s: %s", key, action);
        kvm_action_run("button", action, kvm_setting_str("btn_runbook"), "button");
    }
}

static void task(void *arg)
{
    (void)arg;
    int pin = -1;
    bool active_high = false;
    bool pressed = false, held = false;
    int64_t since = 0, stable_since = 0;
    bool raw_last = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int want = (int)kvm_setting_int("btn_gpio");
        const bool want_ah = kvm_setting_bool("btn_active_high");
        if (want != pin || want_ah != active_high) {
            if (pin >= 0) {
                gpio_reset_pin((gpio_num_t)pin);
            }
            pin = want;
            active_high = want_ah;
            pressed = held = raw_last = false;
            if (pin >= 0) {
                const gpio_config_t io = {
                    .pin_bit_mask = 1ULL << pin,
                    .mode = GPIO_MODE_INPUT,
                    /* A button to ground (the usual) needs the pull-up; one to
                       3V3 the pull-down. Either way an open input reads idle. */
                    .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
                    .pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
                };
                if (gpio_config(&io) != ESP_OK) {
                    ESP_LOGW(TAG, "GPIO %d cannot be an input", pin);
                    pin = -2; /* do not retry until the setting changes */
                } else {
                    ESP_LOGI(TAG, "button on GPIO %d (%s)", pin, active_high ? "active high" : "to ground");
                }
            }
        }
        if (pin < 0) {
            continue;
        }
        const bool raw = gpio_get_level((gpio_num_t)pin) == (active_high ? 1 : 0);
        const int64_t now = esp_timer_get_time() / 1000;
        if (raw != raw_last) {
            raw_last = raw;
            stable_since = now;
            continue;
        }
        if (now - stable_since < DEBOUNCE_MS) {
            continue;
        }
        if (raw && !pressed) {
            pressed = true;
            held = false;
            since = now;
        } else if (raw && pressed && !held && now - since >= HOLD_MS) {
            held = true;
            fire("btn_hold");
        } else if (!raw && pressed) {
            pressed = false;
            if (!held) {
                fire("btn_press");
            }
        }
    }
}

void kvm_button_init(void)
{
    /* Stack in PSRAM: the actions it runs write no flash, and internal RAM is
       what TLS sessions need. */
    if (xTaskCreateWithCaps(task, "button", 3072, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the button task");
    }
}
