/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One low-priority task does two things: it watches for events worth a
 * notification (a watched phrase appearing, the screen going flat) and it
 * delivers the queue to Telegram and a webhook. The delivery holds a TLS
 * session and a copy of the screen JPEG for a moment; both come from PSRAM, so
 * the internal RAM the H.264 encoder needs is never touched.
 *
 * What cannot go out because the network is down waits in a pending list and
 * is retried, oldest first, with the time it really happened added to it. The
 * screenshot and the log tail are taken when the event happens, not when it is
 * finally sent. A refusal (a bad token, a wrong chat) is not retried. With a
 * writable microSD card each waiting event is also a few files in .notify/, so
 * the list survives a restart and its screenshots do not sit in PSRAM.
 */
#include "kvm_notify.h"

#include "capture.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kvm_caps.h"
#include "kvm_log.h"
#include "kvm_settings.h"
#include "kvm_storage.h"
#include "screentext_store.h"
#include "tg_chats.h"
#include "video_frame.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static const char *TAG = "notify";

/* TLS to Telegram needs room: the handshake alone wants several KB, and the
   sender keeps its buffers off the stack besides. 6 KB overflowed on the
   first real send. */
#define TASK_STACK 8192 /* peak 5.2 KB sending to Telegram over TLS */
#define TASK_PRIO 3 /* below the video and web tasks on purpose */
#define POLL_MS 2000
#define TITLE_MAX 80
#define BODY_MAX 200
#define QUEUE_DEPTH 6
#define HTTP_TIMEOUT_MS 15000
/* A screen JPEG we are willing to attach. Bigger than this and we send text
   only - a notification is not a place for a megabyte. */
#define PHOTO_MAX (2 * 1024 * 1024) /* a 1080p screenshot at quality 90; Telegram takes 10 MB */
/* How much of the tail of the device log to attach, when asked. Telegram takes
   up to 4096 characters in one message; a webhook takes it as a field. */
#define LOG_TAIL_MAX 2048
/* Finding chats: how much of getUpdates to read, and how many chats to offer. */
#define UPDATES_MAX (256 * 1024)
#define CHATS_MAX 10
#define CHATS_JSON_MAX 3072
/* Waiting for the network: how many events, how many screenshot bytes in all,
   and how the retries spread out. The screenshots sit in PSRAM, which the
   network buffers and TLS also draw on, so a long outage must never eat it: a
   cap for the list, and a screenshot is only kept while plenty stays free. */
#define PENDING_MAX 50
#define PENDING_PHOTO_BYTES (2 * 1024 * 1024)
/* With a writable card the list lives there too, so it survives a restart and
   the screenshots leave PSRAM at once. */
#define CARD_DIR ".notify"
#define PSRAM_RESERVE (3 * 1024 * 1024)
#define RETRY_FIRST_MS 15000
#define RETRY_MAX_MS (5 * 60 * 1000)
/* Sent this much later than it happened, a message says so. */
#define LATE_NOTE_US (60LL * 1000 * 1000)

typedef enum {
    EV_SEND,
    EV_FIND_CHATS,
    EV_CLIP,
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    char title[TITLE_MAX];
    char body[BODY_MAX];
    bool want_photo;
    char path[112];     /* EV_CLIP: the file */
    char card_path[80]; /* EV_CLIP: how the card names it */
} event_t;

/* How a send went: sent, failed in a way worth trying again (no network, a
   timeout, the server busy or rate-limiting), or refused (a 4xx), which a
   retry would only repeat. */
typedef enum {
    SEND_OK,
    SEND_RETRY,
    SEND_REFUSED,
} send_res_t;

static send_res_t classify(esp_err_t err, int status)
{
    if (err == ESP_OK && status >= 200 && status < 300) {
        return SEND_OK;
    }
    if (status >= 400 && status < 500 && status != 408 && status != 429) {
        return SEND_REFUSED;
    }
    return SEND_RETRY;
}

/* An event waiting for its channels, with what it carried when it happened. */
typedef struct {
    event_t ev;
    time_t at;        /* wall clock when it happened, 0 if the clock was unset */
    int64_t at_us;    /* uptime when it happened */
    uint8_t *photo;   /* PSRAM, or NULL */
    size_t photo_len;
    char *logtail;    /* PSRAM, or NULL */
    bool tg_left;     /* still to go to Telegram */
    bool hook_left;   /* still to go to the webhook */
    uint32_t id;      /* its files on the card are .notify/<id>.* (0 = memory only) */
    bool photo_file;  /* the screenshot is on the card, not in memory */
    bool log_file;
    bool before_boot; /* read back from the card after a restart */
} pending_t;

static pending_t *s_pending[PENDING_MAX];
static int s_npending;
static size_t s_pending_photo_bytes;
static unsigned s_dropped;        /* pushed out of a full list, not yet reported */
static int64_t s_next_retry_us;   /* 0 = try now */
static uint32_t s_retry_ms = RETRY_FIRST_MS;
static uint32_t s_next_id = 1;
static bool s_card_loaded; /* the list on the card has been read back */

/* The Bot API takes uploads up to 50 MB; a little under, for the form around it. */
#define TG_VIDEO_MAX (49 * 1024 * 1024)
#define CLIP_CHUNK (32 * 1024)

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static kvm_notify_status_t s_status;

/* The last "find chats" run. Guarded by s_lock. */
static const char *s_find_state = "idle";
static const char *s_find_error = "";
static char s_find_bot[64];
static char *s_find_chats; /* JSON array, PSRAM */

void kvm_notify_send(const char *title, const char *body, bool want_photo)
{
    if (!s_queue) {
        return;
    }
    event_t ev = {.kind = EV_SEND, .want_photo = want_photo};
    strlcpy(ev.title, title ? title : "", sizeof(ev.title));
    strlcpy(ev.body, body ? body : "", sizeof(ev.body));
    /* Never block a caller (it might be the capture task): drop if the queue is
       full - a backlog of stale alerts helps no one. */
    (void)xQueueSend(s_queue, &ev, 0);
}

void kvm_notify_send_clip(const char *path, const char *card_path, const char *caption)
{
    if (!s_queue) {
        return;
    }
    event_t ev = {.kind = EV_CLIP};
    strlcpy(ev.title, "Dashcam clip", sizeof(ev.title));
    strlcpy(ev.body, caption ? caption : "", sizeof(ev.body));
    strlcpy(ev.path, path ? path : "", sizeof(ev.path));
    strlcpy(ev.card_path, card_path ? card_path : "", sizeof(ev.card_path));
    (void)xQueueSend(s_queue, &ev, 0);
}

static void set_result(const char *result)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.last_result, result, sizeof(s_status.last_result));
    if (now > 1600000000) {
        strftime(s_status.last_at, sizeof(s_status.last_at), "%Y-%m-%d %H:%M:%S", &t);
    }
    xSemaphoreGive(s_lock);
}

/* --- small encoders ------------------------------------------------------- */

/* Percent-encode @p src for a URL query value. */
static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < cap; p++) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xf];
        }
    }
    dst[o] = '\0';
}

/* JSON-escape @p src into @p dst (no surrounding quotes). */
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < cap; p++) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* --- a screen JPEG, copied out of the frame store into PSRAM -------------- */

static uint8_t *grab_photo(size_t *out_len)
{
    /* Either codec: on H.264 the device makes the JPEG from the held frame. */
    uint8_t *jpeg = NULL;
    if (capture_snapshot_jpeg(&jpeg, out_len, 3000) != ESP_OK) {
        *out_len = 0;
        return NULL;
    }
    if (*out_len > PHOTO_MAX) {
        free(jpeg);
        *out_len = 0;
        return NULL;
    }
    return jpeg;
}

/* --- Telegram ------------------------------------------------------------- */

/* sendMessage: text only, form-urlencoded. */
static send_res_t tg_message(const char *token, const char *chat, const char *text)
{
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
    /* The text can be a 2 KB log tail, tripled by the encoding: heap, not stack. */
    const size_t enc_cap = (LOG_TAIL_MAX + TITLE_MAX + BODY_MAX) * 3 + 64;
    const size_t body_cap = enc_cap + 128;
    char *enc = heap_caps_malloc(enc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc || !body) {
        free(enc);
        free(body);
        return SEND_RETRY;
    }
    url_encode(text, enc, enc_cap);
    const int n = snprintf(body, body_cap, "chat_id=%s&disable_web_page_preview=true&text=%s",
                           chat, enc);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
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
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(enc);
    free(body);
    return classify(err, status);
}

/* sendPhoto: multipart/form-data with the JPEG, caption is the text. Streamed
   so the whole request never sits in one buffer. */
static send_res_t tg_photo(const char *token, const char *chat, const char *text, const uint8_t *jpeg,
                     size_t jpeg_len)
{
    static const char *const boundary = "espkvmXXbnd7391";
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", token);

    char pre[BODY_MAX + 512];
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
        .timeout_ms = HTTP_TIMEOUT_MS,
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
            res = classify(ESP_OK, esp_http_client_get_status_code(c));
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
    char pre[BODY_MAX + 640];
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
                res = classify(ESP_OK, status);
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

/* --- a generic webhook ---------------------------------------------------- */

static send_res_t webhook(const char *url, const char *title, const char *body,
                          const char *logtail, time_t at)
{
    char jt[TITLE_MAX * 2];
    char jb[BODY_MAX * 2];
    json_escape(title, jt, sizeof(jt));
    json_escape(body, jb, sizeof(jb));
    char jh[64];
    const char *host = kvm_setting_str("net_hostname");
    json_escape(host[0] ? host : "espkvm", jh, sizeof(jh));

    /* The body may carry a log tail, so the buffer is sized and heap-held. */
    const size_t cap = TITLE_MAX * 2 + BODY_MAX * 2 + 256 + (logtail ? LOG_TAIL_MAX * 2 : 0);
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
        char *jl = malloc(LOG_TAIL_MAX * 2);
        if (jl) {
            json_escape(logtail, jl, LOG_TAIL_MAX * 2);
            n += snprintf(json + n, cap - n, ",\"log\":\"%s\"", jl);
            free(jl);
        }
    }
    n += snprintf(json + n, cap - n, "}");

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* harmless for plain HTTP */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(json);
        return SEND_RETRY;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, n);
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(json);
    return classify(err, status);
}

/* --- delivery ------------------------------------------------------------- */

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
        .timeout_ms = HTTP_TIMEOUT_MS,
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
static void find_chats(void)
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
    if (!s_queue) {
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
    const event_t ev = {.kind = EV_FIND_CHATS};
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
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

/* --- the pending list, and its copy on the card ---------------------------- */

static void card_path(char *out, size_t cap, uint32_t id, const char *ext)
{
    if (id) {
        snprintf(out, cap, "%s/%s/%08lu.%s", kvm_storage_mount_point(), CARD_DIR,
                 (unsigned long)id, ext);
    } else {
        snprintf(out, cap, "%s/%s", kvm_storage_mount_point(), CARD_DIR);
    }
}

static bool card_write(uint32_t id, const char *ext, const void *data, size_t len)
{
    char path[64];
    card_path(path, sizeof(path), id, ext);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(data, 1, len, f) == len;
    return (fclose(f) == 0) && ok;
}

/* Read a whole file into PSRAM, NUL-terminated. NULL if missing or too big. */
static char *card_read(uint32_t id, const char *ext, size_t max, size_t *len)
{
    char path[64];
    card_path(path, sizeof(path), id, ext);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    fclose(f);
    if (!buf || n > max) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static void card_remove(const pending_t *p)
{
    static const char *const exts[] = {"txt", "jpg", "log"};
    char path[64];
    for (size_t i = 0; p->id && i < sizeof(exts) / sizeof(exts[0]); i++) {
        card_path(path, sizeof(path), p->id, exts[i]);
        unlink(path);
    }
}

/* One line per field, newlines in the text written as \n. */
static void put_field(char *out, size_t cap, size_t *o, const char *key, const char *val)
{
    *o += (size_t)snprintf(out + *o, cap - *o, "%s=", key);
    for (const char *c = val; *c && *o + 3 < cap; c++) {
        if (*c == '\n') {
            out[(*o)++] = '\\';
            out[(*o)++] = 'n';
        } else if (*c != '\r') {
            out[(*o)++] = *c;
        }
    }
    *o += (size_t)snprintf(out + *o, cap - *o, "\n");
}

static void card_save_meta(const pending_t *p)
{
    if (!p->id) {
        return;
    }
    char buf[sizeof(event_t) * 2 + 160];
    size_t o = (size_t)snprintf(buf, sizeof(buf),
                                "kind=%d\nat=%lld\ntg=%d\nhook=%d\nphoto=%d\nwant_photo=%d\nlog=%d\n",
                                (int)p->ev.kind, (long long)p->at, p->tg_left, p->hook_left,
                                p->photo_file, p->ev.want_photo, p->log_file);
    put_field(buf, sizeof(buf), &o, "title", p->ev.title);
    put_field(buf, sizeof(buf), &o, "body", p->ev.body);
    put_field(buf, sizeof(buf), &o, "path", p->ev.path);
    put_field(buf, sizeof(buf), &o, "card_path", p->ev.card_path);
    (void)card_write(p->id, "txt", buf, o < sizeof(buf) ? o : sizeof(buf) - 1);
}

/* Move a new event onto the card: its screenshot and log leave memory. */
static void card_store(pending_t *p)
{
    char dir[48];
    card_path(dir, sizeof(dir), 0, NULL);
    struct stat st;
    if (!kvm_storage_writable() || (stat(dir, &st) != 0 && mkdir(dir, 0775) != 0)) {
        return; /* no card to write: it waits in memory */
    }
    p->id = s_next_id++;
    if (p->photo && card_write(p->id, "jpg", p->photo, p->photo_len)) {
        s_pending_photo_bytes -= p->photo_len;
        free(p->photo);
        p->photo = NULL;
        p->photo_len = 0;
        p->photo_file = true;
    }
    if (p->logtail && card_write(p->id, "log", p->logtail, strlen(p->logtail))) {
        free(p->logtail);
        p->logtail = NULL;
        p->log_file = true;
    }
    card_save_meta(p);
}

static void pending_free(pending_t *p)
{
    if (!p) {
        return;
    }
    s_pending_photo_bytes -= p->photo_len;
    free(p->photo);
    free(p->logtail);
    free(p);
}

static void pending_pop_front(void)
{
    card_remove(s_pending[0]);
    pending_free(s_pending[0]);
    memmove(&s_pending[0], &s_pending[1], (size_t)(s_npending - 1) * sizeof(s_pending[0]));
    s_npending--;
}

/* Take an event in: what it carries is captured now, at the moment it
   happened, and the channels it still has to reach are marked. */
static void pending_add(const event_t *ev)
{
    const char *token = kvm_setting_str("notify_tg_token");
    const char *chat = kvm_setting_str("notify_tg_chat");
    const char *url = kvm_setting_str("notify_url");
    const bool token_bad = token[0] && !token_plausible(token);
    if (token_bad) {
        ESP_LOGE(TAG, "telegram: the bot token has characters a token cannot have - paste it again");
    }
    const bool tg = token[0] && chat[0] && !token_bad;
    const bool hook = url[0] != '\0';
    if (!tg && !hook) {
        set_result(token_bad ? "the Telegram token is not a bot token - paste it again"
                             : "no channel configured (set a Telegram bot or a webhook URL)");
        return;
    }
    if (ev->kind == EV_CLIP && !kvm_setting_bool("notify_clip")) {
        return;
    }

    pending_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        set_result("out of memory");
        return;
    }
    p->ev = *ev;
    p->at = time(NULL);
    if (p->at < 1600000000) {
        p->at = 0;
    }
    p->at_us = esp_timer_get_time();
    p->tg_left = tg;
    p->hook_left = hook;
    if (ev->kind == EV_SEND) {
        if (ev->want_photo && kvm_setting_bool("notify_snap") &&
            s_pending_photo_bytes < PENDING_PHOTO_BYTES &&
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > PHOTO_MAX + PSRAM_RESERVE) {
            p->photo = grab_photo(&p->photo_len);
            if (p->photo && (s_pending_photo_bytes + p->photo_len > PENDING_PHOTO_BYTES ||
                             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < PSRAM_RESERVE)) {
                free(p->photo); /* it goes as text: memory matters more */
                p->photo = NULL;
                p->photo_len = 0;
            }
            s_pending_photo_bytes += p->photo_len;
        }
        /* The tail of the device log, so an alert carries the context that
           explains it - which is why the noisy tags are held down. */
        if (kvm_setting_bool("notify_log")) {
            p->logtail = heap_caps_malloc(LOG_TAIL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p->logtail) {
                kvm_log_read(p->logtail, LOG_TAIL_MAX); /* keeps the newest end */
            }
        }
    }

    card_store(p);
    if (s_npending == PENDING_MAX) {
        pending_pop_front(); /* the oldest goes; the newest says more */
        s_dropped++;
    }
    s_pending[s_npending++] = p;
}

static const char *meta_get(char *meta, const char *key)
{
    const size_t kl = strlen(key);
    for (char *line = meta; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            return line + kl + 1; /* up to the newline, which load cuts */
        }
        line = nl ? nl + 1 : NULL;
    }
    return "";
}

static void unescape_into(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *c = src; *c && *c != '\n' && o + 1 < cap; c++) {
        if (c[0] == '\\' && c[1] == 'n') {
            dst[o++] = '\n';
            c++;
        } else {
            dst[o++] = *c;
        }
    }
    dst[o] = '\0';
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* After a restart: put what was waiting on the card back on the list. */
static void card_load(void)
{
    char dir[48];
    card_path(dir, sizeof(dir), 0, NULL);
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    uint32_t ids[PENDING_MAX * 2];
    size_t n = 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        const uint32_t id = (uint32_t)strtoul(e->d_name, NULL, 10);
        if (!dot || strcmp(dot, ".txt") != 0 || !id) {
            continue;
        }
        if (id >= s_next_id) {
            s_next_id = id + 1;
        }
        if (n < sizeof(ids) / sizeof(ids[0])) {
            ids[n++] = id;
        }
    }
    closedir(d);
    qsort(ids, n, sizeof(ids[0]), cmp_u32);
    for (size_t i = 0; i < n; i++) {
        size_t len;
        char *meta = card_read(ids[i], "txt", 2048, &len);
        pending_t *p = meta ? heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                            : NULL;
        if (!p) {
            free(meta);
            continue;
        }
        p->id = ids[i];
        p->before_boot = true;
        p->ev.kind = (ev_kind_t)atoi(meta_get(meta, "kind"));
        p->at = (time_t)atoll(meta_get(meta, "at"));
        p->tg_left = atoi(meta_get(meta, "tg")) != 0;
        p->hook_left = atoi(meta_get(meta, "hook")) != 0;
        p->photo_file = atoi(meta_get(meta, "photo")) != 0;
        p->ev.want_photo = atoi(meta_get(meta, "want_photo")) != 0;
        p->log_file = atoi(meta_get(meta, "log")) != 0;
        unescape_into(p->ev.title, sizeof(p->ev.title), meta_get(meta, "title"));
        unescape_into(p->ev.body, sizeof(p->ev.body), meta_get(meta, "body"));
        unescape_into(p->ev.path, sizeof(p->ev.path), meta_get(meta, "path"));
        unescape_into(p->ev.card_path, sizeof(p->ev.card_path), meta_get(meta, "card_path"));
        free(meta);
        if (s_npending == PENDING_MAX) {
            pending_pop_front();
            s_dropped++;
        }
        s_pending[s_npending++] = p;
    }
    if (s_npending) {
        ESP_LOGI(TAG, "%d alert(s) left waiting on the card from before the restart", s_npending);
    }
}

/* "Screen alert\n\nOn the screen: ...", and for a late one when it happened.
   A clip's caption is its body alone. */
static void compose(const pending_t *p, char *text, size_t cap)
{
    int n;
    if (p->ev.kind == EV_CLIP) {
        n = snprintf(text, cap, "%s", p->ev.body);
    } else {
        n = p->ev.body[0] ? snprintf(text, cap, "%s\n\n%s", p->ev.title, p->ev.body)
                          : snprintf(text, cap, "%s", p->ev.title);
    }
    if (n <= 0 || (size_t)n >= cap) {
        return;
    }
    if (p->before_boot) {
        /* Uptime means nothing across a restart; the wall clock may. */
        const time_t now = time(NULL);
        if (p->at && now > p->at) {
            struct tm t;
            localtime_r(&p->at, &t);
            char when[24];
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &t);
            snprintf(text + n, cap - n, "\n\n(Happened at %s, before the device restarted.)", when);
        } else {
            snprintf(text + n, cap - n, "\n\n(Happened before the device restarted.)");
        }
        return;
    }
    const int64_t late_us = esp_timer_get_time() - p->at_us;
    if (late_us < LATE_NOTE_US) {
        return;
    }
    const unsigned mins = (unsigned)(late_us / (60LL * 1000 * 1000));
    if (p->at) {
        struct tm t;
        localtime_r(&p->at, &t);
        char when[24];
        strftime(when, sizeof(when), "%H:%M:%S", &t);
        snprintf(text + n, cap - n, "\n\n(Happened at %s, %u min ago - the device could not send it then.)",
                 when, mins);
    } else {
        snprintf(text + n, cap - n, "\n\n(Happened %u min ago - the device could not send it then.)",
                 mins);
    }
}

static send_res_t send_event_tg(const pending_t *p, const char *token, const char *chat)
{
    char text[TITLE_MAX + BODY_MAX + 128];
    compose(p, text, sizeof(text));
    /* A screenshot or log kept on the card comes back for the send only. A
       card that has gone meanwhile just means a plain message. */
    size_t photo_len = p->photo_len, log_len = 0;
    uint8_t *photo = p->photo;
    char *logtail = p->logtail;
    uint8_t *photo_loaded = NULL;
    char *log_loaded = NULL;
    if (!photo && p->photo_file &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > PHOTO_MAX + PSRAM_RESERVE) {
        photo = photo_loaded = (uint8_t *)card_read(p->id, "jpg", PHOTO_MAX, &photo_len);
    }
    if (!logtail && p->log_file) {
        logtail = log_loaded = card_read(p->id, "log", LOG_TAIL_MAX, &log_len);
    }

    send_res_t r = (photo && photo_len) ? tg_photo(token, chat, text, photo, photo_len)
                                        : tg_message(token, chat, text);
    if (r == SEND_REFUSED && photo) {
        /* A photo can be refused on its own (size, format); the text still goes. */
        r = tg_message(token, chat, text);
    }
    /* The log goes as its own message: a photo caption is capped at 1024
       characters and a tail does not fit there. Best effort. */
    if (r == SEND_OK && logtail && logtail[0]) {
        char *msg = heap_caps_malloc(LOG_TAIL_MAX + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (msg) {
            snprintf(msg, LOG_TAIL_MAX + 16, "log:\n%s", logtail);
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

static send_res_t send_clip_tg(const pending_t *p, const char *token, const char *chat)
{
    struct stat st;
    if (stat(p->ev.path, &st) != 0) {
        return SEND_REFUSED; /* the clip is gone from the card */
    }
    /* off_t is 32-bit and a clip stays under 4 GB: read the size unsigned. */
    const uint32_t size = (uint32_t)st.st_size;
    char text[BODY_MAX + 200];
    compose(p, text, sizeof(text));
    send_res_t r;
    if (size <= TG_VIDEO_MAX) {
        r = tg_video(token, chat, text, p->ev.path, size);
    } else {
        char big[BODY_MAX + 300];
        snprintf(big, sizeof(big), "%.300s\nSaved on the card as %.80s (%u MB, too big for Telegram).",
                 text, p->ev.card_path, (unsigned)(size / (1024 * 1024)));
        r = tg_message(token, chat, big);
    }
    ESP_LOGI(TAG, "telegram clip: %s (%u KB)", r == SEND_OK ? "sent" : r == SEND_RETRY ? "will retry" : "refused",
             (unsigned)(size / 1024));
    return r;
}

static send_res_t send_hook(const pending_t *p, const char *url)
{
    send_res_t r;
    if (p->ev.kind == EV_CLIP) {
        char text[BODY_MAX + 200];
        snprintf(text, sizeof(text), "%.160s - saved on the card as %.80s", p->ev.body, p->ev.card_path);
        r = webhook(url, p->ev.title, text, NULL, p->at);
    } else {
        size_t log_len = 0;
        char *log_loaded = (!p->logtail && p->log_file) ? card_read(p->id, "log", LOG_TAIL_MAX, &log_len)
                                                        : NULL;
        r = webhook(url, p->ev.title, p->ev.body, p->logtail ? p->logtail : log_loaded, p->at);
        free(log_loaded);
    }
    ESP_LOGI(TAG, "webhook: %s", r == SEND_OK ? "sent" : r == SEND_RETRY ? "will retry" : "refused");
    return r;
}

/* Try the oldest waiting event on each channel it still needs. Returns true if
   it is done with (sent or refused everywhere), false if it has to wait. */
static bool try_front(void)
{
    pending_t *p = s_pending[0];
    const char *token = kvm_setting_str("notify_tg_token");
    const char *chat = kvm_setting_str("notify_tg_chat");
    const char *url = kvm_setting_str("notify_url");
    bool refused = false;

    if (p->tg_left) {
        if (!(token[0] && chat[0] && token_plausible(token))) {
            p->tg_left = false; /* the channel was switched off meanwhile */
        } else {
            const send_res_t r = p->ev.kind == EV_CLIP ? send_clip_tg(p, token, chat)
                                                      : send_event_tg(p, token, chat);
            p->tg_left = r == SEND_RETRY;
            refused |= r == SEND_REFUSED;
        }
    }
    if (p->hook_left) {
        if (!url[0]) {
            p->hook_left = false;
        } else {
            const send_res_t r = send_hook(p, url);
            p->hook_left = r == SEND_RETRY;
            refused |= r == SEND_REFUSED;
        }
    }

    if (p->tg_left || p->hook_left) {
        card_save_meta(p); /* a channel that got through is not tried again after a restart */
        char msg[96];
        snprintf(msg, sizeof(msg), "no network to send on - %d waiting, retrying", s_npending);
        set_result(msg);
        return false;
    }
    set_result(refused ? (p->ev.kind == EV_CLIP
                              ? "the last clip did not go out - check the token, chat id or URL"
                              : "the last send failed - check the token, chat id or URL")
                       : "ok");
    return true;
}

/* Send what is waiting, oldest first, until something has to wait again. */
static void flush_pending(void)
{
    while (s_npending > 0) {
        if (s_next_retry_us && esp_timer_get_time() < s_next_retry_us) {
            return;
        }
        if (!try_front()) {
            s_next_retry_us = esp_timer_get_time() + (int64_t)s_retry_ms * 1000;
            s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
            return;
        }
        pending_pop_front();
        s_next_retry_us = 0;
        s_retry_ms = RETRY_FIRST_MS;
    }
    if (s_dropped) {
        /* Said once the backlog is through, so it lands after what survived. */
        event_t ev = {.kind = EV_SEND, .title = "Alerts dropped"};
        snprintf(ev.body, sizeof(ev.body), "%u older alert%s did not fit while the device was offline.",
                 s_dropped, s_dropped == 1 ? "" : "s");
        s_dropped = 0;
        pending_add(&ev); /* goes out on the next pass */
    }
}

static void pending_clear(void)
{
    while (s_npending > 0) {
        pending_pop_front();
    }
    s_dropped = 0;
    s_next_retry_us = 0;
    s_retry_ms = RETRY_FIRST_MS;
}

/* --- the event poll ------------------------------------------------------- */

static void poll_events(void)
{
    /* A watched phrase appearing. The store gives an edge; we act on the rise. */
    static uint32_t s_alert_seen;
    uint32_t seq = 0;
    char phrase[SCREENTEXT_ALERT_MAX];
    const bool alerting = screentext_alert_get(phrase, sizeof(phrase), &seq);
    if (seq != s_alert_seen) {
        s_alert_seen = seq;
        if (alerting && kvm_setting_bool("notify_watch")) {
            char body[BODY_MAX];
            snprintf(body, sizeof(body), "On the screen: %.180s", phrase);
            kvm_notify_send("Screen alert", body, true);
        }
    }

    /* The screen going flat - a stop screen, a blanked output - for the same
       thirty seconds Home Assistant treats as a state, not a repaint. */
    static bool s_flat_seen;
    kvm_video_status_t v;
    capture_status_get(&v);
    const bool flat = v.signal && v.flat_ms >= 30000u;
    if (flat && !s_flat_seen && kvm_setting_bool("notify_flat")) {
        kvm_notify_send("Screen went blank", "The output has been one flat colour for a while.",
                        true);
    }
    s_flat_seen = flat;
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        event_t ev;
        /* Wake for a queued event, or every POLL_MS to look for one. */
        bool got = xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(POLL_MS)) == pdTRUE;
        if (got && ev.kind == EV_FIND_CHATS) {
            find_chats(); /* setting up, so it works with sending switched off */
            got = false;
        }
        if (!s_card_loaded && kvm_storage_writable()) {
            s_card_loaded = true; /* the card may mount after this task starts */
            card_load();          /* read back even when off, so off can clear it */
        }
        const bool on = kvm_setting_bool("notify_enable");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.enabled = on;
        s_status.pending = s_npending;
        xSemaphoreGive(s_lock);
        if (!on) {
            pending_clear(); /* switched off: nothing is kept for later */
            continue;        /* and a queued event is simply dropped */
        }
        if (got) {
            pending_add(&ev);
            s_next_retry_us = 0; /* something new: try the backlog now too */
        }
        flush_pending();
        poll_events();
    }
}

void kvm_notify_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(event_t));
    strlcpy(s_status.last_result, "nothing sent yet", sizeof(s_status.last_result));
    kvm_cap_report(KVM_CAP_NOTIFY, true, NULL);
    if (xTaskCreate(task, "notify", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the notify task");
    }
}

void kvm_notify_status(kvm_notify_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
