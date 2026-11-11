/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Signaling for the WebRTC video, all of it (no trickle):
 *
 *   POST   /api/v1/video/webrtc        {"sdp": offer} -> {"id": N, "sdp": answer}
 *   DELETE /api/v1/video/webrtc?id=N   the console is done with session N
 *   POST   /api/v1/video/webrtc/sound?id=N&on=1   sound in session N on (or 0: off)
 *
 * A session lives only as long as the sign-in that opened it.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sdkconfig.h"

#include "http_recv.h"
#include "kvm_auth.h"
#include "web_priv.h"

#if CONFIG_KVM_ENABLE_WEBRTC
#include "kvm_webrtc.h"

/* A browser's offer is 3-6 KB; Chrome's with every codec it has, about 6. */
#define OFFER_MAX (16 * 1024)

static const char *status_line(int code)
{
    switch (code) {
    case 400:
        return "400 Bad Request";
    case 409:
        return "409 Conflict";
    case 503:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}

static esp_err_t webrtc_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    uint8_t owner[32];
    if (!kvm_auth_session_of(req, owner)) {
        return kvm_auth_challenge(req);
    }
    if (req->content_len <= 0 || req->content_len >= OFFER_MAX) {
        return send_json_error(req, "413 Payload Too Large", "offer too large");
    }
    char *body = malloc((size_t)req->content_len + 1u);
    if (!body) {
        return send_json_error(req, "503 Service Unavailable", "out of memory");
    }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
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
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *sdp = cJSON_GetObjectItemCaseSensitive(j, "sdp");
    if (!cJSON_IsString(sdp)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "expected {\"sdp\": offer}");
    }

    char *answer = NULL;
    uint32_t id = 0;
    int status = 500;
    char err[160] = {0};
    const esp_err_t r =
        kvm_webrtc_offer(sdp->valuestring, owner, &answer, &id, &status, err, sizeof(err));
    cJSON_Delete(j);
    if (r != ESP_OK) {
        return send_json_error(req, status_line(status), err);
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", id);
    cJSON_AddStringToObject(out, "sdp", answer);
    free(answer);
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!s) {
        kvm_webrtc_close(id, owner);
        return send_json_error(req, "503 Service Unavailable", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t sr = httpd_resp_sendstr(req, s);
    free(s);
    return sr;
}

static esp_err_t webrtc_delete(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    uint8_t owner[32];
    if (!kvm_auth_session_of(req, owner)) {
        return kvm_auth_challenge(req);
    }
    char q[32] = {0};
    char v[12] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "id", v, sizeof(v)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "expected ?id=N");
    }
    kvm_webrtc_close((uint32_t)strtoul(v, NULL, 10), owner);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t webrtc_sound_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    uint8_t owner[32];
    if (!kvm_auth_session_of(req, owner)) {
        return kvm_auth_challenge(req);
    }
    char q[40] = {0};
    char id[12] = {0};
    char on[4] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "id", id, sizeof(id)) != ESP_OK ||
        httpd_query_key_value(q, "on", on, sizeof(on)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "expected ?id=N&on=1 or 0");
    }
    kvm_webrtc_sound((uint32_t)strtoul(id, NULL, 10), owner, on[0] == '1');
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/api/v1/video/webrtc", .method = HTTP_POST, .handler = webrtc_post},
    {.uri = "/api/v1/video/webrtc", .method = HTTP_DELETE, .handler = webrtc_delete},
    {.uri = "/api/v1/video/webrtc/sound", .method = HTTP_POST, .handler = webrtc_sound_post},
};

const httpd_uri_t *webrtc_api_routes(size_t *count)
{
    /* Registered with the routes: the pointer moves that come over a data
     * channel go where the WebSocket's input goes. */
    kvm_webrtc_set_input(kvm_web_webrtc_input);
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}

#else

const httpd_uri_t *webrtc_api_routes(size_t *count)
{
    *count = 0;
    return NULL;
}

#endif
