/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The buttons on the box: up to BTN_COUNT push buttons, each on a GPIO of its
 * own with its own short-press and hold actions (Settings, Buttons). One small
 * task polls them every 20 ms - a button is slow, and polling needs no
 * interrupt and no debounce timer - and tells a short press from a long hold.
 * The hold fires as soon as it has been held long enough, so the person knows
 * it took; a press fires on release. A button set to "menu" opens the menu on
 * the status screen (menu.c), and while it is open the buttons work it.
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
#include "kvm_menu.h"
#include "kvm_settings.h"
#include "sdkconfig.h"

static const char *TAG = "button";

#define POLL_MS 20
#define DEBOUNCE_MS 40
#define HOLD_MS 1500

/* Index = the btn_press / btn_hold choice; must match s_btn_action_choices in
   kvm_settings_table.c. */
static const char *const k_actions[] = {"", "power", "poweroff", "reset", "wol",
                                        "runbook", "clip", "screenshot",
                                        "hotspot", "netnext", "netswap",
                                        "displaypage", "displaytoggle", "usbwake",
                                        "menu"};

static const char *action_of(const char *key)
{
    const int32_t i = kvm_setting_int(key);
    return (i >= 0 && (size_t)i < sizeof(k_actions) / sizeof(k_actions[0])) ? k_actions[i] : "";
}

#define BTN_COUNT 4 /* the most there are keys for */
#ifndef CONFIG_KVM_BTN_COUNT
#define CONFIG_KVM_BTN_COUNT BTN_COUNT
#endif

/* The setting keys of each button. The first keeps the names it had when there
   was only one, so a device set up then keeps its button. */
static const struct {
    const char *gpio, *high, *press, *hold, *runbook;
} k_keys[BTN_COUNT] = {
    {"btn_gpio", "btn_active_high", "btn_press", "btn_hold", "btn_runbook"},
    {"btn2_gpio", "btn2_high", "btn2_press", "btn2_hold", "btn2_runbook"},
    {"btn3_gpio", "btn3_high", "btn3_press", "btn3_hold", "btn3_runbook"},
    {"btn4_gpio", "btn4_high", "btn4_press", "btn4_hold", "btn4_runbook"},
};

typedef struct {
    int set;  /* the GPIO the setting names, -1 for none */
    int pin;  /* the GPIO in use: set, or -1 when it could not be an input */
    bool active_high;
    bool pressed, held, raw_last;
    int64_t since, stable_since;
} btn_t;

static void fire(int i, const char *key)
{
    /* An open menu takes every button: a press moves on, a hold picks. */
    if (kvm_menu_is_open()) {
        if (key == k_keys[i].hold) {
            kvm_menu_pick();
        } else {
            kvm_menu_next();
        }
        return;
    }
    const char *action = action_of(key);
    if (action[0]) {
        ESP_LOGI(TAG, "button %d %s: %s", i + 1, key, action);
        (void)kvm_action_run("button", action, kvm_setting_str(k_keys[i].runbook), "button", NULL, 0);
    }
}

/* Follow the settings: a new pin or polarity takes effect without a restart. */
static void configure(int i, btn_t *b)
{
    const int want = (int)kvm_setting_int(k_keys[i].gpio);
    const bool want_ah = kvm_setting_bool(k_keys[i].high);
    if (want == b->set && want_ah == b->active_high) {
        return;
    }
    if (b->pin >= 0) {
        gpio_reset_pin((gpio_num_t)b->pin);
    }
    *b = (btn_t){.set = want, .pin = -1, .active_high = want_ah};
    if (want < 0) {
        return;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << want,
        .mode = GPIO_MODE_INPUT,
        /* A button to ground (the usual) needs the pull-up; one to 3V3 the
           pull-down. Either way an open input reads idle. */
        .pull_up_en = want_ah ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = want_ah ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "button %d: GPIO %d cannot be an input", i + 1, want);
        return; /* tried again when the setting changes */
    }
    b->pin = want;
    ESP_LOGI(TAG, "button %d on GPIO %d (%s)", i + 1, want, want_ah ? "active high" : "to ground");
}

static void poll(int i, btn_t *b, int64_t now)
{
    if (b->pin < 0) {
        return;
    }
    const bool raw = gpio_get_level((gpio_num_t)b->pin) == (b->active_high ? 1 : 0);
    if (raw != b->raw_last) {
        b->raw_last = raw;
        b->stable_since = now;
        return;
    }
    if (now - b->stable_since < DEBOUNCE_MS) {
        return;
    }
    if (raw && !b->pressed) {
        b->pressed = true;
        b->held = false;
        b->since = now;
    } else if (raw && b->pressed && !b->held && now - b->since >= HOLD_MS) {
        b->held = true;
        fire(i, k_keys[i].hold);
    } else if (!raw && b->pressed) {
        b->pressed = false;
        if (!b->held) {
            fire(i, k_keys[i].press);
        }
    }
}

static void task(void *arg)
{
    (void)arg;
    btn_t btn[BTN_COUNT];
    for (int i = 0; i < BTN_COUNT; i++) {
        btn[i] = (btn_t){.set = -1, .pin = -1};
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int64_t now = esp_timer_get_time() / 1000;
        for (int i = 0; i < CONFIG_KVM_BTN_COUNT && i < BTN_COUNT; i++) {
            configure(i, &btn[i]);
            poll(i, &btn[i], now);
        }
        kvm_menu_tick();
    }
}

void kvm_button_init(void)
{
    /* Internal stack: actions such as the network mode or the display switch
       write settings to flash, and a flash write asserts on a PSRAM stack. */
    if (xTaskCreate(task, "button", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the button task");
    }
}
