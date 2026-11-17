/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual media from a URL (kvm_url_media.h).
 *
 * One task does all the network work: it opens the URL, inserts the source,
 * and answers the media task's reads. Each read is one Range request over a
 * kept-alive connection. The media layer asks for 256 KB at a time while the
 * target reads straight on. Once it has answered such a read, the task fetches
 * the next 256 KB on its own while the target takes this one over USB, so the
 * network and USB run at the same time.
 *
 * The task's stack is in PSRAM, as the download's is: a TLS handshake needs
 * more stack than the media task has, and internal RAM is short. When the
 * target has read nothing for a while the connection is closed, which gives
 * the TLS memory back. The next read opens a new one.
 *
 * A redirect is followed on every new connection, so a link that redirects to
 * a mirror or to a signed storage URL keeps working.
 */
#include "kvm_url_media.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kvm_media.h"
#include "lwip/sockets.h"
#include "web_priv.h"

static const char *TAG = "url_media";

#define URL_MAX 512
/* The client reads a body this much at a time: 2 KB cost a third of the speed. */
#define HTTP_BUF (16 * 1024)
/* Three tries of a read stay inside the host's 30 s, after which it resets the drive. */
#define HTTP_TIMEOUT_MS 6000
#define MAX_REDIRECTS 5
#define READ_TRIES 3
#define READ_AHEAD (256 * 1024)
/* The first retry comes soon: at boot the network is often a few seconds behind. */
#define RETRY_FIRST_MS 3000
#define RETRY_MS 15000
#define TASK_STACK 8192
/* As the media task, which waits for it. */
#define TASK_PRIO 5

typedef struct {
    char url[URL_MAX]; /* as given; redirects are followed again on each connection */
    esp_http_client_handle_t http;
    esp_transport_handle_t tls;
    bool no_chacha;   /* the server refused ChaCha20: use the default suites */
    bool reuse;       /* the connection may take the next request */
    volatile bool closed;
    volatile bool idle;
    /* From the last response's Content-Range: "bytes FIRST-LAST/TOTAL". */
    int64_t range_first;
    int64_t range_total;
    uint64_t size;
} conn_t;

typedef enum { CMD_SELECT, CMD_READ, CMD_WAKE } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint64_t off;
    void *buf;
    uint32_t len;
    esp_err_t *result;
    TaskHandle_t waiter;
} cmd_t;

static SemaphoreHandle_t s_mu;
static QueueHandle_t s_queue;
/* What is wanted, under s_mu. gen moves on with every select. */
static char s_url[URL_MAX];
static bool s_cdrom;
static uint32_t s_gen;
static const char *s_state = "off";
static char s_message[96];
static uint64_t s_size;
/* The inserted source's connection. Only the task touches it. */
static conn_t *s_live;
/* The medium is being taken out: a read stops trying. */
static volatile bool s_cancel;
/* The chunk after the last one read, fetched ahead. Only the task touches it. */
static uint8_t *s_ahead;
static uint64_t s_ahead_off;
static uint32_t s_ahead_len; /* 0: empty */

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    conn_t *c = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Range") == 0) {
        /* "bytes 0-0/1234"; a total of "*" means the server does not know it. */
        const char *v = e->header_value;
        const char *dash = strchr(v, '-');
        const char *slash = strchr(v, '/');
        const char *sp = strchr(v, ' ');
        c->range_first = sp && dash ? strtoll(sp + 1, NULL, 10) : -1;
        c->range_total = slash && slash[1] != '*' ? strtoll(slash + 1, NULL, 10) : -1;
    }
    return ESP_OK;
}

static void drop(conn_t *c)
{
    if (c->http) {
        esp_http_client_close(c->http);
        esp_http_client_cleanup(c->http);
        c->http = NULL;
    }
    if (c->tls) {
        esp_transport_destroy(c->tls);
        c->tls = NULL;
    }
    c->reuse = false;
}

static esp_err_t make_client(conn_t *c)
{
    c->tls = !c->no_chacha && strncmp(c->url, "https://", 8) == 0 ? kvm_web_chacha_transport() : NULL;
    esp_http_client_config_t cfg = {
        .url = c->url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF,
        .buffer_size_tx = HTTP_BUF,
        .disable_auto_redirect = true,
        .transport = c->tls,
        .event_handler = on_http_event,
        .user_data = c,
    };
    c->http = esp_http_client_init(&cfg);
    if (!c->http) {
        drop(c);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * Send a Range request and read its headers. On return the body is next.
 * Follows redirects. *status is the final answer's code.
 */
static esp_err_t request(conn_t *c, uint64_t off, uint32_t len, int *status, int64_t *clen)
{
    char range[48];
    snprintf(range, sizeof(range), "bytes=%" PRIu64 "-%" PRIu64, off, off + len - 1);
    for (int hop = 0;; hop++) {
        if (!c->http) {
            esp_err_t err = make_client(c);
            if (err != ESP_OK) {
                return err;
            }
        }
        esp_http_client_set_header(c->http, "Range", range);
        c->range_first = -1;
        c->range_total = -1;
        esp_err_t err;
        if (c->reuse) {
            err = esp_http_client_prepare(c->http);
            if (err == ESP_OK) {
                err = esp_http_client_request_send(c->http, 0);
            }
        } else {
            err = esp_http_client_open(c->http, 0);
            /* The client writes a request in pieces. With Nagle on, the last
             * piece waits for the server's delayed ACK: 40 ms a request. */
            const int fd = err == ESP_OK ? esp_http_client_get_socket(c->http) : -1;
            if (fd >= 0) {
                const int one = 1;
                (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            }
        }
        if (err == ESP_OK) {
            *clen = esp_http_client_fetch_headers(c->http);
            *status = esp_http_client_get_status_code(c->http);
            if (*clen < 0) {
                err = ESP_FAIL;
            }
        }
        if (err != ESP_OK) {
            /* A kept-alive connection the server has closed meanwhile: once more on a new one. */
            const bool was_reused = c->reuse;
            drop(c);
            if (was_reused) {
                continue;
            }
            return err;
        }
        c->reuse = false;
        if (*status < 300 || *status >= 400) {
            return ESP_OK;
        }
        if (hop >= MAX_REDIRECTS) {
            *status = -1;
            return ESP_OK;
        }
        esp_http_client_set_redirection(c->http);
        esp_http_client_close(c->http);
    }
}

static esp_err_t read_body(conn_t *c, uint8_t *buf, uint32_t len)
{
    for (uint32_t got = 0; got < len;) {
        const int n = esp_http_client_read(c->http, (char *)buf + got, (int)(len - got));
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (uint32_t)n;
    }
    c->reuse = esp_http_client_is_persistent_connection(c->http);
    if (!c->reuse) {
        esp_http_client_close(c->http);
    }
    return ESP_OK;
}

static esp_err_t range_read(conn_t *c, uint64_t off, uint8_t *buf, uint32_t len)
{
    for (int attempt = 0; attempt < READ_TRIES && !s_cancel; attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));
        }
        int status = 0;
        int64_t clen = -1;
        if (request(c, off, len, &status, &clen) != ESP_OK) {
            ESP_LOGW(TAG, "read at %" PRIu64 ": could not connect", off);
            continue;
        }
        if (status != 206 || clen != len || c->range_first != (int64_t)off) {
            ESP_LOGW(TAG, "read at %" PRIu64 ": the server answered %d with %lld bytes", off, status,
                     (long long)clen);
            drop(c);
            continue;
        }
        if (read_body(c, buf, len) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "read at %" PRIu64 ": the connection broke", off);
        drop(c);
    }
    return ESP_ERR_TIMEOUT; /* tried enough: the media layer does not try again */
}

/* ---- the source, as the media task sees it ------------------------------ */

static esp_err_t src_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    esp_err_t result = ESP_FAIL;
    const cmd_t cmd = {.kind = CMD_READ, .off = off, .buf = buf, .len = len, .result = &result,
                       .waiter = xTaskGetCurrentTaskHandle()};
    xQueueSend(s_queue, &cmd, portMAX_DELAY);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return result;
}

/* Both may run on the task itself (an insert closes what was there), so they
 * only mark the connection and wake the task, never wait for it. */
static void src_close(void *ctx)
{
    ((conn_t *)ctx)->closed = true;
    const cmd_t cmd = {.kind = CMD_WAKE};
    xQueueSend(s_queue, &cmd, 0);
}

static void src_cancel(void)
{
    s_cancel = true;
}

static void src_idle(void *ctx)
{
    ((conn_t *)ctx)->idle = true;
    const cmd_t cmd = {.kind = CMD_WAKE};
    xQueueSend(s_queue, &cmd, 0);
}

static const kvm_media_ops_t s_ops = {
    .kind = "URL",
    .read = src_read,
    .close = src_close,
    .idle = src_idle,
    .cancel = src_cancel,
    .read_ahead = READ_AHEAD,
};

/* ---- opening ------------------------------------------------------------ */

/* The file name at the end of the URL's path, for the log and the console. */
static void name_from_url(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    /* Never a user name or password: this name is logged and shown. */
    const char *at = strchr(p, '@');
    if (at && at < p + strcspn(p, "/?#")) {
        p = at + 1;
    }
    const char *end = p + strcspn(p, "?#");
    const char *start = end;
    while (start > p && start[-1] != '/') {
        start--;
    }
    if (start == end) {
        start = p; /* no file name: the host */
        end = p + strcspn(p, "/?#");
    }
    const size_t n = (size_t)(end - start) < cap - 1 ? (size_t)(end - start) : cap - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

/* Try the URL once: its size, and whether it answers Range requests. */
static conn_t *open_url(const char *url, char *why, size_t why_cap, uint64_t *size)
{
    conn_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) {
        snprintf(why, why_cap, "out of memory");
        return NULL;
    }
    strlcpy(c->url, url, sizeof(c->url));
    int status = 0;
    int64_t clen = -1;
    esp_err_t err = request(c, 0, 1, &status, &clen);
    if (err != ESP_OK && strncmp(url, "https://", 8) == 0) {
        ESP_LOGI(TAG, "no ChaCha20 there; trying the default cipher suites");
        c->no_chacha = true;
        err = request(c, 0, 1, &status, &clen);
    }
    uint8_t first;
    if (err != ESP_OK) {
        snprintf(why, why_cap, "could not connect (%s)", esp_err_to_name(err));
    } else if (status == -1) {
        snprintf(why, why_cap, "too many redirects");
    } else if (status == 200) {
        snprintf(why, why_cap, "the server cannot send part of a file (no HTTP Range support)");
    } else if (status != 206) {
        snprintf(why, why_cap, "the server answered %d", status);
    } else if (c->range_total <= 0) {
        snprintf(why, why_cap, "the server did not say how big the image is");
    } else if (clen != 1 || read_body(c, &first, 1) != ESP_OK) {
        snprintf(why, why_cap, "the server sent a broken answer");
    } else {
        *size = c->size = (uint64_t)c->range_total;
        return c;
    }
    drop(c);
    free(c);
    return NULL;
}

static void serve_read(const cmd_t *cmd)
{
    esp_err_t r = ESP_FAIL;
    conn_t *c = s_live;
    if (c && !c->closed) {
        c->idle = false;
        if (s_ahead_len && cmd->off >= s_ahead_off && cmd->off + cmd->len <= s_ahead_off + s_ahead_len) {
            memcpy(cmd->buf, s_ahead + (cmd->off - s_ahead_off), cmd->len);
            r = ESP_OK;
        } else {
            r = range_read(c, cmd->off, cmd->buf, cmd->len);
        }
    }
    *cmd->result = r;
    xTaskNotifyGive(cmd->waiter);

    /* A whole chunk read straight on: the next one is likely wanted next. Not
     * when another command is already waiting, which comes first. */
    s_ahead_len = 0;
    const uint64_t next = cmd->off + cmd->len;
    if (r != ESP_OK || cmd->len != READ_AHEAD || next >= c->size || uxQueueMessagesWaiting(s_queue)) {
        return;
    }
    if (!s_ahead) {
        s_ahead = heap_caps_malloc(READ_AHEAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ahead) {
            return;
        }
    }
    const uint32_t n = c->size - next < READ_AHEAD ? (uint32_t)(c->size - next) : READ_AHEAD;
    if (range_read(c, next, s_ahead, n) == ESP_OK) {
        s_ahead_off = next;
        s_ahead_len = n;
    }
}

static void task(void *arg)
{
    (void)arg;
    uint32_t want_gen = 0;   /* the selection being opened, 0: none */
    TickType_t retry_at = 0;
    int fails = 0;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (want_gen) {
            const int32_t left = (int32_t)(retry_at - xTaskGetTickCount());
            wait = left > 0 ? (TickType_t)left : 0;
        }
        cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, wait) == pdTRUE) {
            if (cmd.kind == CMD_READ) {
                serve_read(&cmd);
            } else if (cmd.kind == CMD_SELECT) {
                xSemaphoreTake(s_mu, portMAX_DELAY);
                want_gen = s_url[0] ? s_gen : 0;
                xSemaphoreGive(s_mu);
                retry_at = xTaskGetTickCount();
                fails = 0;
            }
        }
        if (s_live && s_live->closed) {
            drop(s_live);
            free(s_live);
            s_live = NULL;
            s_ahead_len = 0;
        } else if (s_live && s_live->idle && s_live->http) {
            ESP_LOGI(TAG, "the target is not reading; closing the connection");
            drop(s_live);
            s_ahead_len = 0;
        }
        if (!want_gen || (int32_t)(xTaskGetTickCount() - retry_at) < 0) {
            continue;
        }

        /* Open the wanted URL. */
        char url[URL_MAX];
        xSemaphoreTake(s_mu, portMAX_DELAY);
        strlcpy(url, s_url, sizeof(url));
        const bool cdrom = s_cdrom;
        xSemaphoreGive(s_mu);
        char why[96];
        uint64_t size = 0;
        conn_t *c = open_url(url, why, sizeof(why), &size);

        xSemaphoreTake(s_mu, portMAX_DELAY);
        const bool still = want_gen == s_gen;
        if (still && c) {
            kvm_media_source_t src = {.ops = &s_ops, .ctx = c, .size = size, .cdrom = cdrom};
            name_from_url(url, src.name, sizeof(src.name));
            /* Nothing of ours is inserted (select ejected it), so the media
             * task is not waiting for this task while it inserts. */
            s_cancel = false;
            if (kvm_media_insert(&src) == ESP_OK) {
                s_live = c;
                s_state = "ready";
                s_message[0] = '\0';
                s_size = size;
                c = NULL;
            } else {
                /* Refused and closed: close only marked it, the rest is ours. */
                s_state = "error";
                snprintf(s_message, sizeof(s_message), "the image is too small");
            }
            want_gen = 0;
        } else if (still) {
            s_state = "error";
            strlcpy(s_message, why, sizeof(s_message));
        }
        xSemaphoreGive(s_mu);
        if (c) { /* refused, or opened for a selection that has moved on */
            drop(c);
            free(c);
        }
        if (!still) {
            continue;
        }
        if (want_gen) {
            ESP_LOGW(TAG, "cannot open the image: %s", why);
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(fails++ ? RETRY_MS : RETRY_FIRST_MS);
        }
    }
}

/* ---- the API ------------------------------------------------------------ */

static bool start(void)
{
    if (s_queue) {
        return true;
    }
    s_mu = xSemaphoreCreateMutex();
    QueueHandle_t q = xQueueCreate(4, sizeof(cmd_t));
    if (!s_mu || !q) {
        return false;
    }
    s_queue = q;
    if (xTaskCreateWithCaps(task, "url_media", TASK_STACK, NULL, TASK_PRIO, NULL, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        s_queue = NULL;
        vQueueDelete(q);
        return false;
    }
    return true;
}

void kvm_url_media_select(const char *url, bool cdrom)
{
    if (!url) {
        url = "";
    }
    if (!s_queue && !url[0]) {
        return; /* never used */
    }
    if (!start()) {
        ESP_LOGE(TAG, "no memory for the URL media task");
        return;
    }
    kvm_media_eject_ops(&s_ops);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_url, url, sizeof(s_url));
    s_cdrom = cdrom;
    s_gen++;
    if (s_gen == 0) {
        s_gen = 1;
    }
    s_state = url[0] ? "connecting" : "off";
    s_message[0] = '\0';
    s_size = 0;
    xSemaphoreGive(s_mu);
    const cmd_t cmd = {.kind = CMD_SELECT};
    xQueueSend(s_queue, &cmd, portMAX_DELAY);
    if (url[0]) {
        char name[64];
        name_from_url(url, name, sizeof(name));
        ESP_LOGI(TAG, "opening %s", name);
    }
}

void kvm_url_media_status(kvm_url_media_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = "off";
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    out->state = s_state;
    strlcpy(out->message, s_message, sizeof(out->message));
    out->size = s_size;
    xSemaphoreGive(s_mu);
}
