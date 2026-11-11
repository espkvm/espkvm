/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The REST side of recording: start, stop and status, a screenshot, and the
 * files both leave on the card - listed, downloaded and deleted. Only VIDEO/
 * and SCREENSHOTS/ are reachable here, never the rest of the card.
 */
#include <ctype.h>
#include <strings.h> /* strncasecmp: the search ignores case */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_auth.h"
#include "kvm_record.h"
#include "kvm_storage.h"
#include "kvm_upload.h"
#include "web_priv.h"

#define TAG "web"

#define LIST_MAX 500
#define DOWNLOAD_CHUNK (64 * 1024)
#define DOWNLOAD_STACK (4 * 1024)
#define DOWNLOAD_PRIO (tskIDLE_PRIORITY + 5)
/* Each one holds a socket for as long as the file takes; the server has few. */
#define DOWNLOADS_MAX 2

static volatile int s_downloads;

/* ---- recording and screenshots ---------------------------------------------- */

/* 2025-01-01: a clock before this was never set. */
#define CLOCK_SET_EPOCH 1735689600LL

/*
 * Files are named by the clock, and a device without NTP never has one - its
 * files would be "up-000123". The console sends the browser's time when it signs
 * in and with ?t=, taken only while the device's own clock is unset, so NTP
 * still wins.
 */
void kvm_web_clock_from_browser(long long t)
{
    if ((long long)time(NULL) >= CLOCK_SET_EPOCH || t < CLOCK_SET_EPOCH || t > 4102444800LL) {
        return;
    }
    const struct timeval tv = {.tv_sec = (time_t)t};
    if (settimeofday(&tv, NULL) == 0) {
        ESP_LOGI(TAG, "clock set from the console");
    }
}

static void clock_from_query(httpd_req_t *req)
{
    char query[64], value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "t", value, sizeof(value)) == ESP_OK) {
        kvm_web_clock_from_browser(strtoll(value, NULL, 10));
    }
}

static esp_err_t send_record_status(httpd_req_t *req)
{
    kvm_record_status_t rec;
    kvm_record_status(&rec);
    char body[640];
    const int n = snprintf(body, sizeof(body),
                           "{\"on\":%s,\"file\":\"%s\",\"seconds\":%u,\"bytes\":%llu,\"frames\":%u,"
                           "\"dropped\":%u,\"stopped\":\"%s\",\"event\":%s,\"dashcam\":%s,"
                           "\"prerollSeconds\":%u,\"timelapse\":%u,\"clipSecondsLeft\":%u,"
                           "\"clipsConverting\":%u,\"lastClip\":\"%s\",\"audio\":%s}",
                           rec.recording ? "true" : "false", rec.file, (unsigned)rec.seconds,
                           (unsigned long long)rec.bytes, (unsigned)rec.frames,
                           (unsigned)rec.dropped, rec.stopped, rec.event ? "true" : "false",
                           rec.dashcam ? "true" : "false", (unsigned)rec.preroll_seconds,
                           (unsigned)rec.timelapse_every, (unsigned)rec.clip_seconds_left,
                           (unsigned)rec.clips_converting, rec.last_clip, rec.audio ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(body)) {
        return send_json_error(req, "500 Internal Server Error", "status too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

static esp_err_t record_status_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_record_status(req);
}

static esp_err_t record_start_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    clock_from_query(req);
    /* ?seconds=N records for that long, whatever the length setting says;
     * ?every=N makes it a timelapse of one frame every N seconds. */
    unsigned long seconds = 0, every = 0;
    char query[96], value[16];
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    if (has_query && httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
        seconds = strtoul(value, NULL, 10);
        if (seconds < 1 || seconds > 86400 * 7) {
            return send_json_error(req, "400 Bad Request", "seconds wants 1..604800");
        }
    }
    if (has_query && httpd_query_key_value(query, "every", value, sizeof(value)) == ESP_OK) {
        every = strtoul(value, NULL, 10);
        if (every < 1 || every > KVM_TIMELAPSE_EVERY_MAX) {
            return send_json_error(req, "400 Bad Request", "every wants 1..3600 seconds");
        }
    }
    char why[128] = "";
    const esp_err_t err = every ? kvm_record_start_timelapse((uint32_t)every, (uint32_t)seconds, why, sizeof(why))
                                : kvm_record_start((uint32_t)seconds, why, sizeof(why));
    if (err != ESP_OK) {
        return send_json_error(req, "409 Conflict", why);
    }
    return send_record_status(req);
}

static esp_err_t record_stop_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_record_stop("stopped by the operator");
    return send_record_status(req);
}

/* "Save clip": what the dashcam holds, and a little after. */
static esp_err_t record_event_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    clock_from_query(req);
    char why[96] = "";
    if (!kvm_record_event("Saved by hand", why, sizeof(why))) {
        return send_json_error(req, "409 Conflict", why);
    }
    return send_record_status(req);
}

static esp_err_t screenshot_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    clock_from_query(req);
    char file[64], why[128] = "";
    if (kvm_record_screenshot(file, sizeof(file), why, sizeof(why)) != ESP_OK) {
        return send_json_error(req, "409 Conflict", why);
    }
    char body[128];
    const int n = snprintf(body, sizeof(body), "{\"file\":\"%s\"}", file);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* ---- the files -------------------------------------------------------------- */

static bool name_ok(const char *name, const char *ext)
{
    const size_t n = strlen(name), e = strlen(ext);
    if (n <= e || n > 48 || name[0] == '.' || strcmp(name + n - e, ext) != 0) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
            return false;
        }
    }
    return true;
}

/* "VIDEO/<name>.ts" or its ".srt"/".txt", or "SCREENSHOTS/<name>.jpg" - nothing else. */
static bool path_ok(const char *path, bool *video)
{
    const size_t vd = strlen(KVM_RECORD_DIR), sd = strlen(KVM_SCREENSHOT_DIR);
    if (strncmp(path, KVM_RECORD_DIR "/", vd + 1) == 0) {
        *video = true;
        return name_ok(path + vd + 1, ".ts") || name_ok(path + vd + 1, ".mp4") ||
               name_ok(path + vd + 1, ".srt") || name_ok(path + vd + 1, ".txt");
    }
    if (strncmp(path, KVM_SCREENSHOT_DIR "/", sd + 1) == 0) {
        *video = false;
        return name_ok(path + sd + 1, ".jpg");
    }
    return false;
}

static void url_decode_inplace(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            const char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static bool path_from_query(httpd_req_t *req, char *out, size_t cap, bool *video)
{
    char query[192];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", out, cap) != ESP_OK) {
        return false;
    }
    url_decode_inplace(out);
    return path_ok(out, video);
}

/* Readable from here: a card is mounted and the firmware still owns it. */
static const char *files_blocked(void)
{
    kvm_storage_status_t sd;
    kvm_storage_status(&sd);
    if (!sd.mounted) {
        return "no microSD card in the slot";
    }
    if (kvm_storage_card_handed_over()) {
        return "the whole card is handed to the target";
    }
    return NULL;
}

static cJSON *list_dir(const char *dir, const char *ext, const char *ext2)
{
    cJSON *arr = cJSON_CreateArray();
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), dir);
    DIR *d = opendir(path);
    if (!d) {
        return arr; /* nothing saved yet */
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < LIST_MAX) {
        const bool second = ext2 && name_ok(e->d_name, ext2);
        if (!name_ok(e->d_name, ext) && !second) {
            continue;
        }
        /* name_ok() capped the name at 48, so these always fit. */
        char full[128];
        snprintf(full, sizeof(full), "%s/%.48s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        char rel[80];
        snprintf(rel, sizeof(rel), "%s/%.48s", dir, e->d_name);
        cJSON_AddStringToObject(item, "path", rel);
        uint64_t size = (uint32_t)st.st_size; /* stat() wraps past 2 GB */
        (void)kvm_storage_file_size(rel, &size);
        cJSON_AddNumberToObject(item, "size", (double)size);
        /* What sits beside a recording under the same name: the keystroke
         * subtitles, and the screen's text for the search. */
        const char *dot = strrchr(full, '.');
        const char *rdot = strrchr(rel, '.');
        if (strcmp(ext, ".ts") == 0 && dot && rdot) {
            static const char *const k_side[][2] = {{".srt", "subtitles"}, {".txt", "text"}};
            for (size_t i = 0; i < sizeof(k_side) / sizeof(k_side[0]); i++) {
                char side[128];
                snprintf(side, sizeof(side), "%.*s%s", (int)(dot - full), full, k_side[i][0]);
                if (stat(side, &st) == 0 && S_ISREG(st.st_mode)) {
                    char side_rel[80];
                    snprintf(side_rel, sizeof(side_rel), "%.*s%s", (int)(rdot - rel), rel, k_side[i][0]);
                    cJSON_AddStringToObject(item, k_side[i][1], side_rel);
                }
            }
        }
        cJSON_AddItemToArray(arr, item);
        count++;
    }
    closedir(d);
    return arr;
}

static esp_err_t captures_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *root = cJSON_CreateObject();
    const char *blocked = files_blocked();
    cJSON_AddItemToObject(root, "blocked", blocked ? cJSON_CreateString(blocked) : cJSON_CreateNull());
    if (!blocked) {
        cJSON_AddItemToObject(root, "video", list_dir(KVM_RECORD_DIR, ".ts", ".mp4"));
        cJSON_AddItemToObject(root, "screenshots", list_dir(KVM_SCREENSHOT_DIR, ".jpg", NULL));
    }
    /* The console offers Upload only with somewhere to send it. */
    cJSON_AddBoolToObject(root, "canUpload", !blocked && kvm_upload_ready());
    /* No deleting while a recording runs: it would rewrite the FAT under it. */
    cJSON_AddBoolToObject(root, "canDelete", !blocked && !kvm_record_active() && kvm_storage_writable());
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return r;
}

typedef struct {
    httpd_req_t *req;
    char path[96];
    char rel[80];
    bool video;
    /* A Range request: bytes first..last of size. Without one, the whole file. */
    bool ranged;
    uint64_t first, last, size;
} download_t;

static void download_finish(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    httpd_handle_t server = req->handle;
    httpd_req_async_handler_complete(req);
    if (server && fd >= 0) {
        (void)httpd_sess_trigger_close(server, fd);
    }
}

static void download_task(void *arg)
{
    download_t *dl = arg;
    httpd_req_t *req = dl->req;
    /* Not stdio: fseek works in a 32-bit off_t, and a recording can pass 2 GB. */
    kvm_file_t *f = kvm_storage_file_open(dl->rel, dl->ranged ? dl->first : 0);
    uint8_t *buf = heap_caps_malloc(DOWNLOAD_CHUNK, MALLOC_CAP_SPIRAM);
    if (!f || !buf) {
        send_json_error(req, f ? "500 Internal Server Error" : "404 Not Found",
                        f ? "out of memory" : "no such file");
    } else {
        const char *name = strrchr(dl->path, '/') + 1;
        const size_t nl = strlen(name);
        const bool srt = nl > 4 && strcmp(name + nl - 4, ".srt") == 0;
        const bool mp4 = nl > 4 && strcmp(name + nl - 4, ".mp4") == 0;
        const bool txt = nl > 4 && strcmp(name + nl - 4, ".txt") == 0;
        char disp[96];
        /* A .ts or subtitles download; an MP4 or a screenshot opens in the tab. */
        const bool inline_view = mp4 || txt || !dl->video;
        snprintf(disp, sizeof(disp), "%s; filename=\"%.48s\"", inline_view ? "inline" : "attachment", name);
        httpd_resp_set_type(req, srt   ? "application/x-subrip"
                                 : txt ? "text/plain; charset=utf-8"
                                 : mp4 ? "video/mp4"
                                 : dl->video ? "video/mp2t" : "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        /* Ranges let a player seek: the browser's own for an MP4, the console's
         * for a .ts. The body stays chunked; Content-Range carries the size. */
        httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
        char range[64];
        uint64_t left = UINT64_MAX;
        if (dl->ranged) {
            snprintf(range, sizeof(range), "bytes %llu-%llu/%llu", (unsigned long long)dl->first,
                     (unsigned long long)dl->last, (unsigned long long)dl->size);
            httpd_resp_set_status(req, "206 Partial Content");
            httpd_resp_set_hdr(req, "Content-Range", range);
            left = dl->last - dl->first + 1;
        }
        int n = 0;
        esp_err_t err = ESP_OK;
        while (err == ESP_OK && left &&
               (n = kvm_storage_file_read(f, buf, left < DOWNLOAD_CHUNK ? (size_t)left : DOWNLOAD_CHUNK)) > 0) {
            err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n);
            left -= left == UINT64_MAX ? 0 : n;
        }
        /* A card error part way: no closing chunk, so the browser sees a cut
         * download rather than a short file that looks whole. */
        if (err == ESP_OK && n >= 0) {
            httpd_resp_send_chunk(req, NULL, 0);
        }
    }
    kvm_storage_file_close(f);
    heap_caps_free(buf);
    download_finish(req);
    free(dl);
    s_downloads--;
    vTaskDelete(NULL);
}

static esp_err_t captures_file_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const char *blocked = files_blocked();
    if (blocked) {
        return send_json_error(req, "409 Conflict", blocked);
    }
    char rel[80];
    bool video = false;
    if (!path_from_query(req, rel, sizeof(rel), &video)) {
        return send_json_error(req, "400 Bad Request", "missing or invalid ?path=");
    }
    if (s_downloads >= DOWNLOADS_MAX) {
        return send_json_error(req, "503 Service Unavailable", "two downloads are already running");
    }
    download_t *dl = calloc(1, sizeof(*dl));
    if (!dl) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    snprintf(dl->path, sizeof(dl->path), "%s/%s", kvm_storage_mount_point(), rel);
    snprintf(dl->rel, sizeof(dl->rel), "%s", rel);
    dl->video = video;
    uint64_t fsize = 0;
    if (kvm_storage_file_size(rel, &fsize) != ESP_OK) {
        free(dl);
        return send_json_error(req, "404 Not Found", "no such file");
    }
    /* "bytes=first-", "bytes=first-last" or "bytes=-suffix"; one range only. */
    char range[48];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK &&
        strncmp(range, "bytes=", 6) == 0 && fsize > 0) {
        const uint64_t size = fsize;
        char *p = range + 6, *end;
        uint64_t first, last = size - 1;
        if (*p == '-') {
            const uint64_t suffix = strtoull(p + 1, &end, 10);
            first = suffix >= size ? 0 : size - suffix;
        } else {
            first = strtoull(p, &end, 10);
            if (*end == '-' && end[1] >= '0' && end[1] <= '9') {
                last = strtoull(end + 1, &end, 10);
            }
        }
        if (first >= size || last < first) {
            free(dl);
            char cr[40];
            snprintf(cr, sizeof(cr), "bytes */%llu", (unsigned long long)size);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            return send_json_error(req, "416 Range Not Satisfiable", "that range is not in the file");
        }
        dl->ranged = true;
        dl->first = first;
        dl->last = last < size ? last : size - 1;
        dl->size = size;
    }
    /* A file of any size would tie up the control task; it goes to a worker. */
    if (httpd_req_async_handler_begin(req, &dl->req) != ESP_OK) {
        free(dl);
        return send_json_error(req, "503 Service Unavailable", "download busy");
    }
    s_downloads++;
    if (xTaskCreate(download_task, "kvm_download", DOWNLOAD_STACK, dl, DOWNLOAD_PRIO, NULL) != pdPASS) {
        s_downloads--;
        httpd_req_async_handler_complete(dl->req);
        free(dl);
        return send_json_error(req, "503 Service Unavailable", "download task");
    }
    return ESP_OK;
}

/* ---- searching the screen's text ---------------------------------------------- */

#define SEARCH_LINE 12800
#define SEARCH_HITS 50
#define SEARCH_FILES 64

static bool contains_nocase(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* Where the match is, with a little either side, on one line. */
static void snippet(const char *line, const char *needle, char *out, size_t cap)
{
    const size_t n = strlen(needle);
    const char *at = line;
    for (; *at; at++) {
        if (strncasecmp(at, needle, n) == 0) {
            break;
        }
    }
    const size_t before = 40;
    const char *from = (size_t)(at - line) > before ? at - before : line;
    snprintf(out, cap, "%s%.*s", from == line ? "" : "...", (int)(cap - 8), from);
}

/*
 * Every .txt the recorder wrote, searched for a phrase. Each answer says which
 * recording and how far into it, so the console can play from there.
 */
static esp_err_t captures_search_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const char *blocked = files_blocked();
    if (blocked) {
        return send_json_error(req, "409 Conflict", blocked);
    }
    char query[192], needle[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "q", needle, sizeof(needle)) != ESP_OK || !needle[0]) {
        return send_json_error(req, "400 Bad Request", "missing ?q=");
    }
    url_decode_inplace(needle); /* the phrase arrives percent-encoded */
    const char *decoded = needle;

    char *line = heap_caps_malloc(SEARCH_LINE, MALLOC_CAP_SPIRAM);
    cJSON *root = cJSON_CreateObject();
    cJSON *hits = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "hits", hits);
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%s", kvm_storage_mount_point(), KVM_RECORD_DIR);
    DIR *d = line ? opendir(dir) : NULL;
    struct dirent *e;
    int files = 0, found = 0;
    while (d && (e = readdir(d)) != NULL && files < SEARCH_FILES && found < SEARCH_HITS) {
        const size_t nl = strlen(e->d_name);
        if (nl < 5 || nl > 48 || strcmp(e->d_name + nl - 4, ".txt") != 0) {
            continue;
        }
        char path[128];
        snprintf(path, sizeof(path), "%s/%.48s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        files++;
        /* The video the text belongs to: the .ts, or the .mp4 it became. */
        char video[80];
        snprintf(video, sizeof(video), "%s/%.*s.ts", KVM_RECORD_DIR, (int)(nl - 4), e->d_name);
        char check[128];
        snprintf(check, sizeof(check), "%s/%s", kvm_storage_mount_point(), video);
        struct stat st;
        if (stat(check, &st) != 0) {
            snprintf(video, sizeof(video), "%s/%.*s.mp4", KVM_RECORD_DIR, (int)(nl - 4), e->d_name);
        }
        while (found < SEARCH_HITS && fgets(line, SEARCH_LINE, f)) {
            char *tab = strchr(line, '\t');
            if (!tab) {
                continue;
            }
            *tab = '\0';
            char *text = tab + 1;
            text[strcspn(text, "\n")] = '\0';
            if (!contains_nocase(text, decoded)) {
                continue;
            }
            char around[160];
            snippet(text, decoded, around, sizeof(around));
            cJSON *hit = cJSON_CreateObject();
            cJSON_AddStringToObject(hit, "path", video);
            cJSON_AddNumberToObject(hit, "seconds", (double)strtoll(line, NULL, 10) / 1000.0);
            cJSON_AddStringToObject(hit, "text", around);
            cJSON_AddItemToArray(hits, hit);
            found++;
        }
        fclose(f);
    }
    if (d) {
        closedir(d);
    }
    heap_caps_free(line);
    cJSON_AddBoolToObject(root, "more", found >= SEARCH_HITS);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return r;
}

static esp_err_t captures_delete_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (kvm_record_active()) {
        return send_json_error(req, "409 Conflict", "a recording is running; stop it before deleting");
    }
    if (!kvm_storage_writable()) {
        return send_json_error(req, "409 Conflict", kvm_storage_write_unavailable_reason());
    }
    char rel[80];
    bool video = false;
    if (!path_from_query(req, rel, sizeof(rel), &video)) {
        return send_json_error(req, "400 Bad Request", "missing or invalid ?path=");
    }
    char path[112];
    snprintf(path, sizeof(path), "%s/%s", kvm_storage_mount_point(), rel);
    if (remove(path) != 0) {
        return send_json_error(req, errno == ENOENT ? "404 Not Found" : "500 Internal Server Error",
                               errno == ENOENT ? "no such file" : "could not delete");
    }
    /* A recording takes its subtitles and its screen text with it. */
    char *dot = strrchr(path, '.');
    if (video && dot && strcmp(dot, ".srt") != 0 && (size_t)(dot - path) + 5 <= sizeof(path)) {
        strcpy(dot, ".srt");
        (void)remove(path);
        strcpy(dot, ".txt");
        (void)remove(path);
    }
    ESP_LOGI(TAG, "deleted %s", rel);
    return captures_get(req);
}

/*
 * Uploading to the operator's WebDAV folder (kvm_upload).
 *
 *   POST /api/v1/captures/upload?path=VIDEO/x.ts   queue one file (its .srt goes too)
 *
 * With up_dav_ev set, kvm_record queues finished files by itself.
 *   GET  /api/v1/captures/upload                   the queue and how each file went
 *   POST /api/v1/captures/upload/cancel
 *   POST /api/v1/captures/upload/test              make the device's folder there
 *
 * The path is checked the same way as a download, so only what the recorder
 * and the screenshot button wrote can leave the card.
 */
static esp_err_t send_upload_status(httpd_req_t *req)
{
    const size_t cap = 8192;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    if (!kvm_upload_status_json(body, cap)) {
        free(body);
        return send_json_error(req, "500 Internal Server Error", "status too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_sendstr(req, body);
    free(body);
    return r;
}

static esp_err_t upload_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_upload_status(req);
}

static esp_err_t upload_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const char *blocked = files_blocked();
    if (blocked) {
        return send_json_error(req, "409 Conflict", blocked);
    }
    char rel[80];
    bool video = false;
    if (!path_from_query(req, rel, sizeof(rel), &video)) {
        return send_json_error(req, "400 Bad Request", "missing or invalid ?path=");
    }
    (void)video;
    /* A recording's subtitles go with it: kvm_upload adds them. */
    esp_err_t err = kvm_upload_queue(rel);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "no upload folder is set (Settings, Video, Upload)");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable", "the upload queue is full");
    }
    return send_upload_status(req);
}

static esp_err_t upload_cancel_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_upload_cancel();
    return send_upload_status(req);
}

static esp_err_t upload_test_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (kvm_upload_test() == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "no upload folder is set");
    }
    return send_upload_status(req);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/v1/record/status", .method = HTTP_GET, .handler = record_status_get},
    {.uri = "/api/v1/record/start", .method = HTTP_POST, .handler = record_start_post},
    {.uri = "/api/v1/record/stop", .method = HTTP_POST, .handler = record_stop_post},
    {.uri = "/api/v1/record/event", .method = HTTP_POST, .handler = record_event_post},
    {.uri = "/api/v1/screenshot", .method = HTTP_POST, .handler = screenshot_post},
    {.uri = "/api/v1/captures", .method = HTTP_GET, .handler = captures_get},
    {.uri = "/api/v1/captures/file", .method = HTTP_GET, .handler = captures_file_get},
    {.uri = "/api/v1/captures/search", .method = HTTP_GET, .handler = captures_search_get},
    {.uri = "/api/v1/captures/delete", .method = HTTP_POST, .handler = captures_delete_post},
    {.uri = "/api/v1/captures/upload", .method = HTTP_GET, .handler = upload_get},
    {.uri = "/api/v1/captures/upload", .method = HTTP_POST, .handler = upload_post},
    {.uri = "/api/v1/captures/upload/cancel", .method = HTTP_POST, .handler = upload_cancel_post},
    {.uri = "/api/v1/captures/upload/test", .method = HTTP_POST, .handler = upload_test_post},
};

const httpd_uri_t *record_api_routes(size_t *count)
{
    *count = sizeof(s_routes) / sizeof(s_routes[0]);
    return s_routes;
}
