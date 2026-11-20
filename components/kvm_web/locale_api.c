/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device's own language for the console, kept in the flash "storage"
 * partition (FAT on wear levelling, mounted at /storage when first needed).
 * A browser that has not picked a language of its own uses this one, so a
 * device with no internet still opens in it, sign-in page included.
 *
 *   GET    /api/v1/locale   the language file, 404 when there is none (no sign-in needed)
 *   PUT    /api/v1/locale   keep this language file
 *
 * The device only stores the file. The console checks it every time it loads
 * one, from here or anywhere else, so a broken file leaves the console in
 * English rather than breaking it.
 *   DELETE /api/v1/locale   back to English
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "http_recv.h"
#include "kvm_auth.h"
#include "web_priv.h"

#define TAG "locale"
#define MOUNT "/storage"
#define FILE_PATH MOUNT "/locale.json"
#define TMP_PATH MOUNT "/locale.tmp"
#define CHUNK 4096
#define MAX_BYTES (512u * 1024u)

static SemaphoreHandle_t s_lock;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
/* The partition held no file system the last time it was tried: reading it
   again would only log the same error on every console that opens. */
static bool s_empty;

/*
 * Mount the partition once. @p format allows making a fresh file system, which
 * only a write does: reading an empty partition just means "no language".
 */
static bool mounted(bool format)
{
    if (s_wl != WL_INVALID_HANDLE) {
        return true;
    }
    if (s_empty && !format) {
        return false;
    }
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = format,
        .max_files = 2,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        s_wl = WL_INVALID_HANDLE;
        s_empty = true;
        if (format) {
            ESP_LOGW(TAG, "storage partition: %s", esp_err_to_name(err));
        }
        return false;
    }
    s_empty = false;
    return true;
}

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static esp_err_t locale_get(httpd_req_t *req)
{
    lock();
    FILE *f = mounted(false) ? fopen(FILE_PATH, "rb") : NULL;
    if (!f) {
        unlock();
        return send_json_error(req, "404 Not Found", "no device language");
    }
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        unlock();
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t r = ESP_OK;
    size_t n;
    while (r == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) {
        r = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    fclose(f);
    unlock();
    free(buf);
    return r == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : r;
}

static esp_err_t answer(httpd_req_t *req, bool kept)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, kept ? "{\"kept\":true}" : "{\"kept\":false}");
}

static esp_err_t locale_put(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (req->content_len <= 0 || (size_t)req->content_len > MAX_BYTES) {
        return send_json_error(req, "413 Payload Too Large", "a language file is at most 512 KB");
    }
    char *body = heap_caps_malloc((size_t)req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
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

    lock();
    bool ok = mounted(true);
    if (ok) {
        /* A whole new file first, then swapped in: a power cut leaves the old one. */
        FILE *f = fopen(TMP_PATH, "wb");
        ok = f && fwrite(body, 1, (size_t)got, f) == (size_t)got;
        if (f) {
            ok = fclose(f) == 0 && ok;
        }
        if (ok) {
            remove(FILE_PATH);
            ok = rename(TMP_PATH, FILE_PATH) == 0;
        } else {
            remove(TMP_PATH);
        }
    }
    unlock();
    free(body);
    if (!ok) {
        return send_json_error(req, "500 Internal Server Error", "could not write the storage partition");
    }
    ESP_LOGI(TAG, "device language kept, %d bytes", got);
    return answer(req, true);
}

static esp_err_t locale_delete(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    lock();
    if (mounted(false)) {
        remove(FILE_PATH);
    }
    unlock();
    return answer(req, false);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/locale", .method = HTTP_GET, .handler = locale_get},
    {.uri = "/api/v1/locale", .method = HTTP_PUT, .handler = locale_put},
    {.uri = "/api/v1/locale", .method = HTTP_DELETE, .handler = locale_delete},
};

const httpd_uri_t *locale_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
