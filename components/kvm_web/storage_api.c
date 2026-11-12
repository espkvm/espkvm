/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual media: image files on the microSD, uploads, and the rescue image.
 */
#include "http_server.h"

#include "sdkconfig.h" /* the CONFIG_ guards below are read before any of them */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "kvm_ota_trial.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

/* espcoredump only puts its header on the include path when core dumps are
 * enabled, so a build with them off must not ask for it. Everything that uses
 * it is behind the same guard. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

#include "esp_https_server.h"
#include "esp_tls.h"
#include "esp_netif.h"

#include "lwip/sockets.h"

#include "cJSON.h"

#include "http_recv.h"

#include "capture.h"
#include "screentext.h"
#include "screentext_store.h"
#include "ethernet.h"
#include "kvm_ipv6.h"
#include "wifi.h"
#include "coproc.h"
#include "kvm_rtc.h"
#include "kvm_atx.h"
#include "kvm_mqtt.h"
#include "kvm_ts.h"
#include "kvm_wg.h"
#include "kvm_auth.h"
#include "fw_install.h"
#include "kvm_notify.h"
#include "kvm_sched.h"
#include "runbook.h"
#include "kvm_board_header.h"
#include "kvm_caps.h"
#include "kvm_display.h"
#include "kvm_log.h"
#include "kvm_settings.h"
#include "kvm_profiles.h"
#include "kvm_record.h"
#include "web_priv.h"
#include "http_priv.h"
#include "kvm_storage.h"
#include "kvm_thermal.h"
#include "kvm_tls.h"
#include "kvm_acme.h"
#include "kvm_tools.h"
#include "kvm_proto.h"
#include "usb_hid.h"
#include "video_frame.h"

static const char *TAG = "web";

/* ---- virtual media: image files on the microSD -------------------------- */

#define IMAGE_NAME_MAX 63

/*
 * A file name is a plain entry in the card's root: not empty, short enough for
 * FATFS, no path separators and no "..". This is the only gate between a web
 * request and the filesystem, so it must refuse anything that could reach
 * outside the mount point. The device serves one card to possibly untrusted
 * viewers; a traversal here would hand them the whole VFS.
 */
static bool image_name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) > IMAGE_NAME_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20) {
            return false;
        }
    }
    /* FAT refuses " * : < > ? | too, and a quote would break the JSON answer. */
    return !strpbrk(name, "/\\\"*:<>?|") && !strstr(name, "..");
}

/* Decode %XX escapes in place; a browser sends a space in a filename as %20.
 * '+' is left literal - in a path it is a real character, not a space. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Pull ?name= from the query and decode it. False when absent or unsafe. */
static bool image_name_from_query(httpd_req_t *req, char *out, size_t outlen)
{
    char query[200];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "name", out, outlen) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return image_name_ok(out);
}

/* Full "/sd/<name>" path into buf. Caller has already validated the name. */
static void image_path(char *buf, size_t buflen, const char *name)
{
    int n = snprintf(buf, buflen, "%s/%s", kvm_storage_mount_point(), name);
    /* A truncated path names a different file than the caller asked for, so it
     * is emptied instead - every caller then fails to open it, which is the
     * right answer. Names are validated and short, so this cannot happen in
     * practice; it is here because a silent cut is the dangerous kind. */
    if (n < 0 || (size_t)n >= buflen) {
        buf[0] = '\0';
    }
}

esp_err_t api_storage_images_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_storage_status_t sd;
    kvm_storage_status(&sd);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddBoolToObject(root, "mounted", sd.mounted);
    cJSON_AddNumberToObject(root, "totalBytes", (double)sd.total_bytes);
    cJSON_AddNumberToObject(root, "freeBytes", (double)sd.free_bytes);
    if (sd.mounted) {
        cJSON_AddNumberToObject(root, "busKhz", sd.bus_khz);
        cJSON_AddNumberToObject(root, "busErrors", sd.bus_errors);
        cJSON_AddNumberToObject(root, "busMaxKhz", sd.bus_max_khz);
        cJSON_AddNumberToObject(root, "busRetryS", sd.bus_retry_s);
    }
    cJSON_AddStringToObject(root, "active", kvm_setting_str("msc_image"));
    /* Whether the console may upload or delete: this board serves the card
     * read-only, so those controls are disabled with the reason below. */
    cJSON_AddBoolToObject(root, "writable", kvm_storage_writable());
    if (!kvm_storage_writable()) {
        cJSON_AddStringToObject(root, "writeReason", kvm_storage_write_unavailable_reason());
    }
    /* The built-in rescue image on flash, offered alongside the card's files.
     * Writable from the console because this board's flash writes are reliable,
     * unlike the card's. */
    kvm_rescue_t rescue;
    kvm_storage_rescue_status(&rescue);
    cJSON *rj = cJSON_AddObjectToObject(root, "rescue");
    if (rj) {
        cJSON_AddBoolToObject(rj, "supported", rescue.supported);
        cJSON_AddBoolToObject(rj, "hasImage", rescue.has_image);
        cJSON_AddNumberToObject(rj, "capacityBytes", (double)rescue.capacity_bytes);
    }
    /* While the whole card is handed to the target, the firmware stays off the
     * filesystem, so there is no file list to give and uploads are disabled. */
    const bool handed_over = kvm_storage_card_handed_over();
    cJSON_AddBoolToObject(root, "handedOver", handed_over);
    cJSON *images = cJSON_AddArrayToObject(root, "images");

    if (sd.mounted && !handed_over && images) {
        DIR *dir = opendir(kvm_storage_mount_point());
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') {
                    continue; /* dotfiles and volume metadata are not media */
                }
                /* Skip corrupt directory entries: a name carrying control bytes
                 * is a damaged FAT record, not a file anyone can select. */
                bool printable = true;
                for (const char *c = ent->d_name; *c; c++) {
                    if ((unsigned char)*c < 0x20) {
                        printable = false;
                        break;
                    }
                }
                if (!printable) {
                    continue;
                }
                char path[128];
                image_path(path, sizeof(path), ent->d_name);
                struct stat st;
                if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                    continue;
                }
                cJSON *item = cJSON_CreateObject();
                if (!item) {
                    break;
                }
                cJSON_AddStringToObject(item, "name", ent->d_name);
                /* off_t is 32-bit here; exFAT files may pass 4 GB, so ask FatFs. */
                uint64_t size = (uint32_t)st.st_size;
                (void)kvm_storage_file_size(ent->d_name, &size);
                cJSON_AddNumberToObject(item, "size", (double)size);
                cJSON_AddItemToArray(images, item);
            }
            closedir(dir);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    return send_json_owned(req, json); /* takes ownership, frees it */
}

/*
 * Uploads run on their own task, not on the server's single control task.
 *
 * An image is measured in gigabytes, and reading it in on the control task
 * would hold that task for the whole transfer - during which the video channel
 * gets no frames sent and no new TLS handshake is accepted. The browser, seeing
 * its stream die, reconnects; with the control task busy those handshakes pile
 * up against a seven-socket TLS pool and the whole interface stalls until a
 * reboot. The same async hand-off the MJPEG stream uses keeps the control task
 * free while the bytes land on the card.
 */
#define UPLOAD_WORKER_STACK (8 * 1024)
/* Just below the control task (idle+6), like the stream worker, so it never
 * starves the task accepting connections. */
#define UPLOAD_WORKER_PRIO (tskIDLE_PRIORITY + 5)
/* Give up if the sender goes quiet this many receive-timeouts in a row, so a
 * dropped connection leaves neither a zombie task nor a half-written file. */
#define UPLOAD_MAX_IDLE 3
/*
 * The body is read into big buffers and a second task writes them to the card,
 * so the network is not waiting while the card writes (on a Function EV at
 * 40 MHz each costs about the same). Buffers are PSRAM, cache-line aligned so
 * the SD driver sends each in one DMA multi-block command. UPLOAD_BUFS bounds
 * the memory; when all are queued the reader waits, and TCP slows the sender.
 */
#define UPLOAD_CHUNK (256 * 1024)
#define UPLOAD_BUFS 4
#define UPLOAD_CHUNK_MIN (32 * 1024)
#define UPLOAD_WRITER_STACK (4 * 1024)

typedef struct {
    httpd_req_t *req;
    char name[IMAGE_NAME_MAX + 1];
    size_t content_len;
    uint64_t offset; /* where this part goes; 0 starts a new file */
} upload_ctx_t;

typedef struct {
    char *data;
    size_t len;
} upload_buf_t;

typedef struct {
    int fd;
    QueueHandle_t full;  /* reader -> writer; a NULL data pointer ends it */
    QueueHandle_t empty; /* writer -> reader */
    SemaphoreHandle_t done;
    volatile bool failed;
    int64_t write_us;
} upload_writer_t;

static void upload_writer_task(void *arg)
{
    upload_writer_t *w = (upload_writer_t *)arg;
    upload_buf_t b;
    while (xQueueReceive(w->full, &b, portMAX_DELAY) == pdTRUE && b.data) {
        if (!w->failed) {
            const int64_t t0 = esp_timer_get_time();
            if (write(w->fd, b.data, b.len) != (ssize_t)b.len) {
                ESP_LOGE(TAG, "write failed: %s", strerror(errno));
                w->failed = true;
            }
            w->write_us += esp_timer_get_time() - t0;
        }
        xQueueSend(w->empty, &b, portMAX_DELAY);
    }
    xSemaphoreGive(w->done);
    vTaskDelete(NULL);
}

static void upload_finish(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    httpd_handle_t server = req->handle;
    httpd_req_async_handler_complete(req);
    /* A finished upload is not worth keeping alive, and leaving it in the pool
     * risks the same ENOTCONN accumulation the stream guards against. */
    if (server && fd >= 0) {
        (void)httpd_sess_trigger_close(server, fd);
    }
}

static void upload_worker_task(void *arg)
{
    upload_ctx_t *ctx = (upload_ctx_t *)arg;
    httpd_req_t *req = ctx->req;

    char path[128];
    image_path(path, sizeof(path), ctx->name);
    /* A part after the first appends; the handler checked it lines up. Not
     * stdio: fopen "ab" seeks through a 32-bit off_t, which breaks at 2 GB.
     * write() with O_APPEND seeks inside FatFs, in 64 bits. */
    const int flags = O_WRONLY | O_CREAT | (ctx->offset ? O_APPEND : O_TRUNC);
    upload_writer_t w = {.fd = open(path, flags, 0644)};
    if (w.fd < 0) {
        ESP_LOGE(TAG, "cannot create '%s': %s", path, strerror(errno));
        send_json_error(req, "500 Internal Server Error", "cannot create file on card");
        upload_finish(req);
        free(ctx);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "image upload: %zu bytes at %llu -> %s", ctx->content_len,
             (unsigned long long)ctx->offset, path);
    video_frame_upload_begin();

    const char *why = "out of memory";
    bool ok = true;
    w.full = xQueueCreate(UPLOAD_BUFS + 1, sizeof(upload_buf_t));
    w.empty = xQueueCreate(UPLOAD_BUFS, sizeof(upload_buf_t));
    w.done = xSemaphoreCreateBinary();
    ok = w.full && w.empty && w.done;
    /* PSRAM is mostly taken by video, and what is left can be in pieces: take
     * smaller buffers, or fewer, rather than refuse the upload. */
    size_t chunk = UPLOAD_CHUNK;
    int bufs = 0;
    while (ok && bufs < UPLOAD_BUFS) {
        upload_buf_t b = {.data = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_SPIRAM)};
        if (b.data) {
            xQueueSend(w.empty, &b, 0);
            bufs++;
        } else if (bufs < 2 && chunk > UPLOAD_CHUNK_MIN) {
            chunk /= 2;
            while (xQueueReceive(w.empty, &b, 0) == pdTRUE) { /* all one size */
                free(b.data);
            }
            bufs = 0;
        } else {
            break;
        }
    }
    ok = ok && bufs > 0;
    if (ok && (chunk != UPLOAD_CHUNK || bufs != UPLOAD_BUFS)) {
        ESP_LOGW(TAG, "upload: %d buffers of %u KB (PSRAM largest %u KB)", bufs,
                 (unsigned)(chunk / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    } else if (!ok) {
        ESP_LOGE(TAG, "upload: no buffers (PSRAM free %u KB, largest %u KB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    }
    const bool writer = ok && xTaskCreate(upload_writer_task, "kvm_upload_wr", UPLOAD_WRITER_STACK,
                                          &w, UPLOAD_WORKER_PRIO, NULL) == pdPASS;
    ok = writer;

    const int64_t started_us = esp_timer_get_time();
    size_t received = 0;
    int idle = 0;
    while (ok && received < ctx->content_len) {
        upload_buf_t b = {0};
        xQueueReceive(w.empty, &b, portMAX_DELAY);
        b.len = 0; /* it comes back holding the length it was written with */
        /* Fill the buffer, or take whatever arrived before a lull. recv hands
         * back a TLS record at a time; batching them makes a few large writes. */
        while (b.len < chunk && received + b.len < ctx->content_len) {
            const size_t room = chunk - b.len;
            const size_t left = ctx->content_len - received - b.len;
            const int n = httpd_req_recv(req, b.data + b.len, room < left ? room : left);
            if (kvm_recv_stalled(n)) {
                if (++idle >= UPLOAD_MAX_IDLE) {
                    ESP_LOGE(TAG, "upload stalled after %zu of %zu bytes", received + b.len,
                             ctx->content_len);
                    why = "upload stalled; the network is too slow or dropped";
                    ok = false;
                }
                break; /* hand over what we have, then re-check ok */
            }
            idle = 0;
            if (n <= 0) {
                ESP_LOGE(TAG, "upload cut short after %zu of %zu bytes", received + b.len,
                         ctx->content_len);
                why = "upload cut short by the client";
                ok = false;
                break;
            }
            b.len += (size_t)n;
        }
        received += b.len;
        if (b.len) {
            xQueueSend(w.full, &b, portMAX_DELAY);
        } else {
            xQueueSend(w.empty, &b, portMAX_DELAY);
        }
        if (w.failed) {
            why = "write to the card failed; it may be full";
            ok = false;
        }
    }

    if (writer) {
        const upload_buf_t end = {0};
        xQueueSend(w.full, &end, portMAX_DELAY);
        xSemaphoreTake(w.done, portMAX_DELAY);
        if (w.failed) {
            why = "write to the card failed; it may be full";
            ok = false;
        }
    }
    upload_buf_t b;
    while (w.empty && xQueueReceive(w.empty, &b, 0) == pdTRUE) {
        free(b.data);
    }
    if (w.full) {
        vQueueDelete(w.full);
    }
    if (w.empty) {
        vQueueDelete(w.empty);
    }
    if (w.done) {
        vSemaphoreDelete(w.done);
    }
    close(w.fd);
    video_frame_upload_end();

    if (!ok) {
        remove(path); /* a half-written image is worse than none */
        send_json_error(req, "500 Internal Server Error", why);
    } else {
        const int64_t ms = (esp_timer_get_time() - started_us) / 1000;
        ESP_LOGW(TAG, "image '%s' written, %zu bytes in %lld ms (%llu KB/s), card %lld ms",
                 ctx->name, received, (long long)ms,
                 (unsigned long long)(ms > 0 ? (uint64_t)received / (uint64_t)ms : 0),
                 (long long)(w.write_us / 1000));
        char body[128];
        int bn = snprintf(body, sizeof(body),
                          "{\"status\":\"written\",\"name\":\"%s\",\"size\":%llu}",
                          ctx->name, (unsigned long long)(ctx->offset + received));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, body, bn);
    }
    upload_finish(req);
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t api_storage_upload_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (!kvm_storage_writable()) {
        return send_json_error(req, "501 Not Implemented",
                               kvm_storage_write_unavailable_reason());
    }
    kvm_storage_status_t sd;
    kvm_storage_status(&sd);
    if (!sd.mounted) {
        return send_json_error(req, "409 Conflict", "no microSD card mounted");
    }
    /* An upload holds the stream to 2 fps and fights the recording for the card. */
    if (kvm_record_active()) {
        return send_json_error(req, "409 Conflict", "a recording is running; stop it before uploading");
    }

    char name[IMAGE_NAME_MAX + 1];
    if (!image_name_from_query(req, name, sizeof(name))) {
        return send_json_error(req, "400 Bad Request", "missing or invalid ?name=");
    }
    /* Never write the file the target is currently reading: it is held open
     * read-only and there is no file-level lock, so overwriting it underneath
     * the target would hand it corruption. Eject first, then upload. */
    const char *active = kvm_setting_str("msc_image");
    if (kvm_setting_bool("msc_enable") && active && strcmp(active, name) == 0) {
        return send_json_error(req, "409 Conflict",
                               "image is mounted; eject it before replacing");
    }
    if (req->content_len <= 0) {
        return send_json_error(req, "400 Bad Request", "empty body");
    }
    /* content_len is 32-bit: a 5 GB body would read as 1 GB and leave a stub.
     * A bigger file comes in parts, each with ?offset= of where it goes. */
    char clen[24];
    if (httpd_req_get_hdr_value_str(req, "Content-Length", clen, sizeof(clen)) == ESP_OK &&
        strtoull(clen, NULL, 10) > UINT32_MAX) {
        return send_json_error(req, "413 Payload Too Large",
                               "send a file of 4 GB and over in parts (?offset=)");
    }
    uint64_t offset = 0;
    char query[200], val[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "offset", val, sizeof(val)) == ESP_OK) {
        offset = strtoull(val, NULL, 10);
    }
    if (offset) {
        /* Append only right after the last part, so a lost part cannot leave
         * a hole or a doubled piece. */
        uint64_t have = 0;
        if (kvm_storage_file_size(name, &have) != ESP_OK || have != offset) {
            return send_json_error(req, "409 Conflict",
                                   "offset does not match the file on the card; start again");
        }
    }

    upload_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);
    ctx->content_len = req->content_len;
    ctx->offset = offset;

    /* Hand the socket to the worker; the control task returns at once. */
    esp_err_t res = httpd_req_async_handler_begin(req, &ctx->req);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "upload async begin: %s", esp_err_to_name(res));
        free(ctx);
        return send_json_error(req, "500 Internal Server Error", "upload busy");
    }
    if (xTaskCreate(upload_worker_task, "kvm_upload", UPLOAD_WORKER_STACK, ctx, UPLOAD_WORKER_PRIO,
                    NULL) != pdPASS) {
        httpd_req_async_handler_complete(ctx->req);
        free(ctx);
        return send_json_error(req, "503 Service Unavailable", "upload task");
    }
    return ESP_OK;
}

esp_err_t api_storage_delete_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    /* The whole card handed to the target, or a card this board only reads. */
    if (!kvm_storage_writable()) {
        return send_json_error(req, "409 Conflict", kvm_storage_write_unavailable_reason());
    }
    /* Freeing a large file rewrites the FAT under a recording's feet. */
    if (kvm_record_active()) {
        return send_json_error(req, "409 Conflict", "a recording is running; stop it before deleting");
    }
    char name[IMAGE_NAME_MAX + 1];
    if (!image_name_from_query(req, name, sizeof(name))) {
        return send_json_error(req, "400 Bad Request", "missing or invalid ?name=");
    }
    const char *active = kvm_setting_str("msc_image");
    if (kvm_setting_bool("msc_enable") && active && strcmp(active, name) == 0) {
        return send_json_error(req, "409 Conflict", "image is mounted; eject it before deleting");
    }
    char path[128];
    image_path(path, sizeof(path), name);
    if (remove(path) != 0) {
        return send_json_error(req, errno == ENOENT ? "404 Not Found" : "500 Internal Server Error",
                               errno == ENOENT ? "no such image" : "could not delete");
    }
    ESP_LOGI(TAG, "image '%s' deleted", name);
    return api_storage_images_get(req);
}

/*
 * Write an image into the on-flash rescue partition.
 *
 * Streamed straight into flash like a firmware update, and small (a few MB at
 * most), so unlike the gigabyte card uploads it runs inline on the control task
 * rather than on a worker. Flash writes are reliable on this board, which is why
 * this is offered where card writes are not. To boot from it afterwards, select
 * the rescue image as the active medium (msc_image = "@rescue") with virtual
 * media enabled.
 */
static esp_err_t api_storage_rescue_post_body(httpd_req_t *req);

/* The stream slows down for the length of the upload (video_frame_upload_begin). */
esp_err_t api_storage_rescue_post(httpd_req_t *req)
{
    video_frame_upload_begin();
    const esp_err_t ret = api_storage_rescue_post_body(req);
    video_frame_upload_end();
    return ret;
}

static esp_err_t api_storage_rescue_post_body(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_rescue_t rescue;
    kvm_storage_rescue_status(&rescue);
    if (!rescue.supported) {
        return send_json_error(req, "409 Conflict",
                               "no rescue partition; flash the current partition table first");
    }
    if (req->content_len <= 0) {
        return send_json_error(req, "400 Bad Request", "empty body");
    }
    if ((size_t)req->content_len > rescue.capacity_bytes) {
        return send_json_error(req, "413 Payload Too Large", "image larger than the rescue partition");
    }
    esp_err_t err = kvm_storage_rescue_write_begin((size_t)req->content_len);
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "rescue upload: %d bytes", req->content_len);

    char chunk[2048];
    int received = 0;
    int stalls = 0;
    while (received < req->content_len) {
        const int want = (int)sizeof(chunk) < (req->content_len - received)
                             ? (int)sizeof(chunk)
                             : (req->content_len - received);
        const int n = httpd_req_recv(req, chunk, (size_t)want);
        if (kvm_recv_stalled(n)) {
            if (++stalls > KVM_RECV_MAX_STALLS) {
                kvm_storage_rescue_write_abort();
                return send_json_error(req, "408 Request Timeout", "the upload went quiet");
            }
            continue;
        }
        stalls = 0;
        if (n <= 0) {
            kvm_storage_rescue_write_abort();
            return send_json_error(req, "400 Bad Request", "upload was cut short");
        }
        err = kvm_storage_rescue_write(chunk, (size_t)n);
        if (err != ESP_OK) {
            kvm_storage_rescue_write_abort();
            return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
        }
        received += n;
    }
    kvm_storage_rescue_write_end();
    ESP_LOGW(TAG, "rescue image written (%d bytes)", req->content_len);
    return api_storage_images_get(req); /* echo the new state, incl. rescue.hasImage */
}
