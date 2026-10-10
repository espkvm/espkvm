/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One task looks at the clock about every twenty seconds. When the wall clock
 * is set (over SNTP) and the minute has turned, it walks the schedules and
 * fires the ones whose cron matches this minute - at most once a minute, so a
 * job cannot run twice. The scripts live in the `schedules_json` setting, the
 * same shape as macros and runbooks.
 */
#include "kvm_sched.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_atx.h"
#include "kvm_caps.h"
#include "kvm_record.h"
#include "kvm_settings.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "runbook.h"
#include "sched_cron.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "sched";

#define TASK_STACK 5120
#define TASK_PRIO 4
#define TICK_MS 20000
/* A real time is anything after 2020; before SNTP the clock sits at the epoch. */
#define CLOCK_SET_AFTER 1600000000

static SemaphoreHandle_t s_lock;
static kvm_sched_status_t s_status;
static bool s_sntp_up;
static char s_tz[40];
static int s_last_min = -1; /* the minute last evaluated; -1 = none yet */

static void status_last(const char *name, const char *action)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.last_name, name, sizeof(s_status.last_name));
    strlcpy(s_status.last_action, action, sizeof(s_status.last_action));
    strftime(s_status.last_at, sizeof(s_status.last_at), "%Y-%m-%d %H:%M:%S", &t);
    xSemaphoreGive(s_lock);
}

/*
 * The button's network actions. net_mode only takes effect on a restart, so
 * each one saves the new mode and restarts. Indexes are s_netmode_choices:
 * 0 ethernet, 1 wifi, 2 ap, 3 auto.
 */
#define NET_ETH 0
#define NET_WIFI 1
#define NET_AP 2
#define NET_AUTO 3

static int32_t net_prev_load(int32_t fallback)
{
    nvs_handle_t h;
    int32_t v = fallback;
    if (nvs_open("kvm_sched", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i32(h, "net_prev", &v);
        nvs_close(h);
    }
    return (v >= 0 && v <= NET_AUTO && v != NET_AP) ? v : fallback;
}

static void net_prev_save(int32_t v)
{
    nvs_handle_t h;
    if (nvs_open("kvm_sched", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_i32(h, "net_prev", v);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static bool net_switch(const char *name, const char *action)
{
    if (!kvm_cap_available(KVM_CAP_WIFI)) {
        ESP_LOGW(TAG, "%s: %s needs WiFi, and this board has none", name, action);
        return false;
    }
#if CONFIG_KVM_ETH_ENABLE
    const bool eth = true;
#else
    const bool eth = false;
#endif
    const int32_t now = kvm_setting_int("net_mode");
    int32_t next;
    if (strcmp(action, "hotspot") == 0) {
        /* To the hotspot, and back to whatever it was before. */
        if (now == NET_AP) {
            next = net_prev_load(eth ? NET_ETH : NET_WIFI);
        } else {
            net_prev_save(now);
            next = NET_AP;
        }
    } else if (strcmp(action, "netswap") == 0) {
        if (!eth) {
            ESP_LOGW(TAG, "%s: no Ethernet on this board to swap with", name);
            return false;
        }
        next = now == NET_ETH ? NET_WIFI : NET_ETH;
    } else {
        /* netnext: every mode the board has, in turn. */
        static const int32_t with_eth[] = {NET_ETH, NET_WIFI, NET_AP, NET_AUTO};
        static const int32_t without_eth[] = {NET_WIFI, NET_AP};
        const int32_t *order = eth ? with_eth : without_eth;
        const int n = eth ? 4 : 2;
        int at = 0;
        for (int i = 0; i < n; i++) {
            if (order[i] == now) {
                at = i;
            }
        }
        next = order[(at + 1) % n];
    }
    if (kvm_setting_set_int("net_mode", next) != ESP_OK) {
        ESP_LOGW(TAG, "%s: could not save the network mode", name);
        return false;
    }
    ESP_LOGW(TAG, "%s: network mode %d -> %d, restarting", name, (int)now, (int)next);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return true;
}

/* Carry out one action. Everything here is quick except a runbook, which starts
   its own task and returns at once. */
static void dispatch(const char *name, const char *action, const char *arg, const char *source)
{
    ESP_LOGI(TAG, "%s: %s%s%s (%s)", name, action, arg && arg[0] ? " " : "", arg ? arg : "", source);
    if (strcmp(action, "wol") == 0) {
        kvm_wol_send(kvm_setting_str("pwr_wol_mac"));
    } else if (strcmp(action, "power") == 0) {
        kvm_atx_power_click();
    } else if (strcmp(action, "reset") == 0) {
        kvm_atx_reset();
    } else if (strcmp(action, "poweroff") == 0) {
        kvm_atx_power_hold();
    } else if (strcmp(action, "runbook") == 0) {
        const esp_err_t err = runbook_start(arg ? arg : "", source, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s: runbook \"%s\": %s", name, arg ? arg : "", esp_err_to_name(err));
        }
    } else if (strcmp(action, "clip") == 0) {
        char why[96] = "";
        if (!kvm_record_event(source, why, sizeof(why))) {
            ESP_LOGW(TAG, "%s: no clip saved: %s", name, why);
        }
    } else if (strcmp(action, "screenshot") == 0) {
        char file[64] = "", why[96] = "";
        if (kvm_record_screenshot(file, sizeof(file), why, sizeof(why)) != ESP_OK) {
            ESP_LOGW(TAG, "%s: no screenshot: %s", name, why);
        }
    } else if (strcmp(action, "hotspot") == 0 || strcmp(action, "netnext") == 0 ||
               strcmp(action, "netswap") == 0) {
        if (!net_switch(name, action)) {
            return;
        }
    } else if (strcmp(action, "restart") == 0) {
        ESP_LOGW(TAG, "%s: restarting the device on schedule", name);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "%s: unknown action \"%s\"", name, action);
        return;
    }
    status_last(name, action);
}

/* Walk the schedules and fire the ones matching @p t. */
static uint16_t evaluate(const struct tm *t)
{
    cJSON *list = cJSON_Parse(kvm_setting_str("schedules_json"));
    uint16_t count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        count++;
        const cJSON *jn = cJSON_GetObjectItem(item, "name");
        const cJSON *jc = cJSON_GetObjectItem(item, "cron");
        const cJSON *ja = cJSON_GetObjectItem(item, "action");
        const cJSON *jarg = cJSON_GetObjectItem(item, "arg");
        const cJSON *je = cJSON_GetObjectItem(item, "enabled");
        if (je && !cJSON_IsTrue(je)) {
            continue;
        }
        if (!cJSON_IsString(jn) || !cJSON_IsString(jc) || !cJSON_IsString(ja)) {
            continue;
        }
        cron_t c;
        if (!cron_parse(jc->valuestring, &c, NULL, 0) || !cron_match(&c, t)) {
            continue;
        }
        dispatch(jn->valuestring, ja->valuestring, cJSON_IsString(jarg) ? jarg->valuestring : NULL,
                 "schedule");
    }
    cJSON_Delete(list);
    return count;
}

static void apply_tz(void)
{
    const char *tz = kvm_setting_str("sched_tz");
    if (tz[0] && strcmp(tz, s_tz) != 0) {
        strlcpy(s_tz, tz, sizeof(s_tz));
        setenv("TZ", s_tz, 1);
        tzset();
        s_last_min = -1; /* a zone change re-bases the minute */
    }
}

static void start_sntp(void)
{
    if (s_sntp_up || esp_sntp_enabled()) {
        s_sntp_up = true;
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, kvm_setting_str("sched_ntp"));
    esp_sntp_init();
    s_sntp_up = true;
    ESP_LOGI(TAG, "SNTP started (%s)", kvm_setting_str("sched_ntp"));
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        const bool on = kvm_setting_bool("sched_enable");
        /* The zone always: recordings are named in local time too. */
        apply_tz();
        if (on || kvm_setting_bool("time_sync")) {
            start_sntp();
        }
        const time_t now = time(NULL);
        const bool valid = now > CLOCK_SET_AFTER;

        struct tm t;
        localtime_r(&now, &t);
        char nowstr[24] = "";
        if (valid) {
            strftime(nowstr, sizeof(nowstr), "%Y-%m-%d %H:%M:%S", &t);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.enabled = on;
        s_status.clock_valid = valid;
        strlcpy(s_status.now, nowstr, sizeof(s_status.now));
        strlcpy(s_status.tz, s_tz, sizeof(s_status.tz));
        xSemaphoreGive(s_lock);

        if (on && valid && t.tm_min != s_last_min) {
            s_last_min = t.tm_min;
            const uint16_t count = evaluate(&t);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.count = count;
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void kvm_action_run(const char *name, const char *action, const char *arg, const char *source)
{
    if (!action || !action[0]) {
        return;
    }
    dispatch(name ? name : source, action, arg, source);
}

void kvm_sched_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    kvm_cap_report(KVM_CAP_SCHED, true, NULL);
    if (xTaskCreate(task, "sched", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the scheduler task");
    }
}

void kvm_sched_status(kvm_sched_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

esp_err_t kvm_sched_run(const char *name)
{
    if (!name || !*name) {
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *list = cJSON_Parse(kvm_setting_str("schedules_json"));
    esp_err_t rc = ESP_ERR_NOT_FOUND;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *jn = cJSON_GetObjectItem(item, "name");
        const cJSON *ja = cJSON_GetObjectItem(item, "action");
        const cJSON *jarg = cJSON_GetObjectItem(item, "arg");
        if (cJSON_IsString(jn) && cJSON_IsString(ja) && strcmp(jn->valuestring, name) == 0) {
            dispatch(jn->valuestring, ja->valuestring,
                     cJSON_IsString(jarg) ? jarg->valuestring : NULL, "console");
            rc = ESP_OK;
            break;
        }
    }
    cJSON_Delete(list);
    return rc;
}
