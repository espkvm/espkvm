/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The upload queue and the task that empties it. One file at a time, oldest
 * first. What happened to each stays in the list until newer ones push it out,
 * so the console can show "done" or why not. The task starts with the first
 * file and stays; its stack is in PSRAM, where the room for a TLS session is.
 *
 * Files that still wait are listed in a small file on the card, so a restart
 * or a power cut does not lose them. When the server cannot be reached, the
 * whole queue waits and tries again later, half a minute at first and up to
 * ten minutes, instead of failing every file in turn.
 */
#include "kvm_upload.h"
#include "upload_priv.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_settings.h"
#include "kvm_storage.h"
#include "video_frame.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "upload";

#define ITEMS_MAX 32
#define REL_MAX 80
#define TASK_STACK 8192
#define TASK_PRIO 3 /* below the video and web tasks: an upload can wait */
#define QUEUE_FILE ".espkvm-upload" /* in the card's root: what still waits */
#define RETRY_FIRST_S 30
#define RETRY_MAX_S 600
/* While someone watches, a pause after each chunk keeps the upload near 1 MB/s,
   so it does not take the card and the network from the live picture. */
#define WATCHED_PAUSE_MS 25

typedef enum { U_QUEUED, U_SENDING, U_DONE, U_ERROR, U_CANCELLED } item_state_t;

typedef struct {
    char rel[REL_MAX];
    item_state_t state;
    bool automatic; /* queued by itself, not by a click */
    uint64_t bytes, total;
    char message[96];
} item_t;

static const upload_dest_t *const s_dest = &upload_dest_dav;

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_task;
static item_t s_items[ITEMS_MAX];
static int s_count;
static volatile bool s_cancel;
static int s_current = -1; /* the item going now */
static bool s_loaded;      /* the card's list was read */
static bool s_dirty;       /* the card's list is behind */
static int64_t s_hold_until; /* the server did not answer: nothing goes before this */
static int s_fails;          /* tries in a row that did not reach the server */

static enum { T_IDLE, T_WANTED, T_RUNNING, T_OK, T_ERROR } s_test;
static char s_test_msg[96];

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

bool kvm_upload_ready(void)
{
    return s_dest->ready();
}

static bool progress(uint64_t bytes)
{
    lock();
    if (s_current >= 0) {
        s_items[s_current].bytes = bytes;
    }
    unlock();
    if (video_frame_viewer_count() > 0) {
        vTaskDelay(pdMS_TO_TICKS(WATCHED_PAUSE_MS));
    }
    return !s_cancel;
}

/* ---- the list on the card ---------------------------------------------------- */

static void queue_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", kvm_storage_mount_point(), QUEUE_FILE);
}

/* Write what still waits, or remove the file when nothing does. Called with
   the lock free: it copies the list first. */
static void save_list(void)
{
    static char lines[ITEMS_MAX][REL_MAX + 3];
    int n = 0;
    lock();
    if (!s_dirty) {
        unlock();
        return;
    }
    s_dirty = false;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].state == U_QUEUED || s_items[i].state == U_SENDING) {
            lines[n][0] = s_items[i].automatic ? 'a' : 'm';
            lines[n][1] = ' ';
            strlcpy(lines[n] + 2, s_items[i].rel, sizeof(lines[0]) - 2);
            n++;
        }
    }
    unlock();
    if (!kvm_storage_writable()) {
        lock();
        s_dirty = true; /* written when the card can take it */
        unlock();
        return;
    }
    char path[96], tmp[100];
    queue_path(path, sizeof(path));
    if (n == 0) {
        remove(path);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        return;
    }
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n", lines[i]);
    }
    fclose(f);
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
    }
}

static void init_once(void)
{
    if (!s_mu) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);
        if (!s_mu) {
            s_mu = xSemaphoreCreateMutex();
        }
        portEXIT_CRITICAL(&mux);
    }
}

/* ---- the queue ------------------------------------------------------------------ */

/* The oldest file still waiting, marked as going. -1 when there is none, or
   while the queue waits for the server; then *wait_ms says how long. */
static int take_next(uint32_t *wait_ms)
{
    int found = -1;
    *wait_ms = portMAX_DELAY;
    lock();
    const int64_t now = esp_timer_get_time();
    bool waiting = false;
    for (int i = 0; i < s_count && found < 0; i++) {
        if (s_items[i].state != U_QUEUED) {
            continue;
        }
        if (s_hold_until > now) {
            waiting = true;
            break;
        }
        s_items[i].state = U_SENDING;
        s_current = found = i;
        s_cancel = false; /* under the lock, so a cancel cannot fall between */
    }
    if (waiting) {
        *wait_ms = (uint32_t)((s_hold_until - now) / 1000) + 10;
    }
    unlock();
    return found;
}

static void finish(int i, item_state_t st, const char *msg)
{
    lock();
    s_items[i].state = st;
    strlcpy(s_items[i].message, msg ? msg : "", sizeof(s_items[i].message));
    s_current = -1;
    s_dirty = true;
    unlock();
}

static void send_one(int i)
{
    char rel[REL_MAX];
    lock();
    strlcpy(rel, s_items[i].rel, sizeof(rel));
    unlock();
    char full[128];
    snprintf(full, sizeof(full), "%s/%s", kvm_storage_mount_point(), rel);
    struct stat st;
    if (stat(full, &st) != 0) {
        finish(i, U_ERROR, "the file is no longer on the card");
        return;
    }
    /* off_t is 32-bit and the recorder stops a file at 3.9 GB: read it unsigned. */
    const uint64_t size = (uint32_t)st.st_size;
    lock();
    s_items[i].total = size;
    s_items[i].bytes = 0;
    s_items[i].message[0] = '\0';
    unlock();
    const int64_t t0 = esp_timer_get_time();
    const upload_res_t r = s_dest->put(full, rel, size, progress);
    if (s_cancel) {
        finish(i, U_CANCELLED, "cancelled");
    } else if (r.ok) {
        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "%s: %llu KB in %lld ms", rel, (unsigned long long)(size / 1024), (long long)ms);
        lock();
        s_fails = 0;
        unlock();
        finish(i, U_DONE, "");
    } else if (r.retry) {
        /* Back in line, first: the whole queue waits for the server. */
        lock();
        s_fails++;
        int s = RETRY_FIRST_S;
        for (int k = 1; k < s_fails && s < RETRY_MAX_S; k++) {
            s *= 2;
        }
        s = s > RETRY_MAX_S ? RETRY_MAX_S : s;
        s_hold_until = esp_timer_get_time() + (int64_t)s * 1000000;
        s_items[i].state = U_QUEUED;
        s_items[i].bytes = 0;
        strlcpy(s_items[i].message, r.message, sizeof(s_items[i].message));
        s_current = -1;
        unlock();
        ESP_LOGW(TAG, "%s: %s; trying again in %d s", rel, r.message, s);
    } else {
        ESP_LOGW(TAG, "%s: %s", rel, r.message);
        finish(i, U_ERROR, r.message);
    }
}

static void task(void *arg)
{
    (void)arg;
    uint32_t wait_ms = portMAX_DELAY;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, wait_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms));
        lock();
        const bool test = s_test == T_WANTED;
        if (test) {
            s_test = T_RUNNING;
        }
        unlock();
        if (test) {
            const upload_res_t r = s_dest->test();
            lock();
            s_test = r.ok ? T_OK : T_ERROR;
            strlcpy(s_test_msg, r.message, sizeof(s_test_msg));
            unlock();
        }
        save_list();
        int i;
        while ((i = take_next(&wait_ms)) >= 0) {
            send_one(i);
            save_list();
        }
    }
}

static esp_err_t wake(void)
{
    if (!s_task) {
        /* PSRAM stack: 8 KB of internal RAM is often not there with H.264 on. */
        if (xTaskCreatePinnedToCoreWithCaps(task, "upload", TASK_STACK, NULL, TASK_PRIO, &s_task,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

/* Add one file under the lock. ESP_OK also when it is already on its way. */
static esp_err_t add_locked(const char *rel, bool automatic)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_items[i].rel, rel) == 0 &&
            (s_items[i].state == U_QUEUED || s_items[i].state == U_SENDING)) {
            return ESP_OK;
        }
    }
    if (s_count == ITEMS_MAX) {
        /* Room is made by forgetting the oldest finished one, never a waiting one. */
        int drop = -1;
        for (int i = 0; i < s_count && drop < 0; i++) {
            if (s_items[i].state != U_QUEUED && s_items[i].state != U_SENDING) {
                drop = i;
            }
        }
        if (drop < 0) {
            return ESP_ERR_NO_MEM;
        }
        memmove(&s_items[drop], &s_items[drop + 1], (size_t)(s_count - drop - 1) * sizeof(item_t));
        s_count--;
        if (s_current > drop) {
            s_current--;
        }
    }
    item_t *it = &s_items[s_count++];
    memset(it, 0, sizeof(*it));
    strlcpy(it->rel, rel, sizeof(it->rel));
    it->state = U_QUEUED;
    it->automatic = automatic;
    s_dirty = true;
    return ESP_OK;
}

/* A file and, for a video, the subtitles beside it. */
static esp_err_t add(const char *rel, bool automatic)
{
    if (!rel || !rel[0] || strlen(rel) >= REL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char srt[REL_MAX];
    strlcpy(srt, rel, sizeof(srt));
    char *dot = strrchr(srt, '.');
    bool subs = false;
    if (dot && (strcmp(dot, ".ts") == 0 || strcmp(dot, ".mp4") == 0) && (size_t)(dot - srt) + 5 < sizeof(srt)) {
        strcpy(dot, ".srt");
        uint64_t sz;
        subs = kvm_storage_file_size(srt, &sz) == ESP_OK;
    }
    lock();
    esp_err_t err = add_locked(rel, automatic);
    if (err == ESP_OK && subs) {
        (void)add_locked(srt, automatic);
    }
    unlock();
    return err;
}

/* The files that waited when the device went down, once a card is there. */
static void load_list(void)
{
    lock();
    const bool done = s_loaded;
    unlock();
    kvm_storage_status_t st;
    kvm_storage_status(&st);
    if (done || !st.mounted) {
        return;
    }
    lock();
    s_loaded = true;
    unlock();
    char path[96];
    queue_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[REL_MAX + 4];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) > 2 && line[1] == ' ' && !strstr(line, "..")) {
            lock();
            if (add_locked(line + 2, line[0] == 'a') == ESP_OK) {
                n++;
            }
            unlock();
        }
    }
    fclose(f);
    if (n) {
        ESP_LOGI(TAG, "%d files still to upload from before the restart", n);
    }
}

esp_err_t kvm_upload_queue(const char *rel)
{
    init_once();
    if (!s_dest->ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    load_list();
    const esp_err_t err = add(rel, false);
    if (err != ESP_OK) {
        return err;
    }
    /* A click means now: it does not wait out a pause for the server. */
    lock();
    s_hold_until = 0;
    unlock();
    return wake();
}

void kvm_upload_auto(kvm_upload_kind_t kind, const char *rel)
{
    init_once();
    if (!(kvm_setting_int("up_dav_ev") & (1 << kind)) || !s_dest->ready()) {
        return;
    }
    load_list();
    const esp_err_t err = add(rel, true);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: queued by itself", rel);
        (void)wake();
    } else {
        ESP_LOGW(TAG, "%s: not queued (%s)", rel, esp_err_to_name(err));
    }
}

void kvm_upload_start(void)
{
    init_once();
    if (!s_dest->ready()) {
        return;
    }
    load_list();
    lock();
    bool any = false;
    for (int i = 0; i < s_count && !any; i++) {
        any = s_items[i].state == U_QUEUED;
    }
    unlock();
    if (any) {
        (void)wake();
    }
}

void kvm_upload_cancel(void)
{
    init_once();
    lock();
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].state == U_QUEUED) {
            s_items[i].state = U_CANCELLED;
            strlcpy(s_items[i].message, "cancelled", sizeof(s_items[i].message));
        }
    }
    s_cancel = s_current >= 0;
    s_dirty = true;
    unlock();
    if (s_task) {
        xTaskNotifyGive(s_task); /* the list on the card is rewritten by the task */
    }
}

esp_err_t kvm_upload_test(void)
{
    init_once();
    if (!s_dest->ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (s_test != T_RUNNING) {
        s_test = T_WANTED;
        s_test_msg[0] = '\0';
    }
    unlock();
    return wake();
}

/* What goes into the JSON: the paths are checked names, the messages ours, but
   a server's answer can end up in a message, so quotes are escaped anyway. */
static size_t put_escaped(char *out, size_t cap, size_t o, const char *s)
{
    for (; *s && o + 3 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    return o;
}

size_t kvm_upload_status_json(char *out, size_t cap)
{
    static const char *const states[] = {"queued", "sending", "done", "error", "cancelled"};
    static const char *const tests[] = {"idle", "running", "running", "ok", "error"};
    init_once();
    lock();
    const int64_t now = esp_timer_get_time();
    const unsigned retry_in = s_hold_until > now ? (unsigned)((s_hold_until - now) / 1000000) + 1 : 0;
    size_t o = (size_t)snprintf(out, cap, "{\"ready\":%s,\"dest\":\"%s\",\"test\":{\"state\":\"%s\",\"message\":\"",
                                s_dest->ready() ? "true" : "false", s_dest->name, tests[s_test]);
    o = put_escaped(out, cap, o, s_test_msg);
    o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "\"},\"items\":[");
    for (int i = 0; i < s_count && o < cap; i++) {
        const item_t *it = &s_items[i];
        o += (size_t)snprintf(out + o, cap - o, "%s{\"path\":\"", i ? "," : "");
        o = put_escaped(out, cap, o, it->rel);
        o += (size_t)snprintf(out + o, o < cap ? cap - o : 0,
                              "\",\"state\":\"%s\",\"bytes\":%llu,\"total\":%llu,\"retryIn\":%u,"
                              "\"auto\":%s,\"message\":\"",
                              states[it->state], (unsigned long long)it->bytes, (unsigned long long)it->total,
                              it->state == U_QUEUED ? retry_in : 0u, it->automatic ? "true" : "false");
        o = put_escaped(out, cap, o, it->message);
        o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "\"}");
    }
    unlock();
    o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "]}");
    return o < cap ? o : 0;
}
