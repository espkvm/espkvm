/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Download an image from a URL onto the device: onto the microSD card, or into
 * the on-flash rescue partition (netboot.xyz is one click). The device fetches
 * it itself, so a NAS or a vendor's site works without passing the file through
 * the browser.
 *
 *   POST /api/v1/storage/fetch          {"url":"...","dest":"card"|"rescue","name":"x.iso"}
 *   GET  /api/v1/storage/fetch          progress: state, bytes, total, rate, message
 *   POST /api/v1/storage/fetch/cancel
 *
 * One download at a time. On the card it goes to "<name>.part" and is renamed
 * only once complete, so a broken download never sits under the real name. The
 * rescue partition needs the size up front, so a server that does not say it is
 * refused there - and its image is downloaded whole into PSRAM first and written
 * to flash only after the connection is closed, so the cache-off flash writes
 * never stall a live connection. HTTPS is checked against the built-in root
 * certificates and asks for ChaCha20 first (kvm_web_chacha_transport); plain
 * http works for a LAN server.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_transport_ssl.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "kvm_auth.h"
#include "kvm_record.h"
#include "kvm_settings.h"
#include "kvm_storage.h"
#include "web_priv.h"

static const char *TAG = "url_fetch";

#define URL_MAX 512
#define NAME_MAX_LEN 63
#define CHUNK (16 * 1024)
#define HTTP_BUF 2048
#define HTTP_TIMEOUT_MS 20000
#define MAX_REDIRECTS 5
#define MAX_STALLS 3
/* The download task's stack is in PSRAM: 8 KB of internal RAM is often not
 * there. Writing the rescue partition turns the flash cache off, and a PSRAM
 * stack cannot run then, so that part runs in a small task of its own, started
 * once the TLS session is gone and its internal RAM is back. */
#define TASK_STACK 8192
#define WRITE_STACK 3072
#define TASK_PRIO 4

typedef enum { F_IDLE, F_RUNNING, F_DONE, F_ERROR, F_CANCELLED } fetch_state_t;

static SemaphoreHandle_t s_mu;
static struct {
    fetch_state_t state;
    bool rescue;
    char url[URL_MAX];
    char name[NAME_MAX_LEN + 1];
    uint64_t bytes;
    int64_t total; /* -1 unknown */
    uint64_t rescue_cap; /* read by the POST handler: the task cannot touch flash */
    int64_t started_us;
    int64_t ended_us;
    char message[120];
} s_job;
static volatile bool s_cancel;

static void lock(void)
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static void finish(fetch_state_t st, const char *msg)
{
    lock();
    s_job.state = st;
    s_job.ended_us = esp_timer_get_time();
    snprintf(s_job.message, sizeof(s_job.message), "%s", msg ? msg : "");
    unlock();
    if (st == F_DONE) {
        ESP_LOGI(TAG, "downloaded %s (%llu bytes)", s_job.rescue ? "the rescue image" : s_job.name,
                 (unsigned long long)s_job.bytes);
    } else {
        ESP_LOGW(TAG, "download stopped: %s", msg ? msg : "");
    }
}

/* The same rule as the upload's: a plain name in the card's root. */
static bool name_ok(const char *n)
{
    return n && n[0] && strlen(n) <= NAME_MAX_LEN && !strpbrk(n, "/\\") && !strstr(n, "..") &&
           n[0] != '.';
}

static const int *chacha_suites(void)
{
    static int list[4];
    static bool built;
    if (!built) {
        built = true;
        static const int want[] = {0xCCA9, 0xCCA8, 0xCCAA}; /* ECDHE-ECDSA, ECDHE-RSA, DHE-RSA */
        const int *have = esp_tls_get_ciphersuites_list();
        size_t n = 0;
        for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
            for (const int *h = have; h && *h; h++) {
                if (*h == want[i]) {
                    list[n++] = want[i];
                    break;
                }
            }
        }
        list[n] = 0;
    }
    return list[0] ? list : NULL;
}

esp_transport_handle_t kvm_web_chacha_transport(void)
{
    const int *suites = chacha_suites();
    if (!suites) {
        return NULL;
    }
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    if (!ssl) {
        return NULL;
    }
    esp_transport_set_default_port(ssl, 443);
    esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
    esp_transport_ssl_set_ciphersuites_list(ssl, suites);
    return ssl;
}

/* Open the URL, following redirects by hand (disable_auto_redirect only covers
 * perform()). Returns the client positioned at the body, or NULL with *why. */
static esp_http_client_handle_t open_url(const char *url, esp_transport_handle_t tls, int64_t *total,
                                         int *status, esp_err_t *err)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* A storage host's redirect Location can run to a kilobyte. */
        .buffer_size = HTTP_BUF,
        .buffer_size_tx = HTTP_BUF,
        .disable_auto_redirect = true,
        .transport = tls,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        *err = ESP_ERR_NO_MEM;
        return NULL;
    }
    *status = 0;
    for (int hop = 0;; hop++) {
        *err = esp_http_client_open(http, 0);
        if (*err != ESP_OK) {
            break;
        }
        *total = esp_http_client_fetch_headers(http);
        *status = esp_http_client_get_status_code(http);
        if (*status < 300 || *status >= 400) {
            break;
        }
        if (hop >= MAX_REDIRECTS) {
            *status = -1;
            break;
        }
        esp_http_client_set_redirection(http);
        esp_http_client_close(http);
    }
    return http;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    const char *bad;
    TaskHandle_t waiter;
} rescue_job_t;

static void rescue_write_task(void *arg)
{
    rescue_job_t *j = arg;
    if (kvm_storage_rescue_write_begin(j->len) != ESP_OK) {
        j->bad = "the rescue partition is busy";
    } else {
        for (size_t off = 0; off < j->len && !j->bad; off += CHUNK) {
            const size_t n = j->len - off < CHUNK ? j->len - off : CHUNK;
            if (kvm_storage_rescue_write(j->data + off, n) != ESP_OK) {
                j->bad = "writing the rescue partition failed";
            }
        }
        if (j->bad) {
            kvm_storage_rescue_write_abort();
        } else if (kvm_storage_rescue_write_end() != ESP_OK) {
            j->bad = "finishing the rescue partition failed";
        }
    }
    xTaskNotifyGive(j->waiter);
    vTaskDelete(NULL);
}

static void fetch_task(void *arg)
{
    (void)arg;
    char *chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char url[URL_MAX];
    lock();
    strlcpy(url, s_job.url, sizeof(url));
    const bool rescue = s_job.rescue;
    char name[NAME_MAX_LEN + 1];
    strlcpy(name, s_job.name, sizeof(name));
    unlock();

    int64_t total = -1;
    int status = 0;
    esp_err_t err = ESP_OK;
    esp_http_client_handle_t http = NULL;
    esp_transport_handle_t tls = NULL;
    if (chunk) {
        /* ChaCha20 first (see kvm_web_chacha_suites); a plain http URL or a
         * server without it gets a second try with the default suites. */
        tls = strncmp(url, "https://", 8) == 0 ? kvm_web_chacha_transport() : NULL;
        http = open_url(url, tls, &total, &status, &err);
        if (tls && (!http || err != ESP_OK)) {
            esp_http_client_cleanup(http);
            esp_transport_destroy(tls);
            tls = NULL;
            ESP_LOGI(TAG, "no ChaCha20 there; trying the default cipher suites");
            http = open_url(url, NULL, &total, &status, &err);
        }
    }
    if (!http) {
        free(chunk);
        finish(F_ERROR, "out of memory");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    const char *bad = NULL;
    char why[96];
    if (err != ESP_OK) {
        snprintf(why, sizeof(why), "could not connect (%s)", esp_err_to_name(err));
        bad = why;
    } else if (status == -1) {
        bad = "too many redirects";
    } else if (status != 200) {
        snprintf(why, sizeof(why), "the server answered %d", status);
        bad = why;
    }
    if (!bad && esp_http_client_is_chunked_response(http)) {
        total = -1;
    }
    if (!bad && rescue) {
        const uint64_t cap = s_job.rescue_cap;
        if (total <= 0) {
            bad = "the server did not say how big the image is";
        } else if ((uint64_t)total > cap) {
            snprintf(why, sizeof(why), "%lld bytes do not fit the %u KB rescue partition",
                     (long long)total, (unsigned)(cap / 1024));
            bad = why;
        }
    }
    lock();
    s_job.total = total;
    unlock();

    char path[96] = {0};
    char part[104] = {0};
    int fd = -1;
    char *whole = NULL; /* the rescue image, held in PSRAM until it is all here */
    if (!bad) {
        if (rescue) {
            whole = heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!whole) {
                bad = "out of memory for the image";
            }
        } else {
            snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), name);
            snprintf(part, sizeof(part), "%s.part", path);
            fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                snprintf(why, sizeof(why), "could not create the file on the card (%d)", errno);
                bad = why;
            }
        }
    }

    uint64_t got = 0;
    int stalls = 0;
    while (!bad) {
        if (s_cancel) {
            bad = "cancelled";
            break;
        }
        const int n = esp_http_client_read(http, chunk, CHUNK);
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (++stalls > MAX_STALLS) {
                bad = "the server stopped sending";
            }
            continue;
        }
        if (n < 0) {
            bad = "the connection broke";
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(http) || total < 0) {
                break;
            }
            if (++stalls > MAX_STALLS) {
                bad = "the server stopped sending";
            }
            continue;
        }
        stalls = 0;
        if (rescue) {
            if (got + (uint64_t)n > (uint64_t)total) {
                bad = "the server sent more than it said";
                break;
            }
            memcpy(whole + got, chunk, (size_t)n);
        } else {
            for (int off = 0; off < n;) {
                const ssize_t w = write(fd, chunk + off, (size_t)(n - off));
                if (w <= 0) {
                    bad = "writing the card failed (full?)";
                    break;
                }
                off += (int)w;
            }
        }
        got += (uint64_t)n;
        lock();
        s_job.bytes = got;
        unlock();
        if (total > 0 && got >= (uint64_t)total) {
            break;
        }
    }
    if (!bad && total > 0 && got != (uint64_t)total) {
        bad = "the download ended early";
    }

    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (tls) {
        esp_transport_destroy(tls);
    }
    free(chunk);

    if (whole) {
        /* The connection is closed: now the flash may be erased and written. */
        if (!bad) {
            lock();
            snprintf(s_job.message, sizeof(s_job.message), "writing to flash");
            unlock();
            rescue_job_t job = {.data = (const uint8_t *)whole, .len = (size_t)total, .waiter = xTaskGetCurrentTaskHandle()};
            if (xTaskCreate(rescue_write_task, "rescue_wr", WRITE_STACK, &job, TASK_PRIO, NULL) != pdPASS) {
                bad = "no memory to write the flash";
            } else {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                bad = job.bad;
            }
        }
        free(whole);
    } else if (fd >= 0) {
        fsync(fd);
        close(fd);
        if (bad) {
            unlink(part);
        } else {
            unlink(path); /* replacing a file of the same name */
            if (rename(part, path) != 0) {
                bad = "could not rename the finished file";
                unlink(part);
            }
        }
    }
    finish(bad ? (s_cancel ? F_CANCELLED : F_ERROR) : F_DONE, bad ? bad : "done");
    vTaskDeleteWithCaps(NULL);
}

static const char *state_name(fetch_state_t s)
{
    switch (s) {
    case F_RUNNING: return "running";
    case F_DONE: return "done";
    case F_ERROR: return "error";
    case F_CANCELLED: return "cancelled";
    default: return "idle";
    }
}

static esp_err_t fetch_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    lock();
    const int64_t end = s_job.state == F_RUNNING ? esp_timer_get_time() : s_job.ended_us;
    const double secs = s_job.started_us ? (double)(end - s_job.started_us) / 1e6 : 0;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state_name(s_job.state));
    cJSON_AddStringToObject(root, "dest", s_job.rescue ? "rescue" : "card");
    cJSON_AddStringToObject(root, "url", s_job.url);
    cJSON_AddStringToObject(root, "name", s_job.name);
    cJSON_AddNumberToObject(root, "bytes", (double)s_job.bytes);
    cJSON_AddNumberToObject(root, "total", (double)s_job.total);
    cJSON_AddNumberToObject(root, "rateBps", secs > 0.5 ? (double)s_job.bytes / secs : 0);
    cJSON_AddStringToObject(root, "message", s_job.message);
    unlock();
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t fetch_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char body[URL_MAX + 160];
    const int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        const int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return send_json_error(req, "400 Bad Request", "could not read the body");
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    const cJSON *ju = j ? cJSON_GetObjectItem(j, "url") : NULL;
    const cJSON *jd = j ? cJSON_GetObjectItem(j, "dest") : NULL;
    const cJSON *jn = j ? cJSON_GetObjectItem(j, "name") : NULL;
    const char *url = cJSON_IsString(ju) ? ju->valuestring : "";
    const bool rescue = cJSON_IsString(jd) && strcmp(jd->valuestring, "rescue") == 0;
    char name[NAME_MAX_LEN + 1] = {0};
    if (cJSON_IsString(jn)) {
        strlcpy(name, jn->valuestring, sizeof(name));
    }
    const char *bad = NULL;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        bad = "the URL must start with http:// or https://";
    } else if (strlen(url) >= URL_MAX) {
        bad = "the URL is too long";
    }
    /* Read here, not in the task: its stack is in PSRAM, and reading the flash
     * turns the cache off. */
    kvm_rescue_t rs = {0};
    if (!bad && rescue) {
        kvm_storage_rescue_status(&rs);
        if (!rs.supported) {
            bad = "this device has no rescue partition (flash the full image once to get it)";
        }
    }
    if (!bad && !rescue) {
        kvm_storage_status_t sd;
        kvm_storage_status(&sd);
        const char *active = kvm_setting_str("msc_image");
        if (!kvm_storage_writable()) {
            bad = kvm_storage_write_unavailable_reason();
        } else if (!sd.mounted) {
            bad = "no microSD card mounted";
        } else if (kvm_record_active()) {
            bad = "a recording is running; stop it before downloading";
        } else if (!name_ok(name)) {
            bad = "give the file a plain name, like debian.iso";
        } else if (kvm_setting_bool("msc_enable") && active && strcmp(active, name) == 0) {
            bad = "that image is mounted; eject it before replacing";
        }
    }
    lock();
    if (!bad && s_job.state == F_RUNNING) {
        bad = "a download is already running";
    }
    if (!bad) {
        memset(&s_job, 0, sizeof(s_job));
        s_job.state = F_RUNNING;
        s_job.rescue = rescue;
        s_job.total = -1;
        s_job.rescue_cap = rs.capacity_bytes;
        s_job.started_us = esp_timer_get_time();
        strlcpy(s_job.url, url, sizeof(s_job.url));
        strlcpy(s_job.name, rescue ? "rescue" : name, sizeof(s_job.name));
        s_cancel = false;
    }
    unlock();
    cJSON_Delete(j);
    if (bad) {
        return send_json_error(req, "409 Conflict", bad);
    }
    if (xTaskCreateWithCaps(fetch_task, "url_fetch", TASK_STACK, NULL, TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        finish(F_ERROR, "no memory for the download task");
        return send_json_error(req, "503 Service Unavailable", "no memory for the download task");
    }
    ESP_LOGI(TAG, "downloading %s to %s", s_job.url, rescue ? "the rescue partition" : s_job.name);
    return fetch_get(req);
}

static esp_err_t fetch_cancel_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    s_cancel = true;
    return fetch_get(req);
}

static const httpd_uri_t k_routes[] = {
    {.uri = "/api/v1/storage/fetch", .method = HTTP_GET, .handler = fetch_get},
    {.uri = "/api/v1/storage/fetch", .method = HTTP_POST, .handler = fetch_post},
    {.uri = "/api/v1/storage/fetch/cancel", .method = HTTP_POST, .handler = fetch_cancel_post},
};

const httpd_uri_t *url_fetch_routes(size_t *count)
{
    *count = sizeof(k_routes) / sizeof(k_routes[0]);
    return k_routes;
}
