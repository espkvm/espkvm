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
 * Both are settings (atx_active_high, atx_led_ah) so a wrong guess is
 * a checkbox, not a reflash.
 */
#include "kvm_atx.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "kvm_board.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvmsw_tesmart.h"

static const char *TAG = "kvm_atx";

typedef enum {
    ATX_CMD_CLICK, /* short press, power button */
    ATX_CMD_HOLD,  /* long press, power button (hard off) */
    ATX_CMD_RESET, /* short press, reset button */
    ATX_CMD_SWITCH, /* one press of the KVM switch's select button */
} atx_cmd_t;

typedef struct {
    bool enabled;         /* atx_enable AND both button pins valid */
    int pwr_gpio;
    int rst_gpio;
    int led_gpio;         /* -1 when no LED sense is wired */
    int hdd_gpio;         /* -1 when no HDD LED sense is wired */
    int short_ms;
    int long_ms;
    bool active_high;     /* drive high to "press" the button */
    bool led_active_high; /* LED line reads high when the target is on */
    /* The KVM switch, independent of atx_enable. */
    int sw_gpio;          /* -1 when no switch is wired */
    int sw_led_gpio;      /* -1, or the first machine's LED on the switch */
    int sw_ms;
    int sw_ports;
    bool sw_direct;       /* wired straight to the button: open drain, low presses */
    bool sw_led_ah;
} atx_cfg_t;

static atx_cfg_t s_cfg;
static SemaphoreHandle_t s_cfg_mtx;
/* Serialises a physical press against a reconfigure: apply() must not reset a
 * pin out from under a hold-in-progress. Held for the whole press (up to the
 * hard-off duration), so it is separate from s_cfg_mtx, which stays a brief
 * struct guard that status reads without waiting on a five-second hold. Lock
 * order where both are taken: op before cfg. */
static SemaphoreHandle_t s_op_mtx;
static QueueHandle_t s_cmd_q;

/* Where the switch is: counted from the presses, set right by its LED. Starts
   on the first machine, where such switches start when powered. s_sw_queued
   counts presses waiting in the queue, so a select adds to them. */
static volatile int s_sw_port;
static volatile int s_sw_queued;
static portMUX_TYPE s_sw_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_sw_mode;      /* kvmsw_mode as last applied */
static int s_sw_net_ports; /* kvmsw_ports, for the network switch */

/* The HDD LED blinks for each burst of disk work, far too briefly for a status
 * poll once a second to see the level. An edge interrupt notes when it last
 * changed, and the status calls the disk busy for a moment after. Milliseconds
 * since boot, 32 bits so the ISR's store is one instruction. */
static volatile uint32_t s_hdd_edge_ms;

static void IRAM_ATTR hdd_edge(void *arg)
{
    (void)arg;
    s_hdd_edge_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* Idle level of a button output: whichever level does NOT press it. */
static inline int idle_level(bool active_high) { return active_high ? 0 : 1; }
static inline int press_level(bool active_high) { return active_high ? 1 : 0; }

static void atx_task(void *arg)
{
    (void)arg;
    atx_cmd_t cmd;
    while (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) == pdTRUE) {
        /* Own the pins for the whole press: apply() waits on this before it may
         * reset or reconfigure them. The config snapshot is taken inside the
         * same section, so a reconfigure cannot swap the pin numbers between the
         * snapshot and the drive. */
        xSemaphoreTake(s_op_mtx, portMAX_DELAY);
        atx_cfg_t c;
        xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
        c = s_cfg;
        xSemaphoreGive(s_cfg_mtx);
        if (cmd == ATX_CMD_SWITCH) {
            if (c.sw_gpio >= 0) {
                gpio_set_level(c.sw_gpio, press_level(!c.sw_direct));
                vTaskDelay(pdMS_TO_TICKS(c.sw_ms));
                gpio_set_level(c.sw_gpio, idle_level(!c.sw_direct));
                /* The switch needs a moment before it takes the next press, and
                   its LED a moment to follow. */
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            /* Both in one step, so port + queued never counts a press twice. */
            taskENTER_CRITICAL(&s_sw_mux);
            if (c.sw_gpio >= 0) {
                s_sw_port = (s_sw_port + 1) % c.sw_ports;
            }
            if (s_sw_queued > 0) {
                s_sw_queued--;
            }
            const int now_at = s_sw_port;
            taskEXIT_CRITICAL(&s_sw_mux);
            if (c.sw_gpio >= 0) {
                ESP_LOGI(TAG, "KVM switch: machine %d", now_at + 1);
            }
            xSemaphoreGive(s_op_mtx);
            continue;
        }
        if (!c.enabled) {
            xSemaphoreGive(s_op_mtx);
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
        xSemaphoreGive(s_op_mtx);
    }
    vTaskDelete(NULL);
}

esp_err_t kvm_atx_init(void)
{
    if (s_cfg_mtx) {
        return ESP_OK; /* already initialised */
    }
    s_cfg_mtx = xSemaphoreCreateMutex();
    s_op_mtx = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(20, sizeof(atx_cmd_t)); /* up to 15 switch presses and ATX ones */
    if (!s_cfg_mtx || !s_op_mtx || !s_cmd_q) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.pwr_gpio = s_cfg.rst_gpio = s_cfg.led_gpio = s_cfg.hdd_gpio = -1;
    s_cfg.sw_gpio = s_cfg.sw_led_gpio = -1;
    if (xTaskCreate(atx_task, "atx", 2048, NULL, 5, NULL) != pdPASS) {
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

/* The switch's button. Wired straight to it, the pin pulls the button's line
   to ground like a finger would, and lets go to high impedance, so it never
   pushes its own 3.3 V into the switch. Through an optocoupler module it
   drives high to press, like the ATX outputs. */
static esp_err_t configure_switch(int gpio, bool direct)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = direct ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(gpio, idle_level(!direct));
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        gpio_set_level(gpio, idle_level(!direct));
    }
    return err;
}

/* Same wiring as the power LED, with an interrupt on every change. */
static esp_err_t configure_hdd(int gpio, bool active_high)
{
    esp_err_t err = configure_led(gpio, active_high);
    if (err != ESP_OK) {
        return err;
    }
    err = kvm_board_gpio_isr_service();
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(gpio, hdd_edge, NULL);
    }
    return err;
}

static void release_hdd(int gpio)
{
    if (gpio >= 0) {
        gpio_isr_handler_remove(gpio);
        gpio_reset_pin(gpio);
    }
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
    const int hdd = (int)kvm_setting_int("atx_hdd_gpio");
    const int short_ms = (int)kvm_setting_int("atx_short_ms");
    const int long_ms = (int)kvm_setting_int("atx_long_ms");
    const bool active_high = kvm_setting_bool("atx_active_high");
    const bool led_active_high = kvm_setting_bool("atx_led_ah");

    /* 0 off, 1 a button on a GPIO, 2 a TESmart switch over the network. */
    const int sw_mode = (int)kvm_setting_int("kvmsw_mode");
    const int sw = sw_mode == 1 ? (int)kvm_setting_int("kvmsw_gpio") : -1;
    const int sw_led = (int)kvm_setting_int("kvmsw_led_gpio");
    const int sw_ms = (int)kvm_setting_int("kvmsw_ms");
    int sw_ports = (int)kvm_setting_int("kvmsw_ports");
    const bool sw_direct = kvm_setting_bool("kvmsw_direct");
    const bool sw_led_ah = kvm_setting_bool("kvmsw_led_ah");
    if (sw_ports < 2) {
        sw_ports = 2;
    }
    /* "host" or "host:port"; TESmart's own port is 5000. */
    char sw_host[64];
    strlcpy(sw_host, kvm_setting_str("kvmsw_host"), sizeof(sw_host));
    int sw_tport = 5000;
    char *colon = strchr(sw_host, ':');
    if (colon && colon == strrchr(sw_host, ':')) { /* one colon: a port, not IPv6 */
        sw_tport = atoi(colon + 1);
        *colon = '\0';
    }
    s_sw_mode = sw_mode;
    s_sw_net_ports = sw_ports;
    kvmsw_tesmart_apply(sw_mode == 2, sw_host, sw_tport, sw_ports);

    const bool pins_valid = pwr >= 0 && rst >= 0;
    const bool want_hw = want_enable && pins_valid;

    /* Wait for any press in flight to finish before touching the pins (op before
     * cfg, the module's lock order), so a reconfigure never resets a GPIO that
     * the worker is mid-hold on. */
    xSemaphoreTake(s_op_mtx, portMAX_DELAY);
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);

    /* Drop whatever we held before; a pin may have moved or been switched off. */
    release_pin(s_cfg.pwr_gpio);
    release_pin(s_cfg.rst_gpio);
    release_pin(s_cfg.led_gpio);
    release_hdd(s_cfg.hdd_gpio);
    release_pin(s_cfg.sw_gpio);
    release_pin(s_cfg.sw_led_gpio);
    s_cfg.enabled = false;
    s_cfg.pwr_gpio = s_cfg.rst_gpio = s_cfg.led_gpio = s_cfg.hdd_gpio = -1;
    s_cfg.sw_gpio = s_cfg.sw_led_gpio = -1;

    /* The switch works with or without the ATX wiring. */
    if (sw >= 0) {
        esp_err_t e = configure_switch(sw, sw_direct);
        if (e == ESP_OK && sw_led >= 0) {
            e = configure_led(sw_led, sw_led_ah);
        }
        if (e == ESP_OK) {
            s_cfg.sw_gpio = sw;
            s_cfg.sw_led_gpio = sw_led;
            s_cfg.sw_ms = sw_ms;
            s_cfg.sw_ports = sw_ports;
            s_cfg.sw_direct = sw_direct;
            s_cfg.sw_led_ah = sw_led_ah;
            taskENTER_CRITICAL(&s_sw_mux);
            if (s_sw_port >= sw_ports) {
                s_sw_port = 0;
            }
            taskEXIT_CRITICAL(&s_sw_mux);
        } else {
            ESP_LOGE(TAG, "KVM switch GPIO setup failed: %s", esp_err_to_name(e));
            release_pin(sw);
            release_pin(sw_led);
        }
    }

    esp_err_t err = ESP_OK;
    if (want_hw) {
        err = configure_output(pwr, active_high);
        if (err == ESP_OK) {
            err = configure_output(rst, active_high);
        }
        if (err == ESP_OK && led >= 0) {
            err = configure_led(led, led_active_high);
        }
        if (err == ESP_OK && hdd >= 0) {
            err = configure_hdd(hdd, led_active_high);
        }
        if (err == ESP_OK) {
            s_cfg.enabled = true;
            s_cfg.pwr_gpio = pwr;
            s_cfg.rst_gpio = rst;
            s_cfg.led_gpio = led;
            s_cfg.hdd_gpio = hdd;
            s_cfg.short_ms = short_ms;
            s_cfg.long_ms = long_ms;
            s_cfg.active_high = active_high;
            s_cfg.led_active_high = led_active_high;
        } else {
            ESP_LOGE(TAG, "GPIO setup failed: %s", esp_err_to_name(err));
            release_pin(pwr);
            release_pin(rst);
            release_pin(led);
            release_hdd(hdd);
        }
    }

    xSemaphoreGive(s_cfg_mtx);
    xSemaphoreGive(s_op_mtx);

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
    out->have_hdd = s_cfg.enabled && s_cfg.hdd_gpio >= 0;
    if (out->have_hdd) {
        const int level = gpio_get_level(s_cfg.hdd_gpio);
        const bool lit = s_cfg.led_active_high ? (level != 0) : (level == 0);
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        /* Lit now, or blinked within the last poll and a half. */
        out->hdd_busy = lit || (s_hdd_edge_ms != 0 && now_ms - s_hdd_edge_ms < 1500);
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

/* Read the first machine's LED, when wired, and put the count right: someone
   may have pressed the switch by hand. Only while no press is under way, as
   the LED follows a press with a delay. Called with s_cfg_mtx held. */
static void sw_sync_locked(void)
{
    if (s_cfg.sw_gpio < 0 || s_cfg.sw_led_gpio < 0 || s_sw_queued > 0) {
        return;
    }
    const int level = gpio_get_level(s_cfg.sw_led_gpio);
    const bool first = s_cfg.sw_led_ah ? (level != 0) : (level == 0);
    taskENTER_CRITICAL(&s_sw_mux);
    if (first) {
        s_sw_port = 0;
    } else if (s_sw_port == 0) {
        /* Not the first: with two machines that is the second. With more it
           is a guess, and the next one is where a press by hand goes. */
        s_sw_port = 1;
    }
    taskEXIT_CRITICAL(&s_sw_mux);
}

void kvm_kvmsw_status(kvm_kvmsw_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_cfg_mtx) {
        return;
    }
    if (s_sw_mode == 2) {
        int m;
        kvmsw_tesmart_status(&m, &out->busy, &out->error);
        out->enabled = true;
        out->ports = s_sw_net_ports;
        out->port = m;
        return;
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    out->enabled = s_cfg.sw_gpio >= 0;
    if (out->enabled) {
        sw_sync_locked();
        out->ports = s_cfg.sw_ports;
        out->port = s_sw_port;
        out->busy = s_sw_queued > 0;
    }
    xSemaphoreGive(s_cfg_mtx);
}

esp_err_t kvm_kvmsw_select(int port)
{
    if (!s_cmd_q || !s_cfg_mtx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sw_mode == 2) {
        if (port < 0 || port >= s_sw_net_ports) {
            return ESP_ERR_INVALID_ARG;
        }
        return kvmsw_tesmart_select(port);
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    const bool on = s_cfg.sw_gpio >= 0;
    const int ports = s_cfg.sw_ports;
    if (on) {
        sw_sync_locked();
    }
    xSemaphoreGive(s_cfg_mtx);
    if (!on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (port < 0 || port >= ports) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Count from where the queued presses will leave it. */
    taskENTER_CRITICAL(&s_sw_mux);
    const int presses = ((port - s_sw_port - s_sw_queued) % ports + ports) % ports;
    s_sw_queued += presses;
    taskEXIT_CRITICAL(&s_sw_mux);
    const atx_cmd_t cmd = ATX_CMD_SWITCH;
    for (int i = 0; i < presses; i++) {
        if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
            taskENTER_CRITICAL(&s_sw_mux);
            s_sw_queued -= presses - i;
            taskEXIT_CRITICAL(&s_sw_mux);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t kvm_kvmsw_next(void)
{
    kvm_kvmsw_status_t st;
    kvm_kvmsw_status(&st);
    if (!st.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    int at;
    if (s_sw_mode == 2) {
        at = st.port < 0 ? -1 : st.port; /* not known yet: the first machine */
    } else {
        taskENTER_CRITICAL(&s_sw_mux);
        at = s_sw_port + s_sw_queued;
        taskEXIT_CRITICAL(&s_sw_mux);
    }
    return kvm_kvmsw_select((at + 1) % st.ports);
}

esp_err_t kvm_atx_power_click(void) { return queue_cmd(ATX_CMD_CLICK); }
esp_err_t kvm_atx_power_hold(void) { return queue_cmd(ATX_CMD_HOLD); }
esp_err_t kvm_atx_reset(void) { return queue_cmd(ATX_CMD_RESET); }
