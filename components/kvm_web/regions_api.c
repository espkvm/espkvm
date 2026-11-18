/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen areas over REST.
 *
 *   GET  /api/v1/regions            the areas, and what each sees now
 *   POST /api/v1/regions            {"regions": [...]}: all of them, replaced
 *   POST /api/v1/regions/remember   {"id"}: keep what it shows now, for like/unlike
 *
 * Every POST answers with the list as GET does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "http_recv.h"
#include "kvm_auth.h"
#include "kvm_regions.h"
#include "web_priv.h"

/* Eight areas with long names and every field: well under this. */
#define BODY_MAX 4096

static esp_err_t send_list(httpd_req_t *req)
{
    char *s = kvm_regions_json();
    if (!s) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
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

static esp_err_t regions_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_list(req);
}

static esp_err_t regions_set(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    char err[120] = "";
    esp_err_t res = kvm_regions_set_json(body, err, sizeof(err));
    free(body);
    if (res == ESP_ERR_NO_MEM) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    if (res != ESP_OK) {
        return send_json_error(req, "400 Bad Request", err[0] ? err : "invalid areas");
    }
    return send_list(req);
}

static esp_err_t regions_remember(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t sent;
    char *body = read_body(req, &sent);
    if (!body) {
        return sent;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    const int v = cJSON_IsNumber(id) ? id->valueint : -1;
    cJSON_Delete(j);
    const esp_err_t res = v >= 1 && v <= REGION_MAX ? kvm_regions_remember((uint8_t)v) : ESP_ERR_NOT_FOUND;
    if (res == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "404 Not Found", "no such area");
    }
    if (res == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "the area has not seen a picture yet");
    }
    if (res != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(res));
    }
    return send_list(req);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/regions", .method = HTTP_GET, .handler = regions_get},
    {.uri = "/api/v1/regions", .method = HTTP_POST, .handler = regions_set},
    {.uri = "/api/v1/regions/remember", .method = HTTP_POST, .handler = regions_remember},
};

const httpd_uri_t *regions_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
