/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * espkvm-agent on the target, to the browser.
 *
 *   GET  /api/v1/agent            mode, whether an agent is there, its version and OS
 *   GET  /api/v1/agent/info       the machine: name, OS, uptime, addresses
 *   GET  /api/v1/agent/clipboard  {"text":"..."}
 *   POST /api/v1/agent/clipboard  {"text":"..."}
 *   POST /api/v1/agent/power      {"action":"shutdown"|"reboot"}
 *
 * The agent's answer is passed on as it is. Its error codes become HTTP
 * statuses: not_allowed 403, no_desktop and locked 409, bad_params 400,
 * anything else 502.
 */
#include <stdlib.h>
#include <string.h>

#include "http_recv.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include "kvm_agent.h"
#include "kvm_auth.h"
#include "kvm_settings.h"
#include "web_priv.h"

#define CALL_MS 4000
/* A clipboard text, escaped, plus the braces around it. */
#define BODY_MAX (KVM_AGENT_MAX_TEXT * 6 + 64)

static const char *const k_modes[] = {"off", "console", "agent"};

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t agent_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_agent_status_t st;
    kvm_agent_status(&st);
    if (st.port && !st.connected) {
        /* An agent that opened the port before this side was listening has
         * said its hello to nobody: ask again. */
        char *ans = NULL;
        if (kvm_agent_call("hello", NULL, 300, &ans) == ESP_OK) {
            kvm_agent_status(&st);
        }
        free(ans);
    }
    const int32_t m = kvm_setting_int("usb_serial");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", m >= 0 && m < 3 ? k_modes[m] : "off");
    cJSON_AddBoolToObject(root, "port", st.port);
    cJSON_AddBoolToObject(root, "connected", st.connected);
    if (st.connected) {
        cJSON_AddStringToObject(root, "version", st.version);
        cJSON_AddStringToObject(root, "os", st.os);
        cJSON *ops = cJSON_AddArrayToObject(root, "ops");
        char buf[sizeof(st.ops)];
        memcpy(buf, st.ops, sizeof(buf));
        for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            cJSON_AddItemToArray(ops, cJSON_CreateString(t));
        }
    }
    return send_json(req, root);
}

/* Ask the agent and pass its answer on. */
static esp_err_t relay(httpd_req_t *req, const char *op, const char *params)
{
    char *line = NULL;
    const esp_err_t err = kvm_agent_call(op, params, CALL_MS, &line);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "no agent is connected");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return send_json_error(req, "504 Gateway Timeout", "the agent did not answer");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "no memory");
    }
    cJSON *j = cJSON_Parse(line);
    free(line);
    if (!j) {
        return send_json_error(req, "502 Bad Gateway", "the agent's answer did not parse");
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"))) {
        cJSON *data = cJSON_DetachItemFromObject(j, "data");
        cJSON_Delete(j);
        if (!data || !cJSON_IsObject(data)) {
            cJSON_Delete(data);
            data = cJSON_CreateObject();
        }
        return send_json(req, data);
    }
    const cJSON *code = cJSON_GetObjectItem(j, "code");
    const char *c = cJSON_IsString(code) ? code->valuestring : "failed";
    const char *status = strcmp(c, "not_allowed") == 0  ? "403 Forbidden"
                         : strcmp(c, "no_desktop") == 0 ? "409 Conflict"
                         : strcmp(c, "locked") == 0     ? "409 Conflict"
                         : strcmp(c, "bad_params") == 0 ? "400 Bad Request"
                                                        : "502 Bad Gateway";
    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "error", cJSON_DetachItemFromObject(j, "error"));
    cJSON_AddStringToObject(out, "code", c);
    cJSON_Delete(j);
    httpd_resp_set_status(req, status);
    return send_json(req, out);
}

static esp_err_t agent_info_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return relay(req, "info", NULL);
}

static esp_err_t agent_clip_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return relay(req, "clipboard.get", NULL);
}

/* The body as JSON, or NULL after an error answer has gone out. */
static cJSON *read_body(httpd_req_t *req, esp_err_t *out)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) {
        *out = send_json_error(req, "400 Bad Request", "expected a JSON body of at most 64 KB of text");
        return NULL;
    }
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        *out = send_json_error(req, "500 Internal Server Error", "no memory");
        return NULL;
    }
    const int got = kvm_recv_all(req, body, (int)req->content_len);
    if (got < 0) {
        free(body);
        *out = send_json_error(req, "400 Bad Request", "could not read the body");
        return NULL;
    }
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        *out = send_json_error(req, "400 Bad Request", "the body is not JSON");
    }
    return j;
}

static esp_err_t agent_clip_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t r = ESP_OK;
    cJSON *j = read_body(req, &r);
    if (!j) {
        return r;
    }
    const cJSON *text = cJSON_GetObjectItem(j, "text");
    if (!cJSON_IsString(text) || strlen(text->valuestring) > KVM_AGENT_MAX_TEXT) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "expected {\"text\":\"...\"}, at most 64 KB");
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", text->valuestring);
    cJSON_Delete(j);
    char *params = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!params) {
        return send_json_error(req, "500 Internal Server Error", "no memory");
    }
    r = relay(req, "clipboard.set", params);
    free(params);
    return r;
}

static esp_err_t agent_power_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t r = ESP_OK;
    cJSON *j = read_body(req, &r);
    if (!j) {
        return r;
    }
    const cJSON *a = cJSON_GetObjectItem(j, "action");
    const char *params = NULL;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "shutdown") == 0) {
        params = "{\"action\":\"shutdown\"}";
    } else if (cJSON_IsString(a) && strcmp(a->valuestring, "reboot") == 0) {
        params = "{\"action\":\"reboot\"}";
    }
    cJSON_Delete(j);
    if (!params) {
        return send_json_error(req, "400 Bad Request", "expected {\"action\":\"shutdown\"|\"reboot\"}");
    }
    return relay(req, "power", params);
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/api/v1/agent", .method = HTTP_GET, .handler = agent_get},
    {.uri = "/api/v1/agent/info", .method = HTTP_GET, .handler = agent_info_get},
    {.uri = "/api/v1/agent/clipboard", .method = HTTP_GET, .handler = agent_clip_get},
    {.uri = "/api/v1/agent/clipboard", .method = HTTP_POST, .handler = agent_clip_post},
    {.uri = "/api/v1/agent/power", .method = HTTP_POST, .handler = agent_power_post},
};

const httpd_uri_t *agent_api_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
