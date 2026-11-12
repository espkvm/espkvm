/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware updates: an uploaded image, a published release the device fetches,
 * keep or roll back a trial, boot the other slot.
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

static const char *TAG = "web";

/* WS_D2C_UPDATE phases, mirrored from the enum below. */
#define UPDATE_RECEIVING 0
#define UPDATE_VERIFYING 1
#define UPDATE_RESTARTING 2
#define UPDATE_FAILED 3

/*
 * Restart shortly, from a timer rather than inline. Calling esp_restart() straight
 * from a request handler proved unreliable after a large OTA (issue #13): the
 * handler task can be mid-teardown with its TLS session, or blocked in the response
 * send (up to send_wait_timeout), so the reboot sometimes never happened. A one-shot
 * timer fires it from a clean context a moment after we reply.
 */
static void restart_soon_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void kvm_web_restart_soon(uint32_t delay_ms)
{
    /* Everyone watching gets the same warning, not only whoever asked. */
    ws_broadcast_update(UPDATE_RESTARTING, 100);
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = {.callback = restart_soon_cb, .name = "restart"};
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            esp_restart(); /* fall back to an inline restart if the timer won't create */
            return;
        }
    }
    (void)esp_timer_stop(timer);
    (void)esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
}

/*
 * Receive a firmware image and boot it next.
 *
 * The image is streamed straight into the inactive slot - it is larger than
 * any buffer this device can spare - and the active slot is untouched until
 * the whole thing has been written and verified. Combined with the rollback
 * the bootloader performs when a new image never confirms itself, the worst
 * outcome of a bad upload is one reboot back into what was running before.
 */
static esp_err_t api_system_update_post_body(httpd_req_t *req);

/* The stream slows down for the length of the upload (video_frame_upload_begin). */
esp_err_t api_system_update_post(httpd_req_t *req)
{
    video_frame_upload_begin();
    const esp_err_t ret = api_system_update_post_body(req);
    video_frame_upload_end();
    return ret;
}

/*
 * ?trial=N: keep the new image on trial for N seconds after it boots (see
 * kvm_ota_trial.h). 0 or absent: it confirms itself as before. -1: a value out
 * of range.
 */
static long update_trial_seconds(httpd_req_t *req)
{
    char q[32] = {0};
    char v[12] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "trial", v, sizeof(v)) != ESP_OK) {
        return 0;
    }
    const long secs = strtol(v, NULL, 10);
    if (secs != 0 && (secs < KVM_OTA_TRIAL_MIN_S || secs > KVM_OTA_TRIAL_MAX_S)) {
        return -1;
    }
    return secs;
}

/* A new image now would be written over the one a trial falls back to. */
static esp_err_t refuse_during_trial(httpd_req_t *req)
{
    return send_json_error(req, "409 Conflict",
                           "the last update is still on trial: keep it or roll it back first");
}

static esp_err_t api_system_update_post_body(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (kvm_ota_trial_left()) {
        return refuse_during_trial(req);
    }
    const long trial = update_trial_seconds(req);
    if (trial < 0) {
        return send_json_error(req, "400 Bad Request", "trial must be 60 to 3600 seconds");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return send_json_error(req, "409 Conflict", "no second app slot to write into");
    }
    if (req->content_len <= 0) {
        return send_json_error(req, "400 Bad Request", "empty body");
    }
    if ((size_t)req->content_len > target->size) {
        return send_json_error(req, "413 Payload Too Large", "image larger than the app slot");
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, (size_t)req->content_len, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "firmware update: %d bytes into %s", req->content_len, target->label);

    /*
     * Say so on the panel, if there is one.
     *
     * An update is the longest thing this device does with nothing to show for
     * it, and whoever started it may be looking at the box rather than at the
     * browser - which has just been asked to give up its video connection for
     * the duration. The screen is the one place that can answer "is it doing
     * anything?" without a network.
     */
    kvm_display_notice("UPDATE", "receiving", 0, 30000);
    ws_broadcast_update(UPDATE_RECEIVING, 0);

    char chunk[2048];
    int received = 0;
    int shown = -1;
    int stalls = 0;
    while (received < req->content_len) {
        const int want = (int)sizeof(chunk) < (req->content_len - received)
                             ? (int)sizeof(chunk)
                             : (req->content_len - received);
        const int n = httpd_req_recv(req, chunk, (size_t)want);
        if (kvm_recv_stalled(n)) {
            /* A gap in a multi-megabyte upload is normal: a slow sender, or a
             * device busy enough - live video - to starve the socket. */
            if (++stalls > KVM_RECV_MAX_STALLS) {
                esp_ota_abort(ota);
                kvm_display_notice("UPDATE", "upload lost", -1, 10000);
                ws_broadcast_update(UPDATE_FAILED, 0);
                ESP_LOGE(TAG, "update stalled after %d of %d bytes", received, req->content_len);
                return send_json_error(req, "408 Request Timeout", "the upload went quiet");
            }
            continue;
        }
        stalls = 0;
        if (n <= 0) {
            esp_ota_abort(ota);
            kvm_display_notice("UPDATE", "upload lost", -1, 10000);
            ws_broadcast_update(UPDATE_FAILED, 0);
            ESP_LOGE(TAG, "update aborted after %d of %d bytes (recv %d)", received,
                     req->content_len, n);
            return send_json_error(req, "400 Bad Request", "upload was cut short");
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            kvm_display_notice("UPDATE", "write failed", -1, 10000);
            ws_broadcast_update(UPDATE_FAILED, 0);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
        }
        received += n;
        /* In twentieths: the notice repaints only on a change, but there is no
         * reason to hand it 850 identical percentages to throw away. */
        const int pct = (int)((int64_t)received * 100 / req->content_len);
        if (pct / 5 != shown) {
            shown = pct / 5;
            kvm_display_notice("UPDATE", "receiving", pct, 30000);
            ws_broadcast_update(UPDATE_RECEIVING, (uint8_t)pct);
        }
    }

    kvm_display_notice("UPDATE", "verifying", 100, 30000);
    ws_broadcast_update(UPDATE_VERIFYING, 100);
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        /* Most often the image is not a valid application: a truncated upload
         * or the wrong file entirely. */
        kvm_display_notice("UPDATE", "bad image", -1, 10000);
            ws_broadcast_update(UPDATE_FAILED, 0);
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return send_json_error(req, "400 Bad Request",
                               err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a valid firmware image"
                                                                  : esp_err_to_name(err));
    }
    /* Before the switch: an update asked to be on trial must not start without one. */
    err = kvm_ota_trial_arm(target, (uint32_t)trial);
    if (err != ESP_OK) {
        kvm_display_notice("UPDATE", "failed", -1, 10000);
        ws_broadcast_update(UPDATE_FAILED, 0);
        ESP_LOGE(TAG, "kvm_ota_trial_arm: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "could not store the trial");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    kvm_display_notice("UPDATE", "restarting", 100, 20000);
    ESP_LOGW(TAG, "update written to %s, restarting", target->label);
    /* Arm the reboot before replying, so it fires even if the send below blocks or
     * the connection is already gone (issue #13). */
    kvm_web_restart_soon(1000);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"written\",\"restarting\":true}");
}

/*
 * Boot the other OTA slot on the next restart - the console's "boot from this
 * slot", to fall back to a known-good image or forward to one an OTA left
 * unconfirmed, without reflashing. Refused unless the target slot holds a valid
 * image, so a click can never leave the device booting an empty or bad slot.
 */
/*
 * Install a published release the device fetches for itself.
 *
 * The console can list what has been published - GitHub's API allows that read
 * from a browser - but it cannot fetch an image, because the host serving the
 * assets sends no cross-origin header. So the operator picks a version here and
 * the device goes and gets it. Going backwards is the point, but nothing here
 * cares about the direction: it installs whatever published version is asked
 * for, and the rollback the bootloader already does covers a bad landing.
 */
esp_err_t api_system_install_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_json_body(req);
    if (!j) {
        return send_json_error(req, "400 Bad Request", "expected JSON {version:\"v.X.Y.Z\"}");
    }
    const cJSON *jv = cJSON_GetObjectItem(j, "version");
    char version[FW_INSTALL_VERSION_MAX] = {0};
    if (cJSON_IsString(jv) && jv->valuestring) {
        snprintf(version, sizeof(version), "%s", jv->valuestring);
    }
    const cJSON *jt = cJSON_GetObjectItem(j, "trial");
    const double trial = cJSON_IsNumber(jt) ? jt->valuedouble : 0;
    cJSON_Delete(j);

    if (kvm_ota_trial_left()) {
        return refuse_during_trial(req);
    }
    if (trial != 0 && (trial < KVM_OTA_TRIAL_MIN_S || trial > KVM_OTA_TRIAL_MAX_S)) {
        return send_json_error(req, "400 Bad Request", "trial must be 60 to 3600 seconds");
    }
    const esp_err_t err = fw_install_start(version, (uint32_t)trial);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "400 Bad Request", "that is not a published version");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "an install is already running");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_json_error(req, "403 Forbidden",
                               "the device is not allowed to fetch releases itself "
                               "(turn it on in Settings > System)");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

/** How the fetch above is getting on, for a console that has to show it. */
esp_err_t api_system_install_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    fw_install_status_t st;
    fw_install_get_status(&st);
    static const char *const k_states[] = {"idle", "running", "done", "failed"};
    char body[256];
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"percent\":%d,\"version\":\"%s\",\"message\":\"%s\"}",
             k_states[st.state], st.percent, st.version, st.message);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* The running update is on trial and the operator says it works. */
esp_err_t api_system_update_keep_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const esp_err_t err = kvm_ota_trial_keep();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "no update is on trial");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"kept\"}");
}

/* The running update is on trial and the operator wants the old image back now. */
esp_err_t api_system_update_rollback_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (!kvm_ota_trial_left()) {
        return send_json_error(req, "409 Conflict", "no update is on trial");
    }
    kvm_ota_trial_rollback();
    ESP_LOGW(TAG, "update rolled back by the operator; restarting");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t sent = httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    kvm_web_restart_soon(300);
    return sent;
}

esp_err_t api_system_boot_slot_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_json_body(req);
    const cJSON *jl = j ? cJSON_GetObjectItem(j, "label") : NULL;
    char label[16] = {0};
    if (cJSON_IsString(jl) && jl->valuestring) {
        snprintf(label, sizeof(label), "%s", jl->valuestring);
    }
    if (j) {
        cJSON_Delete(j);
    }
    if (!label[0]) {
        return send_json_error(req, "400 Bad Request", "which slot? send {\"label\":\"ota_1\"}");
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!target || target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        target->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return send_json_error(req, "404 Not Found", "no such OTA slot");
    }
    if (running && target->address == running->address) {
        return send_json_error(req, "409 Conflict", "that slot is already running");
    }
    /* A slot with no valid app, or one the bootloader has marked bad, would brick
     * the next boot - refuse both. */
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(target, &d) != ESP_OK) {
        return send_json_error(req, "409 Conflict", "that slot has no firmware");
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(target, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) {
        return send_json_error(req, "409 Conflict", "that slot's image was marked bad");
    }
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition(%s): %s", label, esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", "could not switch slot");
    }
    ESP_LOGW(TAG, "boot slot switched to %s (%s); restarting", label, d.version);
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    /* From a timer, a moment after the reply reaches the browser (see kvm_web_restart_soon). */
    kvm_web_restart_soon(300);
    return sent;
}
