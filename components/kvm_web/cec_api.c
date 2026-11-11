/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI-CEC over REST: who is on the line, and the remote.
 *
 *   GET  /api/v1/cec        the table, the target, the recent frames
 *   POST /api/v1/cec/key    {"key":"up"} or {"code":1}, optional "la"
 *   POST /api/v1/cec/power  {"action":"standby"|"wake"}, optional "la" (15 = everyone)
 *   POST /api/v1/cec/send   {"hex":"04 8f"} - one raw frame, waits for the verdict
 *   POST /api/v1/cec/scan   look for devices again
 *
 * Without "la" an action goes to the active source, else the first playback
 * device. Key, power and scan only queue their frames and answer at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_recv.h"
#include "cJSON.h"
#include "kvm_auth.h"
#include "kvm_caps.h"
#include "kvm_cec.h"
#include "web_priv.h"

static const char *type_name(int t)
{
    switch (t) {
    case 0: return "tv";
    case 1: return "recorder";
    case 3: return "tuner";
    case 4: return "playback";
    case 5: return "audio";
    case 6: return "switch";
    case 7: return "processor";
    default: return "";
    }
}

static const char *power_name(int p)
{
    switch (p) {
    case KVM_CEC_POWER_ON: return "on";
    case KVM_CEC_POWER_STANDBY: return "standby";
    case KVM_CEC_POWER_TO_ON: return "waking";
    case KVM_CEC_POWER_TO_STANDBY: return "going to standby";
    default: return "unknown";
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t cec_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_cec_status_t *st = malloc(sizeof(*st));
    if (!st) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    kvm_cec_status(st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "available", kvm_cap_available(KVM_CAP_CEC));
    cJSON_AddBoolToObject(root, "running", st->running);
    cJSON_AddNumberToObject(root, "ownAddr", st->own_addr);
    cJSON_AddNumberToObject(root, "active", st->active);
    cJSON_AddNumberToObject(root, "target", kvm_cec_target());
    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int la = 0; la < KVM_CEC_DEVICES; la++) {
        const kvm_cec_device_t *d = &st->dev[la];
        if (!d->present || la == st->own_addr) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "la", la);
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddStringToObject(o, "type", type_name(d->type));
        char buf[16];
        if (d->phys_addr != 0xffff) {
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", d->phys_addr >> 12, (d->phys_addr >> 8) & 15,
                     (d->phys_addr >> 4) & 15, d->phys_addr & 15);
            cJSON_AddStringToObject(o, "physAddr", buf);
        }
        if (d->vendor != 0xffffffff) {
            snprintf(buf, sizeof(buf), "%06lx", (unsigned long)d->vendor);
            cJSON_AddStringToObject(o, "vendor", buf);
        }
        cJSON_AddStringToObject(o, "power", power_name(d->power));
        if (d->version >= 4) {
            static const char *v[] = {"1.3a", "1.4", "2.0"};
            cJSON_AddStringToObject(o, "version", d->version <= 6 ? v[d->version - 4] : "2.0+");
        }
        cJSON_AddNumberToObject(o, "seenMs", d->seen_ms);
        cJSON_AddItemToArray(devs, o);
    }
    cJSON *log = cJSON_AddArrayToObject(root, "log");
    static const char *res[] = {"ok", "nack", "arbitration lost", "error", "timeout"};
    for (int i = 0; i < st->log_count; i++) {
        const kvm_cec_log_t *e = &st->log[i];
        char hex[16 * 3 + 1] = {0};
        size_t at = 0;
        for (int k = 0; k < e->len; k++) {
            at += (size_t)snprintf(hex + at, sizeof(hex) - at, k ? " %02x" : "%02x", e->data[k]);
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ms", e->ms);
        cJSON_AddStringToObject(o, "dir", e->tx ? "tx" : "rx");
        cJSON_AddStringToObject(o, "hex", hex);
        if (e->tx) {
            cJSON_AddStringToObject(o, "result", res[e->result < 5 ? e->result : 4]);
        }
        cJSON_AddItemToArray(log, o);
    }
    free(st);
    return send_json(req, root);
}

static cJSON *read_body(httpd_req_t *req)
{
    char body[160] = {0};
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return NULL;
    }
    const int got = kvm_recv_all(req, body, (int)req->content_len);
    return got > 0 ? cJSON_Parse(body) : NULL;
}

static int read_la(const cJSON *j)
{
    const cJSON *la = cJSON_GetObjectItem(j, "la");
    return cJSON_IsNumber(la) && la->valueint >= 0 && la->valueint <= 15 ? la->valueint : -1;
}

static esp_err_t answer(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "HDMI-CEC is off or not available here");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "no CEC device on the line to send to");
    }
    return send_json_error(req, "503 Service Unavailable", "the CEC line is busy, try again");
}

static esp_err_t cec_key(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_body(req);
    uint8_t code = 0;
    bool ok = false;
    const cJSON *k = j ? cJSON_GetObjectItem(j, "key") : NULL;
    const cJSON *c = j ? cJSON_GetObjectItem(j, "code") : NULL;
    if (cJSON_IsString(k)) {
        ok = kvm_cec_key_from_name(k->valuestring, &code);
    } else if (cJSON_IsNumber(c) && c->valueint >= 0 && c->valueint <= 0xff) {
        code = (uint8_t)c->valueint;
        ok = true;
    }
    const int la = j ? read_la(j) : -1;
    cJSON_Delete(j);
    if (!ok) {
        return send_json_error(req, "400 Bad Request", "expected {\"key\":\"up\"} or {\"code\":1}");
    }
    return answer(req, kvm_cec_key(la, code));
}

static esp_err_t cec_power(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_body(req);
    const cJSON *a = j ? cJSON_GetObjectItem(j, "action") : NULL;
    const int la = j ? read_la(j) : -1;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "standby") == 0) {
        err = kvm_cec_standby(la);
    } else if (cJSON_IsString(a) && strcmp(a->valuestring, "wake") == 0) {
        err = kvm_cec_wake(la);
    }
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "400 Bad Request", "expected {\"action\":\"standby\"|\"wake\"}");
    }
    return answer(req, err);
}

static esp_err_t cec_send(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *j = read_body(req);
    const cJSON *jh = j ? cJSON_GetObjectItem(j, "hex") : NULL;
    uint8_t msg[16];
    int len = 0;
    if (cJSON_IsString(jh)) {
        const char *p = jh->valuestring;
        while (*p && len < 16) {
            char *end;
            const long v = strtol(p, &end, 16);
            if (end == p) {
                p++;
                continue;
            }
            msg[len++] = (uint8_t)v;
            p = end;
        }
    }
    cJSON_Delete(j);
    if (len < 1) {
        return send_json_error(req, "400 Bad Request", "expected {\"hex\":\"04 8f\"}");
    }
    const char *result = "";
    (void)kvm_cec_send(msg, len, &result);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tx", result);
    return send_json(req, root);
}

static esp_err_t cec_scan(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_cec_rescan();
    return answer(req, ESP_OK);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/cec", .method = HTTP_GET, .handler = cec_get},
    {.uri = "/api/v1/cec/key", .method = HTTP_POST, .handler = cec_key},
    {.uri = "/api/v1/cec/power", .method = HTTP_POST, .handler = cec_power},
    {.uri = "/api/v1/cec/send", .method = HTTP_POST, .handler = cec_send},
    {.uri = "/api/v1/cec/scan", .method = HTTP_POST, .handler = cec_scan},
};

const httpd_uri_t *cec_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
