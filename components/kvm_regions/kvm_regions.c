/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen areas: storage, the JSON for the API, and the look at each frame.
 * The arithmetic is in region_core.c.
 */
#include "kvm_regions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "kvm_settings.h"

static const char *TAG = "regions";

#define NS "kvm_rgn"
#define KEY "cfg"
/* Bumped when region_cfg_t changes shape: an old blob is dropped, not misread. */
#define CFG_VERSION 1
/* A look five times a second: a change shows within 200 ms, and eight areas
   cost a few thousand reads of a frame already in cache. */
#define SCAN_MS 200

typedef struct {
    uint8_t version;
    region_cfg_t cfg[REGION_MAX];
} stored_t;

static SemaphoreHandle_t s_mu;
static stored_t s_store;
static region_state_t s_st[REGION_MAX];
static uint8_t s_last[REGION_MAX][REGION_CELLS]; /* the latest signature, for "remember" */
static bool s_have_last[REGION_MAX];
static int64_t s_next_us;
static bool s_any;
static bool s_any_notify;
static volatile int s_waiters; /* runbooks waiting on an area right now */
static volatile uint32_t s_gen;

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static uint32_t now_ms(void)
{
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ms ? ms : 1u;
}

static void recount(void)
{
    s_any = false;
    s_any_notify = false;
    for (int i = 0; i < REGION_MAX; i++) {
        s_any |= s_store.cfg[i].id != 0;
        s_any_notify |= s_store.cfg[i].id != 0 && s_store.cfg[i].notify;
    }
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void kvm_regions_init(void)
{
    if (s_mu) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_store);
        if (nvs_get_blob(h, KEY, &s_store, &len) != ESP_OK || len != sizeof(s_store) ||
            s_store.version != CFG_VERSION) {
            memset(&s_store, 0, sizeof(s_store));
        }
        nvs_close(h);
    }
    s_store.version = CFG_VERSION;
    recount();
    ESP_LOGI(TAG, "%s", s_any ? "watching screen areas" : "no screen areas");
}

void kvm_regions_waiting(bool on)
{
    s_waiters += on ? 1 : -1;
}

bool kvm_regions_due_unwatched(void)
{
    if (!s_mu || !s_any || esp_timer_get_time() < s_next_us) {
        return false;
    }
    /* With nobody watching, a look is worth a frame only if something acts
       on it. The dashcam needs no help: recording keeps frames coming. */
    return s_waiters > 0 || (s_any_notify && kvm_setting_bool("notify_enable")) ||
           kvm_setting_bool("mqtt_enable");
}

void kvm_regions_scan(const region_frame_t *f)
{
    if (!s_mu || !s_any) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_next_us) {
        return;
    }
    s_next_us = now_us + SCAN_MS * 1000;
    const uint32_t now = now_ms();
    lock();
    for (int i = 0; i < REGION_MAX; i++) {
        const region_cfg_t *c = &s_store.cfg[i];
        if (!c->id) {
            continue;
        }
        region_look_t look;
        if (!region_look(f, &c->rect, &look)) {
            continue;
        }
        memcpy(s_last[i], look.sig, REGION_CELLS);
        s_have_last[i] = true;
        if (region_step(c, &s_st[i], &look, now)) {
            ESP_LOGI(TAG, "\"%s\": %s (%u%%)", c->name, region_mode_name(c->mode),
                     (unsigned)s_st[i].diff);
        }
    }
    unlock();
}

void kvm_regions_forget(void)
{
    if (!s_mu) {
        return;
    }
    lock();
    for (int i = 0; i < REGION_MAX; i++) {
        region_reset(&s_st[i]);
        s_have_last[i] = false;
    }
    unlock();
}

/* ---- JSON ------------------------------------------------------------------ */

static double frac(uint16_t v)
{
    return round((double)v / REGION_SCALE * 10000.0) / 10000.0;
}

char *kvm_regions_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "max", REGION_MAX);
    cJSON *arr = cJSON_AddArrayToObject(root, "regions");
    const uint32_t now = now_ms();
    lock();
    for (int i = 0; i < REGION_MAX; i++) {
        const region_cfg_t *c = &s_store.cfg[i];
        if (!c->id) {
            continue;
        }
        const region_state_t *s = &s_st[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->id);
        cJSON_AddStringToObject(o, "name", c->name);
        cJSON_AddNumberToObject(o, "x", frac(c->rect.x));
        cJSON_AddNumberToObject(o, "y", frac(c->rect.y));
        cJSON_AddNumberToObject(o, "w", frac(c->rect.w));
        cJSON_AddNumberToObject(o, "h", frac(c->rect.h));
        cJSON_AddStringToObject(o, "mode", region_mode_name(c->mode));
        cJSON_AddNumberToObject(o, "threshold", c->threshold);
        cJSON_AddNumberToObject(o, "hold", c->hold_s);
        cJSON_AddStringToObject(o, "colour", region_colour_name(c->colour));
        cJSON_AddBoolToObject(o, "notify", c->notify);
        cJSON_AddBoolToObject(o, "clip", c->clip);
        cJSON_AddBoolToObject(o, "hasRef", c->has_ref);
        cJSON *live = cJSON_AddObjectToObject(o, "live");
        cJSON_AddBoolToObject(live, "seen", s_have_last[i]);
        cJSON_AddBoolToObject(live, "active", s->active);
        cJSON_AddNumberToObject(live, "diff", s->diff);
        cJSON_AddNumberToObject(live, "events", s->events);
        cJSON_AddStringToObject(live, "colour", region_colour_name(s->colour));
        cJSON_AddNumberToObject(live, "colourPct", s->colour_pct);
        char rgb[8];
        snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", s->rgb[0], s->rgb[1], s->rgb[2]);
        cJSON_AddStringToObject(live, "rgb", rgb);
        if (s->active) {
            cJSON_AddNumberToObject(live, "activeMs", now - s->active_since_ms);
        }
        if (c->mode == REGION_STILL && s->have_base) {
            cJSON_AddNumberToObject(live, "stillMs", now - s->moved_ms);
        }
        cJSON_AddItemToArray(arr, o);
    }
    unlock();
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void set_err(char *err, size_t len, const char *fmt, const char *what)
{
    if (err && len) {
        snprintf(err, len, fmt, what);
    }
}

static bool num(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

static uint16_t scale(double v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 1) {
        v = 1;
    }
    return (uint16_t)lround(v * REGION_SCALE);
}

/* One area from JSON into @p c. @p old is the area with the same id before, if any. */
static bool parse_one(const cJSON *o, region_cfg_t *c, const region_cfg_t *old, char *err, size_t len)
{
    const cJSON *name = cJSON_GetObjectItem(o, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        set_err(err, len, "%s", "every area needs a name");
        return false;
    }
    if (strlen(name->valuestring) >= REGION_NAME_MAX) {
        set_err(err, len, "the name \"%.30s\" is too long: 23 characters at most", name->valuestring);
        return false;
    }
    strcpy(c->name, name->valuestring);

    double x, y, w, h;
    if (!num(o, "x", &x) || !num(o, "y", &y) || !num(o, "w", &w) || !num(o, "h", &h)) {
        set_err(err, len, "\"%s\" needs x, y, w and h, as parts of the screen (0..1)", c->name);
        return false;
    }
    c->rect.x = scale(x);
    c->rect.y = scale(y);
    c->rect.w = scale(w);
    c->rect.h = scale(h);
    if (c->rect.x + c->rect.w > REGION_SCALE) {
        c->rect.w = REGION_SCALE - c->rect.x;
    }
    if (c->rect.y + c->rect.h > REGION_SCALE) {
        c->rect.h = REGION_SCALE - c->rect.y;
    }
    if (c->rect.w < 50 || c->rect.h < 50) {
        set_err(err, len, "\"%s\" is too small: at least half a percent of the screen each way", c->name);
        return false;
    }

    const cJSON *mode = cJSON_GetObjectItem(o, "mode");
    const int m = region_mode_parse(cJSON_IsString(mode) ? mode->valuestring : "change");
    if (m < 0) {
        set_err(err, len, "\"%s\": mode is change, still, like, unlike or colour", c->name);
        return false;
    }
    c->mode = (region_mode_t)m;

    double v;
    c->threshold = num(o, "threshold", &v) ? (uint8_t)(v < 1 ? 1 : v > 100 ? 100 : v) : 5;
    c->hold_s = num(o, "hold", &v) ? (uint16_t)(v < 0 ? 0 : v > 86400 ? 86400 : v)
                                   : (c->mode == REGION_STILL ? 30 : 0);
    if (c->mode == REGION_STILL && c->hold_s == 0) {
        c->hold_s = 1;
    }

    const cJSON *col = cJSON_GetObjectItem(o, "colour");
    if (!col) {
        col = cJSON_GetObjectItem(o, "color");
    }
    const int k = region_colour_parse(cJSON_IsString(col) ? col->valuestring : "any");
    if (k < 0) {
        set_err(err, len, "\"%s\": colour is any, black, grey, white, red, orange, yellow, green, "
                          "cyan, blue or magenta", c->name);
        return false;
    }
    c->colour = (region_colour_t)k;
    c->notify = cJSON_IsTrue(cJSON_GetObjectItem(o, "notify"));
    c->clip = cJSON_IsTrue(cJSON_GetObjectItem(o, "clip"));

    /* The remembered picture stays with the area unless asked to go, or the
       area moved: it would be a picture of somewhere else. */
    const cJSON *keep = cJSON_GetObjectItem(o, "hasRef");
    if (old && old->has_ref && !cJSON_IsFalse(keep) && memcmp(&old->rect, &c->rect, sizeof(c->rect)) == 0) {
        c->has_ref = true;
        memcpy(c->ref, old->ref, REGION_CELLS);
    }
    return true;
}

esp_err_t kvm_regions_set_json(const char *json, char *err, size_t err_len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *arr = root ? cJSON_GetObjectItem(root, "regions") : NULL;
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        set_err(err, err_len, "%s", "expected {\"regions\": [...]}");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_GetArraySize(arr) > REGION_MAX) {
        cJSON_Delete(root);
        set_err(err, err_len, "%s", "eight areas at most");
        return ESP_ERR_INVALID_SIZE;
    }

    stored_t *next = calloc(1, sizeof(*next));
    if (!next) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    next->version = CFG_VERSION;
    bool used[REGION_MAX + 1] = {false};
    esp_err_t res = ESP_OK;
    int n = 0;
    lock();
    /* Ids first, so new areas do not take an id an existing one keeps. */
    const cJSON *o;
    cJSON_ArrayForEach(o, arr)
    {
        const cJSON *id = cJSON_GetObjectItem(o, "id");
        if (cJSON_IsNumber(id) && id->valueint >= 1 && id->valueint <= REGION_MAX && !used[id->valueint]) {
            used[id->valueint] = true;
        }
    }
    bool taken[REGION_MAX + 1] = {false};
    cJSON_ArrayForEach(o, arr)
    {
        region_cfg_t *c = &next->cfg[n];
        const cJSON *id = cJSON_GetObjectItem(o, "id");
        int want = cJSON_IsNumber(id) ? id->valueint : 0;
        if (want < 1 || want > REGION_MAX || taken[want]) {
            want = 0;
            for (int k = 1; k <= REGION_MAX && !want; k++) {
                if (!used[k] && !taken[k]) {
                    want = k;
                }
            }
        }
        taken[want] = true;
        c->id = (uint8_t)want;
        const region_cfg_t *old = NULL;
        for (int k = 0; k < REGION_MAX; k++) {
            if (s_store.cfg[k].id == c->id) {
                old = &s_store.cfg[k];
            }
        }
        if (!parse_one(o, c, old, err, err_len)) {
            res = ESP_ERR_INVALID_ARG;
            break;
        }
        for (int k = 0; k < n; k++) {
            if (strcasecmp(next->cfg[k].name, c->name) == 0) {
                set_err(err, err_len, "two areas are called \"%s\"", c->name);
                res = ESP_ERR_INVALID_ARG;
            }
        }
        if (res != ESP_OK) {
            break;
        }
        n++;
    }
    if (res == ESP_OK) {
        /* An area that kept its id, place and mode keeps what it has seen;
           anything else starts again. */
        static region_state_t st[REGION_MAX];
        memset(st, 0, sizeof(st));
        for (int i = 0; i < REGION_MAX; i++) {
            for (int k = 0; k < REGION_MAX; k++) {
                const region_cfg_t *a = &next->cfg[i], *b = &s_store.cfg[k];
                if (a->id && a->id == b->id && a->mode == b->mode &&
                    memcmp(&a->rect, &b->rect, sizeof(a->rect)) == 0) {
                    st[i] = s_st[k];
                }
            }
        }
        memcpy(&s_store, next, sizeof(s_store));
        memcpy(s_st, st, sizeof(s_st));
        memset(s_have_last, 0, sizeof(s_have_last));
        recount();
        s_gen++;
        res = save();
        if (res != ESP_OK) {
            set_err(err, err_len, "could not save: %s", esp_err_to_name(res));
        }
    }
    unlock();
    free(next);
    cJSON_Delete(root);
    return res;
}

esp_err_t kvm_regions_remember(uint8_t id)
{
    esp_err_t res = ESP_ERR_NOT_FOUND;
    lock();
    for (int i = 0; i < REGION_MAX; i++) {
        region_cfg_t *c = &s_store.cfg[i];
        if (c->id && c->id == id) {
            if (!s_have_last[i]) {
                res = ESP_ERR_INVALID_STATE; /* no picture yet */
                break;
            }
            memcpy(c->ref, s_last[i], REGION_CELLS);
            c->has_ref = true;
            region_reset(&s_st[i]);
            res = save();
            break;
        }
    }
    unlock();
    return res;
}

static void status_of(int i, kvm_region_status_t *out)
{
    const region_cfg_t *c = &s_store.cfg[i];
    const region_state_t *s = &s_st[i];
    out->id = c->id;
    strcpy(out->name, c->name);
    out->mode = c->mode;
    out->want_colour = c->colour;
    out->notify = c->notify;
    out->clip = c->clip;
    out->active = s->active;
    out->events = s->events;
    out->diff = s->diff;
    out->colour = s->colour;
}

int kvm_regions_list(kvm_region_status_t *out, int max)
{
    if (!s_mu) {
        return 0;
    }
    int n = 0;
    lock();
    for (int i = 0; i < REGION_MAX && n < max; i++) {
        if (s_store.cfg[i].id) {
            status_of(i, &out[n++]);
        }
    }
    unlock();
    return n;
}

bool kvm_regions_find(const char *name, kvm_region_status_t *out)
{
    if (!s_mu || !name) {
        return false;
    }
    const int id = atoi(name);
    bool found = false;
    lock();
    for (int i = 0; i < REGION_MAX && !found; i++) {
        const region_cfg_t *c = &s_store.cfg[i];
        if (c->id && (strcasecmp(c->name, name) == 0 || (id > 0 && c->id == id))) {
            status_of(i, out);
            found = true;
        }
    }
    unlock();
    return found;
}

void kvm_regions_describe(const kvm_region_status_t *a, char *out, size_t len)
{
    switch (a->mode) {
    case REGION_CHANGE:
        snprintf(out, len, "The picture in \"%s\" changed.", a->name);
        break;
    case REGION_STILL:
        snprintf(out, len, "\"%s\" has stopped changing.", a->name);
        break;
    case REGION_LIKE:
        snprintf(out, len, "\"%s\" looks like the picture it remembers.", a->name);
        break;
    case REGION_UNLIKE:
        snprintf(out, len, "\"%s\" no longer looks like the picture it remembers.", a->name);
        break;
    case REGION_COLOUR:
        snprintf(out, len, a->want_colour == REGION_COL_ANY ? "\"%s\" turned %s." : "\"%s\" is %s.",
                 a->name, region_colour_name(a->colour));
        break;
    default:
        snprintf(out, len, "\"%s\"", a->name);
        break;
    }
}

uint32_t kvm_regions_generation(void)
{
    return s_gen;
}
