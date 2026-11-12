/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runbooks, schedules and notifications over REST.
 */
#include "http_server.h"

#include "sdkconfig.h" /* the CONFIG_ guards below are read before any of them */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "kvm_ota_trial.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

/* espcoredump only puts its header on the include path when core dumps are
 * enabled, so a build with them off must not ask for it. Everything that uses
 * it is behind the same guard. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

#include "esp_https_server.h"
#include "esp_tls.h"
#include "esp_netif.h"

#include "lwip/sockets.h"

#include "cJSON.h"

#include "http_recv.h"

#include "capture.h"
#include "screentext.h"
#include "screentext_store.h"
#include "ethernet.h"
#include "kvm_ipv6.h"
#include "wifi.h"
#include "coproc.h"
#include "kvm_rtc.h"
#include "kvm_atx.h"
#include "kvm_mqtt.h"
#include "kvm_ts.h"
#include "kvm_wg.h"
#include "kvm_auth.h"
#include "fw_install.h"
#include "kvm_notify.h"
#include "kvm_sched.h"
#include "runbook.h"
#include "kvm_board_header.h"
#include "kvm_caps.h"
#include "kvm_display.h"
#include "kvm_log.h"
#include "kvm_settings.h"
#include "kvm_profiles.h"
#include "kvm_record.h"
#include "web_priv.h"
#include "http_priv.h"
#include "kvm_storage.h"
#include "kvm_thermal.h"
#include "kvm_tls.h"
#include "kvm_acme.h"
#include "kvm_tools.h"
#include "kvm_proto.h"
#include "usb_hid.h"
#include "video_frame.h"

/* --- runbooks: scripted keys and screen waits, run on the device --------- */

esp_err_t api_runbooks_status_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    runbook_status_t st;
    runbook_get_status(&st);
    static const char *const k_states[] = {"idle", "running", "done", "failed", "stopped"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", k_states[st.state]);
    cJSON_AddStringToObject(j, "name", st.name);
    cJSON_AddNumberToObject(j, "step", st.step);
    cJSON_AddNumberToObject(j, "steps", st.steps);
    cJSON_AddStringToObject(j, "line", st.line);
    cJSON_AddStringToObject(j, "message", st.message);
    cJSON_AddNumberToObject(j, "elapsedMs", st.elapsed_ms);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_sendstr(req, body);
    free(body);
    return r;
}

esp_err_t api_runbooks_run_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_json_body(req);
    const cJSON *jn = j ? cJSON_GetObjectItem(j, "name") : NULL;
    char name[RUNBOOK_NAME_MAX] = {0};
    if (cJSON_IsString(jn) && jn->valuestring) {
        snprintf(name, sizeof(name), "%s", jn->valuestring);
    }
    cJSON_Delete(j);
    if (!name[0]) {
        return send_json_error(req, "400 Bad Request", "expected JSON {name:\"...\"}");
    }
    char why[RB_ERR_MAX];
    const esp_err_t err = runbook_start(name, "console", why, sizeof(why));
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "no runbook of that name");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "a runbook is already running");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_error(req, "409 Conflict", "no USB target attached");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "400 Bad Request", why);
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

/* "Reconnect HDMI" in the console: the reset the device no longer does on its
 * own where it cannot tell a sleeping screen from a stuck bridge. */
esp_err_t api_video_reconnect_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const esp_err_t err = capture_reconnect_source();
    if (err != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable", esp_err_to_name(err));
    }
    return send_ok(req);
}

esp_err_t api_runbooks_stop_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (runbook_stop() != ESP_OK) {
        return send_json_error(req, "409 Conflict", "no runbook is running");
    }
    return send_ok(req);
}

/* --- scheduler: cron lines that fire actions on the device --------------- */

esp_err_t api_schedules_status_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_sched_status_t st;
    kvm_sched_status(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", st.enabled);
    cJSON_AddBoolToObject(j, "clockValid", st.clock_valid);
    cJSON_AddStringToObject(j, "now", st.now);
    cJSON_AddStringToObject(j, "tz", st.tz);
    cJSON_AddNumberToObject(j, "count", st.count);
    cJSON_AddStringToObject(j, "lastName", st.last_name);
    cJSON_AddStringToObject(j, "lastAction", st.last_action);
    cJSON_AddStringToObject(j, "lastAt", st.last_at);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_sendstr(req, body);
    free(body);
    return r;
}

esp_err_t api_schedules_run_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_json_body(req);
    const cJSON *jn = j ? cJSON_GetObjectItem(j, "name") : NULL;
    char name[KVM_SCHED_NAME_MAX] = {0};
    if (cJSON_IsString(jn) && jn->valuestring) {
        snprintf(name, sizeof(name), "%s", jn->valuestring);
    }
    cJSON_Delete(j);
    if (!name[0]) {
        return send_json_error(req, "400 Bad Request", "expected JSON {name:\"...\"}");
    }
    if (kvm_sched_run(name) != ESP_OK) {
        return send_json_error(req, "404 Not Found", "no schedule of that name");
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"fired\"}");
}

/* --- notifications ------------------------------------------------------- */

esp_err_t api_notify_status_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_notify_status_t st;
    kvm_notify_status(&st);
    char res[96 * 2];
    /* The chat list rides along, so finding chats costs no route of its own. */
    const size_t chats_cap = 4096;
    char *chats = heap_caps_malloc(chats_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t body_cap = sizeof(res) + 256 + chats_cap;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chats || !body) {
        free(chats);
        free(body);
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    if (!kvm_notify_chats_json(chats, chats_cap)) {
        strlcpy(chats, "{\"state\":\"idle\",\"error\":\"\",\"bot\":\"\",\"chats\":[]}", chats_cap);
    }
    /* last_result may hold a URL or a quote; keep the JSON valid. */
    size_t o = 0;
    for (const char *p = st.last_result; *p && o + 2 < sizeof(res); p++) {
        if (*p == '"' || *p == '\\') {
            res[o++] = '\\';
        }
        res[o++] = *p;
    }
    res[o] = '\0';
    char ready[128];
    if (!kvm_notify_ready_json(ready, sizeof(ready))) {
        strlcpy(ready, "{}", sizeof(ready));
    }
    snprintf(body, body_cap,
             "{\"enabled\":%s,\"lastResult\":\"%s\",\"lastAt\":\"%s\",\"pending\":%d,\"ready\":%s,"
             "\"telegram\":%s}",
             st.enabled ? "true" : "false", res, st.last_at, st.pending, ready, chats);
    free(chats);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

esp_err_t api_notify_chats_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    (void)kvm_notify_find_chats();
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"queued\"}");
}

esp_err_t api_notify_test_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    /* {"channel":"notify_ntfy_ev"} tests one channel; no body tests them all. */
    char channel[24] = "";
    cJSON *j = read_json_body(req);
    if (j) {
        const cJSON *ch = cJSON_GetObjectItem(j, "channel");
        if (cJSON_IsString(ch) && ch->valuestring) {
            strlcpy(channel, ch->valuestring, sizeof(channel));
        }
        cJSON_Delete(j);
    }
    const esp_err_t err = kvm_notify_send_test(channel);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "400 Bad Request", "no such channel");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable", "the notification queue is full");
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"queued\"}");
}
