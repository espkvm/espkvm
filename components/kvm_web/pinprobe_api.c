/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A diagnostic: how fast each of some pins is switching. GET
 * /api/v1/system/pinprobe?pins=19,20,21 counts rising edges on each pin for a
 * few milliseconds with the pulse counter and answers the frequency, plus the
 * level the pin sat at. Made to find out whether a capture board puts its HDMI
 * audio (I2S: a word clock at exactly 44.1 or 48 kHz beside a bit clock in the
 * megahertz) on any pin the P4 can see.
 *
 * It only listens: the pulse counter takes the pin's input through the GPIO
 * matrix and leaves whatever drives the pin, and whatever it drives, alone.
 * The one exception is &test=<pin>, which drives that pin with a 48 kHz square
 * wave for the length of the request - a known signal, to prove the probe
 * counts. Give it a pin nothing is wired to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "kvm_auth.h"
#include "web_priv.h"

#define PROBE_US 5000 /* 3 MHz x 5 ms = 15000 edges, inside the 16-bit counter */
#define MAX_PINS 24

static int probe_pin(int pin, int *hz, int *level)
{
    pcnt_unit_config_t ucfg = {.low_limit = -32768, .high_limit = 32767};
    pcnt_unit_handle_t unit = NULL;
    if (pcnt_new_unit(&ucfg, &unit) != ESP_OK) {
        return -1;
    }
    pcnt_chan_config_t ccfg = {.edge_gpio_num = pin, .level_gpio_num = -1};
    pcnt_channel_handle_t chan = NULL;
    int rc = -1;
    if (pcnt_new_channel(unit, &ccfg, &chan) == ESP_OK &&
        pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) == ESP_OK &&
        pcnt_unit_enable(unit) == ESP_OK) {
        pcnt_unit_clear_count(unit);
        pcnt_unit_start(unit);
        const int64_t t0 = esp_timer_get_time();
        esp_rom_delay_us(PROBE_US);
        int count = 0;
        pcnt_unit_get_count(unit, &count);
        const int64_t dt = esp_timer_get_time() - t0;
        pcnt_unit_stop(unit);
        pcnt_unit_disable(unit);
        *hz = dt > 0 ? (int)((int64_t)count * 1000000 / dt) : 0;
        *level = gpio_get_level((gpio_num_t)pin);
        rc = 0;
    }
    if (chan) {
        pcnt_del_channel(chan);
    }
    pcnt_del_unit(unit);
    return rc;
}

static esp_err_t pinprobe_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char q[160] = "", list[140] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "pins", list, sizeof(list)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "give ?pins=19,20,21");
    }
    char tv[8] = "";
    int test_pin = -1;
    if (httpd_query_key_value(q, "test", tv, sizeof(tv)) == ESP_OK) {
        test_pin = atoi(tv);
        const ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                                       .timer_num = LEDC_TIMER_3, .freq_hz = 48000, .clk_cfg = LEDC_AUTO_CLK};
        const ledc_channel_config_t c = {.gpio_num = test_pin, .speed_mode = LEDC_LOW_SPEED_MODE,
                                         .channel = LEDC_CHANNEL_7, .timer_sel = LEDC_TIMER_3, .duty = 128};
        if (test_pin < 0 || test_pin >= GPIO_NUM_MAX || ledc_timer_config(&t) != ESP_OK ||
            ledc_channel_config(&c) != ESP_OK) {
            return send_json_error(req, "400 Bad Request", "cannot drive that test pin");
        }
    }
    char out[MAX_PINS * 64 + 16];
    size_t n = (size_t)snprintf(out, sizeof(out), "[");
    int done = 0;
    for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok && done < MAX_PINS;
         tok = strtok_r(NULL, ",", &save)) {
        const int pin = atoi(tok);
        if (pin < 0 || pin >= GPIO_NUM_MAX) {
            continue;
        }
        int hz = 0, level = 0;
        const int rc = probe_pin(pin, &hz, &level);
        n += (size_t)snprintf(out + n, sizeof(out) - n, "%s{\"pin\":%d,\"hz\":%d,\"level\":%d%s}",
                              done ? "," : "", pin, rc ? -1 : hz, rc ? -1 : level,
                              rc ? ",\"error\":\"no pulse counter\"" : "");
        done++;
    }
    snprintf(out + n, sizeof(out) - n, "]");
    if (test_pin >= 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
        gpio_reset_pin((gpio_num_t)test_pin);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/api/v1/system/pinprobe", .method = HTTP_GET, .handler = pinprobe_get},
};

const httpd_uri_t *pinprobe_api_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
