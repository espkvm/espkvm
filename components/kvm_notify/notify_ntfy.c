/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ntfy channel: a message to a topic on ntfy.sh or on a server of the
 * operator's own, which the ntfy app on a phone shows as a notification. The
 * text is the body of a POST, the title a query parameter. A screenshot goes
 * as an attachment, a PUT with the JPEG as the body; a server that takes no
 * attachments (the default on a self-hosted one) refuses it, and then the
 * same message goes again as text. A clip is named by its file on the card.
 */
#include "notify_priv.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "kvm_settings.h"
#include "mbedtls/base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "notify";

#define NTFY_TEXT_MAX 4096 /* ntfy's own limit for a message */
#define NTFY_URL_MAX 2048 /* topic + title + up to 500 characters encoded */
/* With an attachment the text rides in the URL, so it is kept short. */
#define NTFY_TEXT_IN_URL 500

/* "tk_..." is an access token, "user:password" a login. */
static void set_auth(esp_http_client_handle_t c)
{
    const char *key = kvm_setting_str("notify_ntfy_key");
    if (!key[0]) {
        return;
    }
    char hdr[200];
    if (strchr(key, ':')) {
        unsigned char b64[160];
        size_t n = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &n, (const unsigned char *)key, strlen(key)) != 0) {
            return;
        }
        b64[n] = '\0';
        snprintf(hdr, sizeof(hdr), "Basic %s", (const char *)b64);
    } else {
        snprintf(hdr, sizeof(hdr), "Bearer %s", key);
    }
    esp_http_client_set_header(c, "Authorization", hdr);
}

/* The topic URL with ?title=... (and more) added. */
static bool build_url(char *out, size_t cap, const char *title, const char *extra)
{
    const char *topic = kvm_setting_str("notify_ntfy_url");
    char t[NOTIFY_TITLE_MAX * 3 + 1];
    notify_url_encode(title, t, sizeof(t));
    const int n = snprintf(out, cap, "%s%stitle=%s%s", topic, strchr(topic, '?') ? "&" : "?", t,
                           extra ? extra : "");
    return n > 0 && (size_t)n < cap;
}

static send_res_t post(const char *url, esp_http_client_method_t method, const char *body, size_t len,
                       const char *type)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* harmless for plain HTTP */
        /* The request line carries the title and message: the default 512
         * bytes would refuse it before anything is sent. */
        .buffer_size_tx = NTFY_URL_MAX + 64,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return SEND_RETRY;
    }
    set_auth(c);
    esp_http_client_set_header(c, "Content-Type", type);
    esp_http_client_set_post_field(c, body, (int)len);
    (void)esp_http_client_perform(c); /* the status says how it went */
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (status / 100 != 2) {
        ESP_LOGW(TAG, "ntfy: HTTP %d", status);
    }
    return notify_classify(status);
}

/* The message without the title on top: ntfy shows the title on its own. */
static const char *without_title(const notify_pending_t *p, const char *text)
{
    const size_t n = strlen(p->ev.title);
    if (n && strncmp(text, p->ev.title, n) == 0) {
        text += n;
        while (*text == '\n') {
            text++;
        }
    }
    return text;
}

static send_res_t send_ntfy(const notify_pending_t *p)
{
    char *text = malloc(NTFY_TEXT_MAX);
    char *url = malloc(NTFY_URL_MAX);
    if (!text || !url) {
        free(text);
        free(url);
        return SEND_RETRY;
    }
    if (p->ev.op == EV_CLIP) {
        notify_clip_text(p, text, NTFY_TEXT_MAX);
    } else {
        notify_compose(p, text, NTFY_TEXT_MAX);
    }
    const char *msg = without_title(p, text);
    if (!msg[0]) {
        msg = p->ev.title;
    }

    send_res_t r = SEND_RETRY;
    uint8_t *photo_loaded = NULL;
    size_t photo_len = 0;
    const uint8_t *photo = p->ev.op == EV_CLIP ? NULL : notify_photo(p, &photo_len, &photo_loaded);
    bool sent = false;
    if (photo && photo_len) {
        char short_msg[NTFY_TEXT_IN_URL + 1];
        strlcpy(short_msg, msg, sizeof(short_msg));
        char *enc = malloc(NTFY_TEXT_IN_URL * 3 + 32);
        if (enc) {
            strcpy(enc, "&filename=screen.jpg&message=");
            notify_url_encode(short_msg, enc + strlen(enc), NTFY_TEXT_IN_URL * 3 + 1);
            if (build_url(url, NTFY_URL_MAX, p->ev.title, enc)) {
                r = post(url, HTTP_METHOD_PUT, (const char *)photo, photo_len, "image/jpeg");
                /* Refused with a picture: most likely a server that takes no
                 * attachments, or one this big. The words still matter. */
                sent = r != SEND_REFUSED;
            }
            free(enc);
        }
    }
    free(photo_loaded);
    if (!sent) {
        char *log_loaded = NULL;
        const char *tail = p->ev.op == EV_CLIP ? NULL : notify_logtail(p, &log_loaded);
        if (tail && tail[0]) {
            const size_t used = strlen(text);
            snprintf(text + used, NTFY_TEXT_MAX - used, "\n\n%s", tail);
        }
        free(log_loaded);
        if (build_url(url, NTFY_URL_MAX, p->ev.title, NULL)) {
            r = post(url, HTTP_METHOD_POST, msg, strlen(msg), "text/plain; charset=utf-8");
        } else {
            r = SEND_REFUSED; /* a topic URL too long to use */
        }
    }
    free(text);
    free(url);
    ESP_LOGI(TAG, "ntfy: %s", r == SEND_OK ? "sent" : r == SEND_RETRY ? "will retry" : "refused");
    return r;
}

static uint32_t ntfy_targets(void)
{
    return kvm_setting_str("notify_ntfy_url")[0] ? 1 : 0;
}

static bool ntfy_send(const notify_pending_t *p, uint32_t *left)
{
    const send_res_t r = send_ntfy(p);
    if (r != SEND_RETRY) {
        *left = 0;
    }
    return r == SEND_REFUSED;
}

const notify_channel_t notify_channel_ntfy = {
    .id = "ntfy",
    .route_key = "notify_ntfy_ev",
    .photo = true,
    .targets = ntfy_targets,
    .send = ntfy_send,
};
