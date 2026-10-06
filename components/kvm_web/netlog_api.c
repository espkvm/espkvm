/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The target's log as received over the network (netconsole / syslog):
 *
 *   GET /api/v1/netlog          state: listening, port, packets, last sender
 *   GET /api/v1/netlog/log      ?since=N for what came after position N (the
 *                               reply's X-Log-Pos is where to ask from next);
 *                               without it, all 64 KB kept, as a file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"

#include "kvm_auth.h"
#include "kvm_netlog.h"
#include "kvm_settings.h"
#include "web_priv.h"

static esp_err_t netlog_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_netlog_status_t st;
    kvm_netlog_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", kvm_setting_bool("nc_enable"));
    cJSON_AddBoolToObject(root, "running", st.running);
    cJSON_AddNumberToObject(root, "port", st.port);
    cJSON_AddNumberToObject(root, "rxBytes", (double)st.rx_bytes);
    cJSON_AddNumberToObject(root, "packets", st.packets);
    cJSON_AddNumberToObject(root, "refused", st.refused);
    cJSON_AddStringToObject(root, "lastFrom", st.last_from);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t netlog_log_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char q[48] = {0};
    char v[24] = {0};
    const bool live = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
                      httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK;
    char *buf = heap_caps_malloc(KVM_NETLOG_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return send_json_error(req, "503 Service Unavailable", "no memory");
    }
    uint64_t pos = live ? strtoull(v, NULL, 10) : 0;
    const size_t n = kvm_netlog_read_from(&pos, buf, KVM_NETLOG_RING_BYTES, NULL);
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%llu", (unsigned long long)pos);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Log-Pos", hdr);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!live) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"target-netlog.txt\"");
    }
    const esp_err_t r = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return r;
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/api/v1/netlog", .method = HTTP_GET, .handler = netlog_get},
    {.uri = "/api/v1/netlog/log", .method = HTTP_GET, .handler = netlog_log_get},
};

const httpd_uri_t *netlog_api_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
