/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * "New firmware" in the notification table: once a day, the device reads the
 * update manifest and, when it names a newer release than the one running,
 * says so on the channels that take it. Once per release: the last one told
 * about is kept in NVS, so a restart does not say it again.
 *
 * It asks only with the update check on and a channel ticked for it, which
 * already means a way out to the internet. Otherwise it stays quiet, as the
 * device always has.
 */
#include "fw_install.h"
#include "fw_version.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_notify.h"
#include "kvm_settings.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "fw_watch";

#define NVS_NS "fw_watch"
#define NVS_TOLD "told"
#define TICK_US (60LL * 60 * 1000000)       /* look every hour, the first an hour after boot */
#define EVERY_US (24LL * 60 * 60 * 1000000) /* then once a day */
#define RETRY_US (60LL * 60 * 1000000)      /* a failed check again in an hour */

static int64_t s_next_us; /* 0: at the next tick */
static volatile bool s_running;

static void told_get(char *out, size_t cap)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, NVS_TOLD, out, &len) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(h);
    }
}

static void told_set(const char *v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, NVS_TOLD, v) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

static void check_task(void *arg)
{
    (void)arg;
    char latest[FW_INSTALL_VERSION_MAX];
    const esp_err_t err = fw_latest_version(latest, sizeof(latest));
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "release check: %s", esp_err_to_name(err));
        s_next_us = esp_timer_get_time() + RETRY_US;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    s_next_us = esp_timer_get_time() + EVERY_US;

    const esp_app_desc_t *app = esp_app_get_description();
    const char *running = app ? app->version : "";
    int cmp = 0;
    char told[FW_INSTALL_VERSION_MAX];
    told_get(told, sizeof(told));
    if (fw_version_cmp(latest, running, &cmp) && cmp > 0 && strcmp(latest, told) != 0) {
        char body[320];
        int o = snprintf(body, sizeof(body),
                         "ESP-KVM %s is out. This device runs %s. Open the console to install it.",
                         latest, running);
        /* The project's own builds have release notes; a fork's manifest may not. */
        if (strncmp(kvm_setting_str("upd_url"), "https://fw.espkvm.io/", 21) == 0 && o > 0 &&
            (size_t)o < sizeof(body)) {
            snprintf(body + o, sizeof(body) - o,
                     " What changed: https://github.com/espkvm/espkvm/releases/tag/%s", latest);
        }
        ESP_LOGI(TAG, "%s", body);
        kvm_notify_send(KVM_NOTIFY_RELEASE, "New firmware", body, false);
        told_set(latest);
    }
    s_running = false;
    vTaskDelete(NULL);
}

static void tick(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    if (s_running || now < s_next_us) {
        return;
    }
    if (!kvm_setting_bool("upd_check") || !kvm_notify_routes(KVM_NOTIFY_RELEASE)) {
        return; /* nobody asked: no request goes out */
    }
    s_running = true;
    /* HTTPS with a 20 s timeout: not on the timer task. */
    if (xTaskCreate(check_task, "fw_watch", 8192, NULL, 2, NULL) != pdPASS) {
        s_running = false;
        s_next_us = now + RETRY_US;
        ESP_LOGW(TAG, "no memory for the release check");
    }
}

void fw_watch_start(void)
{
    static esp_timer_handle_t s_timer;
    if (s_timer) {
        return;
    }
    const esp_timer_create_args_t args = {.callback = tick, .name = "fw_watch"};
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        /* Hourly, so a check missed while the box was unticked waits at most an hour. */
        esp_timer_start_periodic(s_timer, TICK_US);
    }
}
