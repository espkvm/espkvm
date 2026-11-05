/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web Push over REST, for Settings, Notifications.
 *
 *   GET  /api/v1/push            the VAPID public key and the browsers
 *   POST /api/v1/push/subscribe  {endpoint, keys: {p256dh, auth}, name}
 *   POST /api/v1/push/remove     {id} or {endpoint}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "kvm_auth.h"
#include "kvm_push.h"
#include "http_recv.h"
#include "web_priv.h"

#define BODY_MAX 2048
#define LIST_MAX 2048

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

static cJSON *read_json(httpd_req_t *req, esp_err_t *sent)
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
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        *sent = send_json_error(req, "400 Bad Request", "not JSON");
    }
    return j;
}

static esp_err_t push_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char key[100];
    if (kvm_push_public_key(key, sizeof(key)) != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not make the push key");
    }
    char *list = malloc(LIST_MAX);
    if (!list) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "key", key);
    cJSON *subs = kvm_push_list_json(list, LIST_MAX) ? cJSON_Parse(list) : NULL;
    cJSON_AddItemToObject(root, "browsers", subs ? subs : cJSON_CreateArray());
    free(list);
    return send_json(req, root);
}

static const char *str_of(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static esp_err_t push_subscribe_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent = ESP_OK;
    cJSON *j = read_json(req, &sent);
    if (!j) {
        return sent;
    }
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(j, "keys");
    int slot = -1;
    const esp_err_t r = kvm_push_subscribe(str_of(j, "endpoint"), str_of(keys, "p256dh"), str_of(keys, "auth"),
                                           str_of(j, "name"), &slot);
    cJSON_Delete(j);
    if (r == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "400 Bad Request", "not a push subscription");
    }
    if (r != ESP_OK && r != ESP_ERR_NO_MEM) {
        return send_json_error(req, "507 Insufficient Storage", "the device could not save it");
    }
    if (r != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "there is room for %d browsers: remove one first", KVM_PUSH_MAX);
        return send_json_error(req, "409 Conflict", msg);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", slot);
    return send_json(req, root);
}

static esp_err_t push_remove_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent = ESP_OK;
    cJSON *j = read_json(req, &sent);
    if (!j) {
        return sent;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    const char *endpoint = str_of(j, "endpoint");
    const int slot = cJSON_IsNumber(id) ? id->valueint : endpoint ? kvm_push_find(endpoint) : -1;
    cJSON_Delete(j);
    const esp_err_t r = kvm_push_remove(slot);
    if (r == ESP_ERR_NOT_FOUND || r == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "404 Not Found", "no such browser");
    }
    if (r != ESP_OK) {
        return send_json_error(req, "507 Insufficient Storage", "the device could not save it");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/push", .method = HTTP_GET, .handler = push_get},
    {.uri = "/api/v1/push/subscribe", .method = HTTP_POST, .handler = push_subscribe_post},
    {.uri = "/api/v1/push/remove", .method = HTTP_POST, .handler = push_remove_post},
};

const httpd_uri_t *push_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
