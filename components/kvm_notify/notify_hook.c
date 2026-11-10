/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The webhook channel: a small JSON POST to a URL of the operator's choice,
 * for n8n, Node-RED, Home Assistant or a relay of their own. Nothing is
 * uploaded: a clip is named by its file on the card.
 */
#include "notify_priv.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "kvm_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "notify";

static send_res_t webhook(const char *url, const char *title, const char *body,
                          const char *logtail, time_t at)
{
    char jt[NOTIFY_TITLE_MAX * 2];
    char jb[NOTIFY_BODY_MAX * 2];
    notify_json_escape(title, jt, sizeof(jt));
    notify_json_escape(body, jb, sizeof(jb));
    char jh[64];
    const char *host = kvm_setting_str("net_hostname");
    notify_json_escape(host[0] ? host : "espkvm", jh, sizeof(jh));

    /* The body may carry a log tail, so the buffer is sized and heap-held. */
    const size_t cap = NOTIFY_TITLE_MAX * 2 + NOTIFY_BODY_MAX * 2 + 256 + (logtail ? NOTIFY_LOG_TAIL_MAX * 2 : 0);
    char *json = malloc(cap);
    if (!json) {
        return SEND_RETRY;
    }
    int n = snprintf(json, cap, "{\"title\":\"%s\",\"message\":\"%s\",\"device\":\"%s\"",
                     jt, jb, jh);
    if (at > 1600000000) {
        /* When it happened, which a delayed delivery makes worth saying. */
        n += snprintf(json + n, cap - n, ",\"at\":%lld", (long long)at);
    }
    if (logtail && logtail[0]) {
        char *jl = malloc(NOTIFY_LOG_TAIL_MAX * 2);
        if (jl) {
            notify_json_escape(logtail, jl, NOTIFY_LOG_TAIL_MAX * 2);
            n += snprintf(json + n, cap - n, ",\"log\":\"%s\"", jl);
            free(jl);
        }
    }
    n += snprintf(json + n, cap - n, "}");

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* harmless for plain HTTP */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(json);
        return SEND_RETRY;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, n);
    (void)esp_http_client_perform(c); /* the status says how it went */
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(json);
    return notify_classify(status);
}

static send_res_t send_hook(const notify_pending_t *p, const char *url)
{
    send_res_t r;
    if (p->ev.op == EV_CLIP) {
        char text[NOTIFY_BODY_MAX + 200];
        notify_clip_text(p, text, sizeof(text));
        r = webhook(url, p->ev.title, text, NULL, p->at);
    } else {
        char *log_loaded = NULL;
        r = webhook(url, p->ev.title, p->ev.body, notify_logtail(p, &log_loaded), p->at);
        free(log_loaded);
    }
    ESP_LOGI(TAG, "webhook: %s", r == SEND_OK ? "sent" : r == SEND_RETRY ? "will retry" : "refused");
    return r;
}

static uint32_t hook_targets(void)
{
    return kvm_setting_str("notify_url")[0] ? 1 : 0;
}

static bool hook_send(const notify_pending_t *p, uint32_t *left)
{
    const send_res_t r = send_hook(p, kvm_setting_str("notify_url"));
    if (r != SEND_RETRY) {
        *left = 0;
    }
    return r == SEND_REFUSED;
}

const notify_channel_t notify_channel_hook = {
    .id = "hook",
    .route_key = "notify_hook_ev",
    .targets = hook_targets,
    .send = hook_send,
};
