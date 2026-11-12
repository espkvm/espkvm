/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TLS: the operator-supplied certificate and Let's Encrypt.
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

/* ---- TLS: operator-supplied certificate --------------------------------- */

#define TLS_BODY_MAX 8192

/** Report whether the served certificate is the operator's or the self-signed one. */
esp_err_t api_tls_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const bool byo = kvm_tls_byo_present();
    char acme[640];
    kvm_acme_status_json(acme, sizeof(acme));
    char body[768];
    snprintf(body, sizeof(body), "{\"https\":%s,\"custom\":%s,\"acme\":%s}",
             kvm_setting_bool("sec_https") ? "true" : "false", byo ? "true" : "false",
             acme[0] ? acme : "null");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/** Get or renew the Let's Encrypt certificate now. {"staging":true} uses the test CA. */
esp_err_t api_tls_acme_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char buf[64] = "";
    if (req->content_len > 0 && req->content_len < (int)sizeof(buf)) {
        (void)httpd_req_recv(req, buf, (size_t)req->content_len);
    }
    const bool staging = strstr(buf, "\"staging\":true") != NULL;
    esp_err_t res = kvm_acme_run(staging);
    if (res == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "already running");
    }
    if (res != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(res));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"started\"}");
}

/** The start of the private-key block within a combined PEM, or NULL. */
static const char *pem_key_start(const char *pem)
{
    static const char *const markers[] = {
        "-----BEGIN PRIVATE KEY-----",
        "-----BEGIN EC PRIVATE KEY-----",
        "-----BEGIN RSA PRIVATE KEY-----",
        "-----BEGIN ENCRYPTED PRIVATE KEY-----",
    };
    const char *best = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        const char *p = strstr(pem, markers[i]);
        if (p && (!best || p < best)) {
            best = p;
        }
    }
    return best;
}

/*
 * Install an operator certificate. The body is one PEM blob: the certificate
 * (leaf first, any intermediates after) followed by the matching private key -
 * exactly `cat fullchain.pem privkey.pem`. It is validated before it is stored,
 * and only takes effect on the restart this triggers; if the TLS stack later
 * rejects it, start-up falls back to the self-signed certificate.
 */
esp_err_t api_tls_cert_put(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (req->content_len <= 0 || req->content_len >= TLS_BODY_MAX) {
        return send_json_error(req, "413 Payload Too Large", "certificate body too large");
    }
    char *body = malloc((size_t)req->content_len + 1u);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (kvm_recv_stalled(n) && ++stalls <= KVM_RECV_MAX_STALLS) {
            continue;
        }
        if (n <= 0) {
            free(body);
            return send_json_error(req, "400 Bad Request", "truncated body");
        }
        stalls = 0;
        got += n;
    }
    body[got] = '\0';

    const char *ks = pem_key_start(body);
    if (!ks) {
        free(body);
        return send_json_error(req, "400 Bad Request",
                               "no private key in the body (send the certificate then the key)");
    }
    const size_t cert_len = (size_t)(ks - body);
    char *cert = malloc(cert_len + 1u);
    char *key = strdup(ks);
    if (!cert || !key) {
        free(cert);
        free(key);
        free(body);
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    memcpy(cert, body, cert_len);
    cert[cert_len] = '\0';
    free(body);

    char err[128];
    esp_err_t res = kvm_tls_byo_set(cert, key, err, sizeof(err));
    free(cert);
    free(key);
    if (res != ESP_OK) {
        const char *status = (res == ESP_ERR_INVALID_SIZE) ? "413 Payload Too Large"
                                                            : "400 Bad Request";
        return send_json_error(req, status, err);
    }
    ESP_LOGW(TAG, "operator certificate installed; restarting to apply");
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"status\":\"stored\",\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return sent;
}

/** Remove the operator certificate and revert to the self-signed one. */
esp_err_t api_tls_cert_delete(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (!kvm_tls_byo_present()) {
        return send_json_error(req, "404 Not Found", "no operator certificate is installed");
    }
    esp_err_t res = kvm_tls_byo_clear();
    if (res != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(res));
    }
    ESP_LOGW(TAG, "operator certificate removed; restarting to apply");
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"status\":\"cleared\",\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return sent;
}
