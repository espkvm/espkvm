/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_atx.h. A single worker task owns the button outputs so a five-second
 * hard-off hold never blocks an HTTP worker, and so two presses can never
 * overlap on the wire. The LED is read straight from the pin - it is a level,
 * not an event.
 *
 * Blind bring-up: none of this has touched a real optocoupler yet. The two
 * unknowns until it does are the module's trigger polarity (does driving the
 * input high press the button, or release it?) and the LED sense polarity.
 * Both are settings (atx_active_high, atx_led_active_high) so a wrong guess is
 * a checkbox, not a reflash.
 */
#include "kvm_atx.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kvm_caps.h"
#include "kvm_settings.h"

static const char *TAG = "kvm_atx";

typedef enum {
    ATX_CMD_CLICK, /* short press, power button */
    ATX_CMD_HOLD,  /* long press, power button (hard off) */
    ATX_CMD_RESET, /* short press, reset button */
} atx_cmd_t;

typedef struct {
    bool enabled;         /* atx_enable AND both button pins valid */
    int pwr_gpio;
    int rst_gpio;
    int led_gpio;         /* -1 when no LED sense is wired */
    int short_ms;
    int long_ms;
    bool active_high;     /* drive high to "press" the button */
    bool led_active_high; /* LED line reads high when the target is on */
} atx_cfg_t;

static atx_cfg_t s_cfg;
static SemaphoreHandle_t s_cfg_mtx;
static QueueHandle_t s_cmd_q;

/* Idle level of a button output: whichever level does NOT press it. */
static inline int idle_level(bool active_high) { return active_high ? 0 : 1; }
static inline int press_level(bool active_high) { return active_high ? 1 : 0; }

static void atx_task(void *arg)
{
    (void)arg;
    atx_cmd_t cmd;
    while (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) == pdTRUE) {
        atx_cfg_t c;
        xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
        c = s_cfg;
        xSemaphoreGive(s_cfg_mtx);
        if (!c.enabled) {
            continue; /* disabled between queueing and running; drop it */
        }

        int pin;
        int hold_ms;
        const char *what;
        switch (cmd) {
        case ATX_CMD_HOLD:
            pin = c.pwr_gpio;
            hold_ms = c.long_ms;
            what = "power hold (hard off)";
            break;
        case ATX_CMD_RESET:
            pin = c.rst_gpio;
            hold_ms = c.short_ms;
            what = "reset";
            break;
        case ATX_CMD_CLICK:
        default:
            pin = c.pwr_gpio;
            hold_ms = c.short_ms;
            what = "power click";
            break;
        }

        ESP_LOGW(TAG, "%s: GPIO %d for %d ms", what, pin, hold_ms);
        gpio_set_level(pin, press_level(c.active_high));
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
        gpio_set_level(pin, idle_level(c.active_high));
        /* A settle gap so a rapid second command is still a distinct press. */
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    vTaskDelete(NULL);
}

esp_err_t kvm_atx_init(void)
{
    if (s_cfg_mtx) {
        return ESP_OK; /* already initialised */
    }
    s_cfg_mtx = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(4, sizeof(atx_cmd_t));
    if (!s_cfg_mtx || !s_cmd_q) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.pwr_gpio = s_cfg.rst_gpio = s_cfg.led_gpio = -1;
    if (xTaskCreate(atx_task, "atx", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Return a pin to a safe, un-driven input so a later apply() can re-use it or
 * hand it back for something else entirely. */
static void release_pin(int gpio)
{
    if (gpio >= 0) {
        gpio_reset_pin(gpio);
    }
}

static esp_err_t configure_output(int gpio, bool active_high)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        gpio_set_level(gpio, idle_level(active_high)); /* never press on boot */
    }
    return err;
}

static esp_err_t configure_led(int gpio, bool active_high)
{
    /* Bias the line toward "off" so an unconnected pin reads as powered-down
     * rather than floating into a false "on". */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

esp_err_t kvm_atx_apply(void)
{
    if (!s_cfg_mtx) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool want_enable = kvm_setting_bool("atx_enable");
    const int pwr = (int)kvm_setting_int("atx_pwr_gpio");
    const int rst = (int)kvm_setting_int("atx_rst_gpio");
    const int led = (int)kvm_setting_int("atx_led_gpio");
    const int short_ms = (int)kvm_setting_int("atx_short_ms");
    const int long_ms = (int)kvm_setting_int("atx_long_ms");
    const bool active_high = kvm_setting_bool("atx_active_high");
    const bool led_active_high = kvm_setting_bool("atx_led_active_high");

    const bool pins_valid = pwr >= 0 && rst >= 0;
    const bool want_hw = want_enable && pins_valid;

    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);

    /* Drop whatever we held before; a pin may have moved or been switched off. */
    release_pin(s_cfg.pwr_gpio);
    release_pin(s_cfg.rst_gpio);
    release_pin(s_cfg.led_gpio);
    s_cfg.enabled = false;
    s_cfg.pwr_gpio = s_cfg.rst_gpio = s_cfg.led_gpio = -1;

    esp_err_t err = ESP_OK;
    if (want_hw) {
        err = configure_output(pwr, active_high);
        if (err == ESP_OK) {
            err = configure_output(rst, active_high);
        }
        if (err == ESP_OK && led >= 0) {
            err = configure_led(led, led_active_high);
        }
        if (err == ESP_OK) {
            s_cfg.enabled = true;
            s_cfg.pwr_gpio = pwr;
            s_cfg.rst_gpio = rst;
            s_cfg.led_gpio = led;
            s_cfg.short_ms = short_ms;
            s_cfg.long_ms = long_ms;
            s_cfg.active_high = active_high;
            s_cfg.led_active_high = led_active_high;
        } else {
            ESP_LOGE(TAG, "GPIO setup failed: %s", esp_err_to_name(err));
            release_pin(pwr);
            release_pin(rst);
            release_pin(led);
        }
    }

    xSemaphoreGive(s_cfg_mtx);

    /* "available" describes the wiring, not the on/off switch: the console shows
     * the switched-off case through the separate "enabled" flag. */
    if (pins_valid && err == ESP_OK) {
        kvm_cap_report(KVM_CAP_ATX, true, NULL);
    } else if (!pins_valid) {
        kvm_cap_report(KVM_CAP_ATX, false,
                       "set the power and reset button GPIOs (Settings -> Power)");
    } else {
        kvm_cap_report(KVM_CAP_ATX, false, "GPIO setup failed: %s", esp_err_to_name(err));
    }
    return err;
}

void kvm_atx_status(kvm_atx_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_cfg_mtx) {
        return;
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    out->enabled = s_cfg.enabled;
    out->have_led = s_cfg.enabled && s_cfg.led_gpio >= 0;
    if (out->have_led) {
        const int level = gpio_get_level(s_cfg.led_gpio);
        out->power_on = s_cfg.led_active_high ? (level != 0) : (level == 0);
    }
    xSemaphoreGive(s_cfg_mtx);
}

static esp_err_t queue_cmd(atx_cmd_t cmd)
{
    if (!s_cmd_q) {
        return ESP_ERR_INVALID_STATE;
    }
    bool enabled;
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    enabled = s_cfg.enabled;
    xSemaphoreGive(s_cfg_mtx);
    if (!enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t kvm_atx_power_click(void) { return queue_cmd(ATX_CMD_CLICK); }
esp_err_t kvm_atx_power_hold(void) { return queue_cmd(ATX_CMD_HOLD); }
esp_err_t kvm_atx_reset(void) { return queue_cmd(ATX_CMD_RESET); }
