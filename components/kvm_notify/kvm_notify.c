/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One low-priority task does two things: it watches for events worth a
 * notification (a watched phrase appearing, the screen going flat) and it
 * delivers the queue to the channels: Telegram (notify_tg.c), a webhook
 * (notify_hook.c) and the browsers that asked for Web Push (notify_push.c).
 * Each channel takes the kinds of event its mask in the settings names. The
 * delivery holds a TLS session and a copy of the screen JPEG for a moment;
 * both come from PSRAM, so the internal RAM the H.264 encoder needs is never
 * touched.
 *
 * What cannot go out because the network is down waits in a pending list and
 * is retried, oldest first, with the time it really happened added to it. The
 * screenshot and the log tail are taken when the event happens, not when it is
 * finally sent. A refusal (a bad token, a wrong chat) is not retried. With a
 * writable microSD card each waiting event is also a few files in .notify/, so
 * the list survives a restart and its screenshots do not sit in PSRAM.
 */
#include "kvm_notify.h"
#include "kvm_regions.h"
#include "notify_priv.h"

#include "capture.h"
#include "esp_heap_caps.h"
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
#define QUEUE_DEPTH 6

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

/* The channels, in the order a waiting event keeps its left[] for them. */
static const notify_channel_t *const s_channels[] = {
    &notify_channel_tg,
    &notify_channel_hook,
    &notify_channel_push,
};
#define NCH (sizeof(s_channels) / sizeof(s_channels[0]))
_Static_assert(NCH <= NOTIFY_CH_MAX, "more channels than a waiting event has room for");
_Static_assert(KVM_NOTIFY_KINDS < KVM_NOTIFY_ALL, "kinds overlap the everywhere value");

send_res_t notify_classify(int status)
{
    /* A 2xx is the server taking it, whatever went wrong with the body after:
       an HTTP/1.0 answer with no length ends in an error, and sending it again
       only repeated the message for ever. */
    if (status >= 200 && status < 300) {
        return SEND_OK;
    }
    if (status >= 400 && status < 500 && status != 408 && status != 429) {
        return SEND_REFUSED;
    }
    return SEND_RETRY;
}

static notify_pending_t *s_pending[PENDING_MAX];
static int s_npending;
static size_t s_pending_photo_bytes;
static unsigned s_dropped;        /* pushed out of a full list, not yet reported */
static int64_t s_next_retry_us;   /* 0 = try now */
static int64_t s_last_try_us;     /* when the backlog was last tried */
static uint32_t s_retry_ms = RETRY_FIRST_MS;
static uint32_t s_next_id = 1;
static bool s_card_loaded; /* the list on the card has been read back */

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static kvm_notify_status_t s_status;

bool notify_post(const notify_event_t *ev)
{
    /* Never block a caller (it might be the capture task): drop if the queue is
       full - a backlog of stale alerts helps no one. */
    return s_queue && xQueueSend(s_queue, ev, 0) == pdTRUE;
}

void kvm_notify_send(kvm_notify_ev_t kind, const char *title, const char *body, bool want_photo)
{
    notify_event_t ev = {.op = EV_SEND, .kind = kind, .want_photo = want_photo};
    strlcpy(ev.title, title ? title : "", sizeof(ev.title));
    strlcpy(ev.body, body ? body : "", sizeof(ev.body));
    (void)notify_post(&ev);
}

void kvm_notify_send_clip(const char *path, const char *card_path, const char *caption)
{
    notify_event_t ev = {.op = EV_CLIP, .kind = KVM_NOTIFY_CLIP};
    strlcpy(ev.title, "Dashcam clip", sizeof(ev.title));
    strlcpy(ev.body, caption ? caption : "", sizeof(ev.body));
    strlcpy(ev.path, path ? path : "", sizeof(ev.path));
    strlcpy(ev.card_path, card_path ? card_path : "", sizeof(ev.card_path));
    (void)notify_post(&ev);
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
void notify_url_encode(const char *src, char *dst, size_t cap)
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
void notify_json_escape(const char *src, char *dst, size_t cap)
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
    if (*out_len > NOTIFY_PHOTO_MAX) {
        free(jpeg);
        *out_len = 0;
        return NULL;
    }
    return jpeg;
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

static void card_remove(const notify_pending_t *p)
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

static void card_save_meta(const notify_pending_t *p)
{
    if (!p->id) {
        return;
    }
    char buf[sizeof(notify_event_t) * 2 + 160 + NCH * 24];
    /* "kind" is the operation, as it always was; "ev" what it is about. */
    size_t o = (size_t)snprintf(buf, sizeof(buf), "kind=%d\nev=%d\nat=%lld\nphoto=%d\nwant_photo=%d\nlog=%d\n",
                                (int)p->ev.op, (int)p->ev.kind, (long long)p->at, p->photo_file,
                                p->ev.want_photo, p->log_file);
    for (size_t c = 0; c < NCH; c++) {
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s=%lu\n", s_channels[c]->id,
                              (unsigned long)p->left[c]);
    }
    put_field(buf, sizeof(buf), &o, "title", p->ev.title);
    put_field(buf, sizeof(buf), &o, "body", p->ev.body);
    put_field(buf, sizeof(buf), &o, "path", p->ev.path);
    put_field(buf, sizeof(buf), &o, "card_path", p->ev.card_path);
    (void)card_write(p->id, "txt", buf, o < sizeof(buf) ? o : sizeof(buf) - 1);
}

/* Move a new event onto the card: its screenshot and log leave memory. */
static void card_store(notify_pending_t *p)
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

static void pending_free(notify_pending_t *p)
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
   happened, and the channels it still has to reach are marked: those set up
   whose mask takes this kind. */
static void pending_add(const notify_event_t *ev)
{
    uint32_t left[NCH] = {0};
    bool any_set_up = false, any = false, photo = false;
    for (size_t c = 0; c < NCH; c++) {
        const uint32_t t = s_channels[c]->targets();
        any_set_up |= t != 0;
        const bool takes = ev->kind == KVM_NOTIFY_ALL ||
                           ((uint32_t)kvm_setting_int(s_channels[c]->route_key) >> ev->kind) & 1u;
        left[c] = takes ? t : 0;
        any |= left[c] != 0;
        photo |= left[c] && s_channels[c]->photo;
    }
    if (!any_set_up) {
        set_result("no channel configured (set a Telegram bot, a webhook URL or a browser)");
        return;
    }
    if (!any) {
        return; /* no channel takes this kind: by choice, not a failure */
    }

    notify_pending_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    memcpy(p->left, left, sizeof(left));
    if (ev->op == EV_SEND) {
        if (photo && ev->want_photo && kvm_setting_bool("notify_snap") &&
            s_pending_photo_bytes < PENDING_PHOTO_BYTES &&
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > NOTIFY_PHOTO_MAX + NOTIFY_PSRAM_RESERVE) {
            p->photo = grab_photo(&p->photo_len);
            if (p->photo && (s_pending_photo_bytes + p->photo_len > PENDING_PHOTO_BYTES ||
                             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < NOTIFY_PSRAM_RESERVE)) {
                free(p->photo); /* it goes as text: memory matters more */
                p->photo = NULL;
                p->photo_len = 0;
            }
            s_pending_photo_bytes += p->photo_len;
        }
        /* The tail of the device log, so an alert carries the context that
           explains it - which is why the noisy tags are held down. */
        if (kvm_setting_bool("notify_log")) {
            p->logtail = heap_caps_malloc(NOTIFY_LOG_TAIL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (p->logtail) {
                kvm_log_read(p->logtail, NOTIFY_LOG_TAIL_MAX); /* keeps the newest end */
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
        notify_pending_t *p = meta ? heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                            : NULL;
        if (!p) {
            free(meta);
            continue;
        }
        p->id = ids[i];
        p->before_boot = true;
        p->ev.op = (notify_ev_op_t)atoi(meta_get(meta, "kind"));
        /* A file from before the kinds was a clip or an alert: either way it is
           already routed by its left[], so the kind only names it. */
        const char *ev = meta_get(meta, "ev");
        p->ev.kind = ev[0] ? (kvm_notify_ev_t)atoi(ev)
                           : p->ev.op == EV_CLIP ? KVM_NOTIFY_CLIP : KVM_NOTIFY_ALL;
        p->at = (time_t)atoll(meta_get(meta, "at"));
        for (size_t c = 0; c < NCH; c++) {
            p->left[c] = (uint32_t)strtoul(meta_get(meta, s_channels[c]->id), NULL, 10);
        }
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
void notify_compose(const notify_pending_t *p, char *text, size_t cap)
{
    int n;
    if (p->ev.op == EV_CLIP) {
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

const uint8_t *notify_photo(const notify_pending_t *p, size_t *len, uint8_t **loaded)
{
    *loaded = NULL;
    *len = p->photo_len;
    if (p->photo) {
        return p->photo;
    }
    /* Kept on the card: back for the send only. A card that has gone
       meanwhile just means a plain message. */
    if (p->photo_file && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > NOTIFY_PHOTO_MAX + NOTIFY_PSRAM_RESERVE) {
        *loaded = (uint8_t *)card_read(p->id, "jpg", NOTIFY_PHOTO_MAX, len);
    }
    if (!*loaded) {
        *len = 0;
    }
    return *loaded;
}

const char *notify_logtail(const notify_pending_t *p, char **loaded)
{
    *loaded = NULL;
    if (p->logtail) {
        return p->logtail;
    }
    size_t len;
    if (p->log_file) {
        *loaded = card_read(p->id, "log", NOTIFY_LOG_TAIL_MAX, &len);
    }
    return *loaded;
}

void notify_clip_text(const notify_pending_t *p, char *text, size_t cap)
{
    snprintf(text, cap, "%.160s - saved on the card as %.80s", p->ev.body, p->ev.card_path);
}

/* Try the oldest waiting event on each channel it still needs. Returns true if
   it is done with (sent or refused everywhere), false if it has to wait. */
static bool try_front(void)
{
    notify_pending_t *p = s_pending[0];
    bool refused = false, waiting = false;
    for (size_t c = 0; c < NCH; c++) {
        if (!p->left[c]) {
            continue;
        }
        if (!s_channels[c]->targets()) {
            p->left[c] = 0; /* the channel was switched off meanwhile */
            continue;
        }
        refused |= s_channels[c]->send(p, &p->left[c]);
        waiting |= p->left[c] != 0;
    }

    if (waiting) {
        card_save_meta(p); /* a channel that got through is not tried again after a restart */
        char msg[96];
        snprintf(msg, sizeof(msg), "no network to send on - %d waiting, retrying", s_npending);
        set_result(msg);
        return false;
    }
    set_result(refused ? (p->ev.op == EV_CLIP
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
        s_last_try_us = esp_timer_get_time();
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
        notify_event_t ev = {.op = EV_SEND, .kind = KVM_NOTIFY_ALL, .title = "Alerts dropped"};
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
        if (alerting) {
            char body[NOTIFY_BODY_MAX];
            snprintf(body, sizeof(body), "On the screen: %.180s", phrase);
            kvm_notify_send(KVM_NOTIFY_WATCH, "Screen alert", body, true);
        }
    }

    /* The screen going flat - a stop screen, a blanked output - for the same
       thirty seconds Home Assistant treats as a state, not a repaint. */
    static bool s_flat_seen;
    kvm_video_status_t v;
    capture_status_get(&v);
    const bool flat = v.signal && v.flat_ms >= 30000u;
    if (flat && !s_flat_seen) {
        kvm_notify_send(KVM_NOTIFY_FLAT, "Screen went blank", "The output has been one flat colour for a while.",
                        true);
    }
    s_flat_seen = flat;

    /* Screen areas that asked to be told, one message per event. An id given
       to a new area starts the count again: catch up, do not send. */
    static uint32_t s_area_seen[REGION_MAX + 1];
    kvm_region_status_t areas[REGION_MAX];
    const int n = kvm_regions_list(areas, REGION_MAX);
    for (int i = 0; i < n; i++) {
        const kvm_region_status_t *a = &areas[i];
        uint32_t *seen = &s_area_seen[a->id];
        if (a->events > *seen && a->notify) {
            char body[NOTIFY_BODY_MAX];
            kvm_regions_describe(a, body, sizeof(body));
            kvm_notify_send(KVM_NOTIFY_AREA, "Screen area", body, true);
        }
        *seen = a->events;
    }
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        notify_event_t ev;
        /* Wake for a queued event, or every POLL_MS to look for one. */
        bool got = xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(POLL_MS)) == pdTRUE;
        if (got && ev.op == EV_FIND_CHATS) {
            notify_tg_find_chats(); /* setting up, so it works with sending switched off */
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
            /* Something new: try now, the network may be back. But not more
               often than the first retry step, or a screen area that fires
               every second hammers a dead server. */
            if (esp_timer_get_time() - s_last_try_us >= (int64_t)RETRY_FIRST_MS * 1000) {
                s_next_retry_us = 0;
            }
        }
        flush_pending();
        poll_events();
    }
}

void kvm_notify_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    notify_tg_init();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(notify_event_t));
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

size_t kvm_notify_ready_json(char *out, size_t cap)
{
    size_t o = 0;
    for (size_t c = 0; c < NCH && o < cap; c++) {
        o += (size_t)snprintf(out + o, cap - o, "%s\"%s\":%s", c ? "," : "{", s_channels[c]->route_key,
                              s_channels[c]->targets() ? "true" : "false");
    }
    if (o + 2 > cap) {
        return 0;
    }
    out[o++] = '}';
    out[o] = '\0';
    return o;
}
