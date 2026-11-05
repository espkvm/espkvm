/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tailscale over REST, for the VPN section of the console.
 *
 *   GET  /api/v1/tailscale         state, the login link while one is needed,
 *                                  the relay, and every peer with its path
 *   POST /api/v1/tailscale/logout  forget this device's keys and auth key
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "kvm_auth.h"
#include "kvm_settings.h"
#include "kvm_ts.h"
#include "web_priv.h"

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

/* One word for where the client is, for the console to say in a line. */
static const char *state_name(const kvm_ts_status_t *st)
{
    if (!kvm_setting_bool("ts_enable")) {
        return "off";
    }
    if (st->up) {
        return "up";
    }
    if (st->login_url[0]) {
        return "login";
    }
    if (st->error[0]) {
        return "refused";
    }
    return st->enabled ? "connecting" : "waiting";
}

static esp_err_t ts_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_ts_status_t *st = malloc(sizeof(*st));
    kvm_ts_peer_t *p = malloc(sizeof(*p));
    if (!st || !p) {
        free(st);
        free(p);
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    kvm_ts_status(st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_name(st));
    cJSON_AddStringToObject(root, "address", st->address);
    cJSON_AddStringToObject(root, "name", st->name);
    cJSON_AddStringToObject(root, "loginUrl", st->login_url);
    cJSON_AddStringToObject(root, "error", st->error);
    cJSON_AddNumberToObject(root, "keyExpiry", (double)st->key_expiry);
    cJSON_AddBoolToObject(root, "keyExpired", st->key_expired);
    cJSON *derp = cJSON_AddObjectToObject(root, "relay");
    cJSON_AddStringToObject(derp, "region", st->derp);
    cJSON_AddBoolToObject(derp, "up", st->derp_up);

    cJSON *list = cJSON_AddArrayToObject(root, "peers");
    const int n = kvm_ts_peer_count();
    for (int i = 0; i < n; i++) {
        if (!kvm_ts_peer(i, p)) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ip", p->ip);
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "user", p->user);
        cJSON_AddStringToObject(o, "userName", p->user_display);
        if (p->online >= 0) {
            cJSON_AddBoolToObject(o, "online", p->online == 1);
        } else {
            cJSON_AddNullToObject(o, "online");
        }
        cJSON_AddStringToObject(o, "path", p->direct ? "direct" : "relay");
        cJSON_AddStringToObject(o, "endpoint", p->endpoint);
        cJSON_AddStringToObject(o, "relay", p->derp);
        if (p->rtt_ms) {
            cJSON_AddNumberToObject(o, "ms", p->rtt_ms);
            cJSON_AddBoolToObject(o, "msDirect", p->rtt_direct);
        }
        cJSON_AddItemToArray(list, o);
    }
    free(st);
    free(p);
    return send_json(req, root);
}

static esp_err_t ts_logout_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (kvm_ts_logout() != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable", "Tailscale is not running");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/tailscale", .method = HTTP_GET, .handler = ts_get},
    {.uri = "/api/v1/tailscale/logout", .method = HTTP_POST, .handler = ts_logout_post},
};

const httpd_uri_t *ts_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
