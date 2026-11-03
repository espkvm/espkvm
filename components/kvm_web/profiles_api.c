/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device profiles over REST. /api/v1/settings stays as it was: it shows the
 * values in effect and writes to the active profile.
 *
 *   GET  /api/v1/profiles          the profiles, the active one, the presets
 *   POST /api/v1/profiles          make one: {"name", "from", "match", "select"}
 *   POST /api/v1/profiles/update   {"id", "name", "match", "set", "unset"}
 *   POST /api/v1/profiles/delete   {"id"}
 *   POST /api/v1/profiles/select   {"id"}
 *   POST /api/v1/profiles/dismiss  {"welcome": true}, {"tour": version} or {"preset": id}
 *
 * Every POST answers with the list as GET does, plus "reboot": true when a
 * value that needs a restart moved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "http_recv.h"
#include "kvm_auth.h"
#include "kvm_profiles.h"
#include "web_priv.h"

#define BODY_MAX 2048

static esp_err_t send_list(httpd_req_t *req, bool reboot)
{
    char *s = kvm_profiles_json();
    cJSON *root = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!root) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddBoolToObject(root, "reboot", reboot);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out ? out : "{}");
    free(out);
    return r;
}

/** The body as a string, or NULL with the error already sent. */
static char *read_body(httpd_req_t *req, esp_err_t *sent)
{
    if (req->content_len <= 0 || req->content_len >= BODY_MAX) {
        *sent = send_json_error(req, "413 Payload Too Large", "body missing or too large");
        return NULL;
    }
    char *body = malloc((size_t)req->content_len + 1u);
    if (!body) {
        *sent = send_json_error(req, "500 Internal Server Error", "out of memory");
        return NULL;
    }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (kvm_recv_stalled(n) && ++stalls <= KVM_RECV_MAX_STALLS) {
            continue;
        }
        if (n <= 0) {
            free(body);
            *sent = send_json_error(req, "400 Bad Request", "truncated body");
            return NULL;
        }
        stalls = 0;
        got += n;
    }
    body[got] = '\0';
    return body;
}

static esp_err_t answer(httpd_req_t *req, esp_err_t res, const char *err, bool reboot)
{
    switch (res) {
    case ESP_OK:
        return send_list(req, reboot);
    case ESP_ERR_NOT_FOUND:
        return send_json_error(req, "404 Not Found", err[0] ? err : "no such profile");
    case ESP_ERR_NOT_ALLOWED:
        return send_json_error(req, "409 Conflict", err[0] ? err : "Default cannot be changed here");
    case ESP_ERR_NO_MEM:
        return send_json_error(req, "409 Conflict", err[0] ? err : "no room for another profile");
    default:
        return send_json_error(req, "400 Bad Request", err[0] ? err : "invalid profile");
    }
}

/** {"id": n} from @p body, or -1. */
static int body_id(const char *body)
{
    cJSON *j = cJSON_Parse(body);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    const int v = cJSON_IsNumber(id) && id->valueint >= 0 && id->valueint <= KVM_PROFILES_MAX
                      ? id->valueint
                      : -1;
    cJSON_Delete(j);
    return v;
}

static esp_err_t profiles_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_list(req, false);
}

static esp_err_t profiles_create(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    char err[96] = "";
    uint8_t id = 0;
    bool reboot = false;
    esp_err_t res = kvm_profiles_create(body, &id, &reboot, err, sizeof(err));
    free(body);
    return answer(req, res, err, reboot);
}

static esp_err_t profiles_update(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    char err[96] = "";
    bool reboot = false;
    esp_err_t res = kvm_profiles_update(body, &reboot, err, sizeof(err));
    free(body);
    return answer(req, res, err, reboot);
}

static esp_err_t profiles_delete(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    const int id = body_id(body);
    free(body);
    bool reboot = false;
    esp_err_t res = id < 0 ? ESP_ERR_NOT_FOUND : kvm_profiles_delete((uint8_t)id, &reboot);
    return answer(req, res, "", reboot);
}

static esp_err_t profiles_select(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    const int id = body_id(body);
    free(body);
    bool reboot = false;
    esp_err_t res = id < 0 ? ESP_ERR_NOT_FOUND : kvm_profiles_select((uint8_t)id, true, &reboot);
    return answer(req, res, "", reboot);
}

static esp_err_t profiles_dismiss(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    char err[96] = "";
    esp_err_t res = kvm_profiles_dismiss(body, err, sizeof(err));
    free(body);
    return answer(req, res, err, false);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/profiles", .method = HTTP_GET, .handler = profiles_get},
    {.uri = "/api/v1/profiles", .method = HTTP_POST, .handler = profiles_create},
    {.uri = "/api/v1/profiles/update", .method = HTTP_POST, .handler = profiles_update},
    {.uri = "/api/v1/profiles/delete", .method = HTTP_POST, .handler = profiles_delete},
    {.uri = "/api/v1/profiles/select", .method = HTTP_POST, .handler = profiles_select},
    {.uri = "/api/v1/profiles/dismiss", .method = HTTP_POST, .handler = profiles_dismiss},
};

const httpd_uri_t *profiles_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
