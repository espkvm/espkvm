/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Telegram channel: a bot of the operator's own writes to one chat. A
 * screenshot goes as a photo with the text as its caption, a dashcam clip as a
 * video that plays in the chat. Also the chat finder, so the chat id can be
 * picked rather than looked up.
 */
#include "notify_priv.h"
#include "tg_chats.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kvm_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "notify";

/* The Bot API takes uploads up to 50 MB; a little under, for the form around it. */
#define TG_VIDEO_MAX (49 * 1024 * 1024)
#define CLIP_CHUNK (32 * 1024)
/* Finding chats: how much of getUpdates to read, and how many chats to offer. */
#define UPDATES_MAX (256 * 1024)
#define CHATS_MAX 10
#define CHATS_JSON_MAX 3072

/* The last "find chats" run. Guarded by s_lock. */
static SemaphoreHandle_t s_lock;
static const char *s_find_state = "idle";
static const char *s_find_error = "";
static char s_find_bot[64];
static char *s_find_chats; /* JSON array, PSRAM */

void notify_tg_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

/* sendMessage: text only, form-urlencoded. */
static send_res_t tg_message(const char *token, const char *chat, const char *text)
{
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
    /* The text can be a 2 KB log tail, tripled by the encoding: heap, not stack. */
    const size_t enc_cap = (NOTIFY_LOG_TAIL_MAX + NOTIFY_TITLE_MAX + NOTIFY_BODY_MAX) * 3 + 64;
    const size_t body_cap = enc_cap + 128;
    char *enc = heap_caps_malloc(enc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc || !body) {
        free(enc);
        free(body);
        return SEND_RETRY;
    }
    notify_url_encode(text, enc, enc_cap);
    const int n = snprintf(body, body_cap, "chat_id=%s&disable_web_page_preview=true&text=%s",
                           chat, enc);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(enc);
        free(body);
        return SEND_RETRY;
    }
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, body, n);
    (void)esp_http_client_perform(c); /* the status says how it went */
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(enc);
    free(body);
    return notify_classify(status);
}

/* sendPhoto: multipart/form-data with the JPEG, caption is the text. Streamed
   so the whole request never sits in one buffer. */
static send_res_t tg_photo(const char *token, const char *chat, const char *text, const uint8_t *jpeg,
                     size_t jpeg_len)
{
    static const char *const boundary = "espkvmXXbnd7391";
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", token);

    char pre[NOTIFY_BODY_MAX + 512];
    int pn = 0;
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
                   boundary, chat);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n",
                   boundary, text);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"photo\"; "
                   "filename=\"screen.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n",
                   boundary);
    char post[64];
    const int pon = snprintf(post, sizeof(post), "\r\n--%s--\r\n", boundary);
    const int total = pn + (int)jpeg_len + pon;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return SEND_RETRY;
    }
    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(c, "Content-Type", ctype);

    send_res_t res = SEND_RETRY;
    if (esp_http_client_open(c, total) == ESP_OK) {
        if (esp_http_client_write(c, pre, pn) == pn &&
            esp_http_client_write(c, (const char *)jpeg, (int)jpeg_len) == (int)jpeg_len &&
            esp_http_client_write(c, post, pon) == pon) {
            esp_http_client_fetch_headers(c);
            res = notify_classify(esp_http_client_get_status_code(c));
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

/*
 * sendVideo, streamed from the card: the file never has to fit in memory. With
 * supports_streaming the chat plays it at once, which the MP4's index at the
 * front of the file allows.
 */
static send_res_t tg_video(const char *token, const char *chat, const char *text,
                           const char *path, size_t size)
{
    FILE *f = fopen(path, "rb");
    char *chunk = heap_caps_malloc(CLIP_CHUNK, MALLOC_CAP_SPIRAM);
    if (!f || !chunk) {
        if (f) {
            fclose(f);
        }
        free(chunk);
        return f ? SEND_RETRY : SEND_REFUSED; /* a clip that is gone stays gone */
    }
    static const char *const boundary = "espkvmXXbnd7391";
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendVideo", token);
    char pre[NOTIFY_BODY_MAX + 640];
    int pn = 0;
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
                   boundary, chat);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n",
                   boundary, text);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"supports_streaming\"\r\n\r\n"
                   "true\r\n",
                   boundary);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"video\"; "
                   "filename=\"clip.mp4\"\r\nContent-Type: video/mp4\r\n\r\n",
                   boundary);
    char post[64];
    const int pon = snprintf(post, sizeof(post), "\r\n--%s--\r\n", boundary);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    send_res_t res = SEND_RETRY;
    if (c) {
        char ctype[80];
        snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
        esp_http_client_set_header(c, "Content-Type", ctype);
        if (esp_http_client_open(c, pn + (int)size + pon) == ESP_OK &&
            esp_http_client_write(c, pre, pn) == pn) {
            size_t sent = 0;
            size_t n;
            bool w = true;
            while (w && (n = fread(chunk, 1, CLIP_CHUNK, f)) > 0) {
                w = esp_http_client_write(c, chunk, (int)n) == (int)n;
                sent += n;
            }
            if (w && sent == size && esp_http_client_write(c, post, pon) == pon) {
                esp_http_client_fetch_headers(c);
                const int status = esp_http_client_get_status_code(c);
                res = notify_classify(status);
                if (res != SEND_OK) {
                    ESP_LOGW(TAG, "telegram video: HTTP %d", status);
                }
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    fclose(f);
    free(chunk);
    return res;
}

/* A bot token is digits, a colon, then letters, digits, '_' and '-'. Anything
   else (a pasted sentence, a stray space) would only fail inside the URL
   parser with a message that names nothing. */
static bool token_plausible(const char *tok)
{
    for (const char *p = tok; *p; p++) {
        const char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == ':' || c == '_' || c == '-')) {
            return false;
        }
    }
    return strchr(tok, ':') != NULL;
}

/* --- finding chats -------------------------------------------------------- */

/* GET a Bot API method into @p buf (NUL-terminated). Returns the HTTP status,
   or -1 when nothing came back. A reply longer than @p cap is cut. */
static int tg_get(const char *token, const char *method, char *buf, size_t cap)
{
    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", token, method);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    int status = -1;
    size_t len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        while (len + 1 < cap) {
            const int r = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
            if (r <= 0) {
                break;
            }
            len += (size_t)r;
        }
    }
    buf[len] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static void find_done(const char *state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_find_state = state;
    s_find_error = error;
    xSemaphoreGive(s_lock);
}

/* getMe checks the token and names the bot; getUpdates lists who wrote to it.
   No offset is sent, so nothing is marked as read. */
void notify_tg_find_chats(void)
{
    const char *token = kvm_setting_str("notify_tg_token");
    if (!token[0]) {
        find_done("error", "set the bot token first");
        return;
    }
    if (!token_plausible(token)) {
        find_done("error", "the Telegram token is not a bot token - paste it again");
        return;
    }
    char *buf = heap_caps_malloc(UPDATES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *list = heap_caps_malloc(CHATS_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !list) {
        free(buf);
        free(list);
        find_done("error", "out of memory");
        return;
    }

    char bot[sizeof(s_find_bot)] = "";
    int status = tg_get(token, "getMe", buf, 4096);
    if (status == 401 || status == 404) {
        find_done("error", "Telegram did not accept the bot token");
    } else if (status != 200 || !tg_bot_username(buf, bot, sizeof(bot))) {
        find_done("error", "could not reach Telegram");
    } else {
        status = tg_get(token, "getUpdates?limit=100", buf, UPDATES_MAX);
        const int n = status == 200 ? tg_chats_from_updates(buf, list, CHATS_JSON_MAX, CHATS_MAX) : -1;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_find_bot, bot, sizeof(s_find_bot));
        if (n >= 0) {
            free(s_find_chats);
            s_find_chats = list;
            list = NULL;
        }
        xSemaphoreGive(s_lock);
        if (status == 409) {
            find_done("error", "the bot has a webhook set, so it cannot list its chats");
        } else if (n < 0) {
            find_done("error", "could not read the chats from Telegram");
        } else {
            ESP_LOGI(TAG, "telegram: found %d chat(s)", n);
            find_done("ok", "");
        }
    }
    free(buf);
    free(list);
}

esp_err_t kvm_notify_find_chats(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = strcmp(s_find_state, "running") == 0;
    if (!busy) {
        s_find_state = "running";
        s_find_error = "";
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_OK;
    }
    const notify_event_t ev = {.op = EV_FIND_CHATS};
    if (!notify_post(&ev)) {
        find_done("error", "the device is busy sending - try again");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

size_t kvm_notify_chats_json(char *out, size_t cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = snprintf(out, cap, "{\"state\":\"%s\",\"error\":\"%s\",\"bot\":\"%s\",\"chats\":%s}",
                           s_find_state, s_find_error, s_find_bot,
                           s_find_chats ? s_find_chats : "[]");
    xSemaphoreGive(s_lock);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static send_res_t send_event_tg(const notify_pending_t *p, const char *token, const char *chat)
{
    char text[NOTIFY_TITLE_MAX + NOTIFY_BODY_MAX + 128];
    notify_compose(p, text, sizeof(text));
    /* A screenshot or log kept on the card comes back for the send only. A
       card that has gone meanwhile just means a plain message. */
    size_t photo_len = 0;
    uint8_t *photo_loaded = NULL;
    char *log_loaded = NULL;
    const uint8_t *photo = notify_photo(p, &photo_len, &photo_loaded);
    const char *logtail = notify_logtail(p, &log_loaded);

    send_res_t r = (photo && photo_len) ? tg_photo(token, chat, text, photo, photo_len)
                                        : tg_message(token, chat, text);
    if (r == SEND_REFUSED && photo) {
        /* A photo can be refused on its own (size, format); the text still goes. */
        r = tg_message(token, chat, text);
    }
    /* The log goes as its own message: a photo caption is capped at 1024
       characters and a tail does not fit there. Best effort. */
    if (r == SEND_OK && logtail && logtail[0]) {
        char *msg = heap_caps_malloc(NOTIFY_LOG_TAIL_MAX + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (msg) {
            snprintf(msg, NOTIFY_LOG_TAIL_MAX + 16, "log:\n%s", logtail);
            (void)tg_message(token, chat, msg);
            free(msg);
        }
    }
    ESP_LOGI(TAG, "telegram: %s%s", r == SEND_OK ? "sent" : r == SEND_RETRY ? "will retry" : "refused",
             photo ? " (with photo)" : "");
    free(photo_loaded);
    free(log_loaded);
    return r;
}

static send_res_t send_clip_tg(const notify_pending_t *p, const char *token, const char *chat)
{
    struct stat st;
    if (stat(p->ev.path, &st) != 0) {
        return SEND_REFUSED; /* the clip is gone from the card */
    }
    /* off_t is 32-bit and a clip stays under 4 GB: read the size unsigned. */
    const uint32_t size = (uint32_t)st.st_size;
    char text[NOTIFY_BODY_MAX + 200];
    notify_compose(p, text, sizeof(text));
    send_res_t r;
    if (size <= TG_VIDEO_MAX) {
        r = tg_video(token, chat, text, p->ev.path, size);
    } else {
        char big[NOTIFY_BODY_MAX + 300];
        snprintf(big, sizeof(big), "%.300s\nSaved on the card as %.80s (%u MB, too big for Telegram).",
                 text, p->ev.card_path, (unsigned)(size / (1024 * 1024)));
        r = tg_message(token, chat, big);
    }
    ESP_LOGI(TAG, "telegram clip: %s (%u KB)", r == SEND_OK ? "sent" : r == SEND_RETRY ? "will retry" : "refused",
             (unsigned)(size / 1024));
    return r;
}

static uint32_t tg_targets(void)
{
    const char *token = kvm_setting_str("notify_tg_token");
    const char *chat = kvm_setting_str("notify_tg_chat");
    static bool s_warned; /* asked every poll: say it once per bad token */
    if (token[0] && !token_plausible(token)) {
        if (!s_warned) {
            ESP_LOGE(TAG, "telegram: the bot token has characters a token cannot have - paste it again");
        }
        s_warned = true;
        return 0;
    }
    s_warned = false;
    return token[0] && chat[0] ? 1 : 0;
}

static bool tg_send(const notify_pending_t *p, uint32_t *left)
{
    const char *token = kvm_setting_str("notify_tg_token");
    const char *chat = kvm_setting_str("notify_tg_chat");
    const send_res_t r = p->ev.op == EV_CLIP ? send_clip_tg(p, token, chat) : send_event_tg(p, token, chat);
    if (r != SEND_RETRY) {
        *left = 0;
    }
    return r == SEND_REFUSED;
}

const notify_channel_t notify_channel_tg = {
    .id = "tg",
    .route_key = "notify_tg_ev",
    .photo = true,
    .targets = tg_targets,
    .send = tg_send,
};
