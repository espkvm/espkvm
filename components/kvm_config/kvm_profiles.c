/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each profile is one JSON string in NVS namespace "kvm_prof", key "p<id>":
 *   {"name": "Steam Deck", "match": {"cecName": .., "cecVendor": .., "usbOs": ..},
 *    "set": {"mouse_mode": 1}}
 * plus "active" and "manual". Nothing is written there until the first profile
 * is made, and an older firmware never reads it. Two keys there are for the
 * console, not for a profile: "welcome" (the first password is set, ask which
 * devices it will see), "tour_ver" (the firmware version the console's
 * overview of features was last closed on) and "declined" (presets not to
 * offer again). "tour_ver" is absent on a device updated from before it, so
 * the overview shows there once too.
 */
#include "kvm_profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kvm_settings.h"
#include "nvs.h"
#include "settings_priv.h"

static const char *TAG = "kvm_prof";

#define PROF_NS "kvm_prof"
#define NAME_LEN 31

typedef struct {
    bool used;
    char name[NAME_LEN + 1];
    char cec_name[16];
    char cec_vendor[8];
    char usb_os[12];
    cJSON *set; /* own values as stored, an object */
} profile_t;

typedef struct {
    const char *id;
    const char *name;
    const char *cec_name;
    const char *cec_vendor;
    const char *usb_os;
    const char *set; /* JSON; enums by choice name */
} preset_t;

/* Console buttons a games console or a TV box has no use for (ids from
   web/src/ui/features.ts). A profile's own list also brings back what Default
   hides - the remote and the gamepad on a computer-only setup. */
#define UI_NO_COMPUTER "\"copy,netlog,select,serial\""

/* The CEC names and vendor ids are what web/src/state/targetKind.ts knows.
   "declined" keeps a bit per entry, by place: add new ones at the end. */
static const preset_t k_presets[] = {
    {"steamdeck", "Steam Deck", "Steam Deck", "e0319e", "",
     "{\"target_os\":\"linux\",\"mouse_mode\":\"relative\",\"ptr_desk_w\":0,\"ptr_desk_h\":0,"
     "\"ui_hidden\":\"netlog,serial\"}"},
    {"switch", "Nintendo Switch", "Nintendo", "0009bf", "",
     /* switch_alone shows it a gamepad only: no keys, no mouse to send. */
     "{\"usb_pad\":\"switch_alone\","
     "\"ui_hidden\":\"copy,keyboard,keylock,netlog,padmouse,select,serial\"}"},
    {"androidtv", "Android TV", "", "", "android",
     "{\"target_os\":\"android\",\"mouse_mode\":\"relative\",\"ui_hidden\":" UI_NO_COMPUTER "}"},
    /* A stick has no USB to us, only CEC: its micro-USB is power. So the
       remote is all there is. By name: Xiaomi's vendor id is on other things
       too. */
    {"mitvstick", "Mi TV Stick", "Mi TV Stick", "", "",
     "{\"target_os\":\"android\",\"mouse_mode\":\"relative\",\"ui_hidden\":"
     "\"copy,gamepad,keyboard,keylock,netlog,padmouse,select,serial,touch\"}"},
};

static profile_t s_p[KVM_PROFILES_MAX + 1] = {
    [KVM_PROFILE_DEFAULT] = {.used = true, .name = "Default"}, /* a name only */
};
static uint8_t s_active;
static bool s_manual;
static SemaphoreHandle_t s_mu;
/* What is attached now, as profile_auto.c last saw it. */
static char s_seen_name[16], s_seen_vendor[8], s_seen_os[12];
#define FP_LEN 48
static char s_seen_fp[FP_LEN];
static bool s_welcome;
#define TOUR_VER_LEN 24
static char s_tour_ver[TOUR_VER_LEN];
static uint8_t s_declined;
#define N_PRESETS (sizeof(k_presets) / sizeof(k_presets[0]))

static void lock(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

static void copy_str(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    strlcpy(dst, cJSON_IsString(it) ? it->valuestring : "", cap);
}

static void read_match(profile_t *p, const cJSON *m)
{
    copy_str(p->cec_name, sizeof(p->cec_name), m, "cecName");
    copy_str(p->cec_vendor, sizeof(p->cec_vendor), m, "cecVendor");
    copy_str(p->usb_os, sizeof(p->usb_os), m, "usbOs");
}

static cJSON *match_json(const char *cec_name, const char *cec_vendor, const char *usb_os)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "cecName", cec_name);
    cJSON_AddStringToObject(m, "cecVendor", cec_vendor);
    cJSON_AddStringToObject(m, "usbOs", usb_os);
    return m;
}

static cJSON *profile_json(uint8_t id, const profile_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", p->name);
    if (id != KVM_PROFILE_DEFAULT) {
        cJSON_AddItemToObject(o, "match", match_json(p->cec_name, p->cec_vendor, p->usb_os));
        cJSON_AddItemToObject(o, "set", cJSON_Duplicate(p->set, true));
    }
    return o;
}

static esp_err_t nvs_write(void (*fn)(nvs_handle_t, void *), void *arg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PROF_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    fn(nvs, arg);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void write_profile(nvs_handle_t nvs, void *arg)
{
    const uint8_t id = *(const uint8_t *)arg;
    char key[5];
    snprintf(key, sizeof(key), "p%u", id);
    if (!s_p[id].used) {
        (void)nvs_erase_key(nvs, key);
        return;
    }
    cJSON *o = profile_json(id, &s_p[id]);
    cJSON_DeleteItemFromObject(o, "id");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        esp_err_t err = nvs_set_str(nvs, key, s);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save %s: %s", key, esp_err_to_name(err));
        }
        free(s);
    }
}

static void write_active(nvs_handle_t nvs, void *arg)
{
    (void)arg;
    (void)nvs_set_u8(nvs, "active", s_active);
    (void)nvs_set_u8(nvs, "manual", s_manual ? 1 : 0);
}

#define SEEN_KEY "seen"

static void write_seen(nvs_handle_t nvs, void *arg)
{
    (void)nvs_set_str(nvs, SEEN_KEY, (const char *)arg);
}

/* A choice by hand belongs to the device attached now: auto-selection leaves
 * it alone until another device comes. Called with the lock held. */
static void write_asks(nvs_handle_t nvs, void *arg)
{
    (void)arg;
    (void)nvs_set_u8(nvs, "welcome", s_welcome ? 1 : 0);
    (void)nvs_set_str(nvs, "tour_ver", s_tour_ver);
    (void)nvs_set_u8(nvs, "declined", s_declined);
}

static void hold_for_this_device(void)
{
    if (s_seen_fp[0]) {
        (void)nvs_write(write_seen, s_seen_fp);
    }
}

static esp_err_t save_profile(uint8_t id)
{
    return nvs_write(write_profile, &id);
}

static esp_err_t save_active(void)
{
    return nvs_write(write_active, NULL);
}

static void erase_all(nvs_handle_t nvs, void *arg)
{
    (void)arg;
    (void)nvs_erase_all(nvs);
}

/*
 * Put profile @p id over the settings. Called with the lock held; the
 * subscribers are told after it is released, by the caller, through
 * finish_switch().
 */
static bool *switch_to(uint8_t id, bool *reboot)
{
    bool *changed = calloc(settings_count(), sizeof(bool));
    if (!changed) {
        return NULL;
    }
    s_active = id;
    *reboot = settings_overlay_load(id == KVM_PROFILE_DEFAULT ? NULL : s_p[id].set, changed);
    return changed;
}

static void finish_switch(bool *changed)
{
    if (changed) {
        settings_overlay_notify(changed);
        free(changed);
    }
}

void profiles_init(void)
{
    s_mu = xSemaphoreCreateMutex();

    nvs_handle_t nvs;
    if (nvs_open(PROF_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return; /* never made a profile */
    }
    for (uint8_t id = 1; id <= KVM_PROFILES_MAX; id++) {
        char key[5];
        snprintf(key, sizeof(key), "p%u", id);
        size_t len = 0;
        if (nvs_get_str(nvs, key, NULL, &len) != ESP_OK || len == 0) {
            continue;
        }
        char *s = malloc(len);
        if (!s || nvs_get_str(nvs, key, s, &len) != ESP_OK) {
            free(s);
            continue;
        }
        cJSON *o = cJSON_Parse(s);
        free(s);
        if (!cJSON_IsObject(o)) {
            ESP_LOGW(TAG, "%s is not a profile; skipped", key);
            cJSON_Delete(o);
            continue;
        }
        profile_t *p = &s_p[id];
        copy_str(p->name, sizeof(p->name), o, "name");
        read_match(p, cJSON_GetObjectItemCaseSensitive(o, "match"));
        cJSON *set = cJSON_DetachItemFromObjectCaseSensitive(o, "set");
        p->set = cJSON_IsObject(set) ? set : cJSON_CreateObject();
        if (p->set != set) {
            cJSON_Delete(set);
        }
        p->used = true;
        cJSON_Delete(o);
    }
    uint8_t active = 0, manual = 0, welcome = 0;
    (void)nvs_get_u8(nvs, "active", &active);
    (void)nvs_get_u8(nvs, "manual", &manual);
    (void)nvs_get_u8(nvs, "welcome", &welcome);
    size_t tour_len = sizeof(s_tour_ver);
    if (nvs_get_str(nvs, "tour_ver", s_tour_ver, &tour_len) != ESP_OK) {
        s_tour_ver[0] = '\0';
    }
    (void)nvs_get_u8(nvs, "declined", &s_declined);
    nvs_close(nvs);
    s_welcome = welcome != 0;

    if (active <= KVM_PROFILES_MAX && s_p[active].used && active != KVM_PROFILE_DEFAULT) {
        bool reboot;
        /* No subscribers yet: everyone reads the values as they start. */
        free(switch_to(active, &reboot));
        s_manual = manual != 0;
        ESP_LOGI(TAG, "profile %u \"%s\"%s", active, s_p[active].name, s_manual ? " (by hand)" : "");
    }
}

bool profiles_custom_active(void)
{
    return s_active != KVM_PROFILE_DEFAULT;
}

esp_err_t profiles_store(const char *key, const cJSON *value)
{
    lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    profile_t *p = &s_p[s_active];
    if (s_active != KVM_PROFILE_DEFAULT && p->set) {
        cJSON_DeleteItemFromObjectCaseSensitive(p->set, key);
        cJSON_AddItemToObject(p->set, key, cJSON_Duplicate(value, true));
        err = save_profile(s_active);
    }
    unlock();
    return err;
}

void profiles_reset(void)
{
    lock();
    for (uint8_t id = 1; id <= KVM_PROFILES_MAX; id++) {
        cJSON_Delete(s_p[id].set);
        memset(&s_p[id], 0, sizeof(s_p[id]));
    }
    s_active = KVM_PROFILE_DEFAULT;
    s_manual = false;
    s_welcome = false;
    s_tour_ver[0] = '\0';
    s_declined = 0;
    (void)nvs_write(erase_all, NULL);
    unlock();
}

uint8_t kvm_profiles_active(void)
{
    return s_active;
}

bool kvm_profiles_manual(void)
{
    return s_manual;
}

static const preset_t *find_preset(const char *id)
{
    for (size_t i = 0; i < N_PRESETS; i++) {
        if (strcmp(k_presets[i].id, id) == 0) {
            return &k_presets[i];
        }
    }
    return NULL;
}

/**
 * Merge @p src into @p dst after checking every value; nothing changes on an
 * error. A null value drops the key.
 */
static esp_err_t merge_set(cJSON *dst, const cJSON *src, char *err, size_t err_len)
{
    if (!cJSON_IsObject(src)) {
        snprintf(err, err_len, "\"set\" must be an object");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *checked = cJSON_CreateObject();
    for (const cJSON *it = src->child; it; it = it->next) {
        if (cJSON_IsNull(it)) {
            continue;
        }
        cJSON *v = settings_profile_value(it->string, it, err, err_len);
        if (!v) {
            cJSON_Delete(checked);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(checked, it->string, v);
    }
    for (const cJSON *it = src->child; it; it = it->next) {
        cJSON_DeleteItemFromObjectCaseSensitive(dst, it->string);
    }
    while (checked->child) {
        cJSON *v = cJSON_DetachItemViaPointer(checked, checked->child);
        cJSON_AddItemToObject(dst, v->string, v);
    }
    cJSON_Delete(checked);
    return ESP_OK;
}

/*
 * A preset to offer for what is attached now: the device names itself over
 * CEC, no profile of one's own fits it, and the operator has not said no to
 * that preset. The USB guess alone is not enough - "android" is a phone as
 * often as a TV box. Under the lock.
 */
static const preset_t *suggestion(void)
{
    const kvm_prof_seen_t seen = {s_seen_name, s_seen_vendor, s_seen_os};
    if (!seen.cec_name[0] && !seen.cec_vendor[0]) {
        return NULL;
    }
    if (s_manual && s_active != KVM_PROFILE_DEFAULT) {
        return NULL; /* picked by hand for this device already */
    }
    for (uint8_t id = 1; id <= KVM_PROFILES_MAX; id++) {
        const kvm_prof_rule_t r = {s_p[id].cec_name, s_p[id].cec_vendor, s_p[id].usb_os};
        if (s_p[id].used && kvm_prof_rule_score(&r, &seen) >= 0) {
            return NULL;
        }
    }
    for (size_t i = 0; i < N_PRESETS; i++) {
        const preset_t *k = &k_presets[i];
        const kvm_prof_rule_t r = {k->cec_name, k->cec_vendor, k->usb_os};
        if (!(s_declined & (1u << i)) && kvm_prof_rule_score(&r, &seen) >= 2) {
            return k;
        }
    }
    return NULL;
}

char *kvm_profiles_json(void)
{
    cJSON *root = cJSON_CreateObject();
    lock();
    cJSON_AddNumberToObject(root, "active", s_active);
    cJSON_AddBoolToObject(root, "manual", s_manual);
    cJSON_AddNumberToObject(root, "max", KVM_PROFILES_MAX);
    cJSON_AddItemToObject(root, "seen", match_json(s_seen_name, s_seen_vendor, s_seen_os));
    cJSON_AddBoolToObject(root, "welcome", s_welcome);
    cJSON_AddStringToObject(root, "tourSeen", s_tour_ver);
    const preset_t *sug = suggestion();
    if (sug) {
        cJSON_AddStringToObject(root, "suggest", sug->id);
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    for (uint8_t id = 0; id <= KVM_PROFILES_MAX; id++) {
        if (s_p[id].used) {
            cJSON_AddItemToArray(arr, profile_json(id, &s_p[id]));
        }
    }
    unlock();
    cJSON *pre = cJSON_AddArrayToObject(root, "presets");
    for (size_t i = 0; i < N_PRESETS; i++) {
        const preset_t *k = &k_presets[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", k->id);
        cJSON_AddStringToObject(o, "name", k->name);
        cJSON_AddItemToObject(o, "match", match_json(k->cec_name, k->cec_vendor, k->usb_os));
        cJSON *set = cJSON_CreateObject();
        cJSON *src = cJSON_Parse(k->set);
        char err[64];
        if (src && merge_set(set, src, err, sizeof(err)) != ESP_OK) {
            ESP_LOGE(TAG, "preset %s: %s", k->id, err);
        }
        cJSON_Delete(src);
        cJSON_AddItemToObject(o, "set", set);
        cJSON_AddItemToArray(pre, o);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static esp_err_t parse_body(const char *json, cJSON **out, char *err, size_t err_len)
{
    *out = json ? cJSON_Parse(json) : NULL;
    if (!cJSON_IsObject(*out)) {
        cJSON_Delete(*out);
        *out = NULL;
        snprintf(err, err_len, "body must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t take_name(profile_t *p, const cJSON *body, char *err, size_t err_len)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(body, "name");
    if (!n) {
        return ESP_OK;
    }
    if (!cJSON_IsString(n) || !n->valuestring[0] || strlen(n->valuestring) > NAME_LEN) {
        snprintf(err, err_len, "\"name\" must be 1 to %d characters", NAME_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(p->name, n->valuestring, sizeof(p->name));
    return ESP_OK;
}

esp_err_t kvm_profiles_create(const char *json, uint8_t *id, bool *reboot, char *err, size_t err_len)
{
    *reboot = false;
    cJSON *body;
    esp_err_t res = parse_body(json, &body, err, err_len);
    if (res != ESP_OK) {
        return res;
    }
    const cJSON *from = cJSON_GetObjectItemCaseSensitive(body, "from");
    const char *from_s = cJSON_IsString(from) ? from->valuestring : "current";
    const preset_t *preset = NULL;
    if (strcmp(from_s, "current") != 0 && strcmp(from_s, "empty") != 0) {
        preset = find_preset(from_s);
        if (!preset) {
            snprintf(err, err_len, "no preset '%s'", from_s);
            cJSON_Delete(body);
            return ESP_ERR_NOT_FOUND;
        }
    }

    profile_t p = {.used = true, .set = cJSON_CreateObject()};
    strlcpy(p.name, preset ? preset->name : "New profile", sizeof(p.name));
    if (preset) {
        strlcpy(p.cec_name, preset->cec_name, sizeof(p.cec_name));
        strlcpy(p.cec_vendor, preset->cec_vendor, sizeof(p.cec_vendor));
        strlcpy(p.usb_os, preset->usb_os, sizeof(p.usb_os));
        cJSON *src = cJSON_Parse(preset->set);
        res = merge_set(p.set, src, err, err_len);
        cJSON_Delete(src);
    }
    if (res == ESP_OK) {
        res = take_name(&p, body, err, err_len);
    }
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "match");
    if (res == ESP_OK && m) {
        read_match(&p, m);
    }
    /* Values of its own on top of where it started (an import uses this). */
    const cJSON *own = cJSON_GetObjectItemCaseSensitive(body, "set");
    cJSON *extra = cJSON_CreateObject();
    if (res == ESP_OK && own) {
        res = merge_set(extra, own, err, err_len);
    }
    const cJSON *sel = cJSON_GetObjectItemCaseSensitive(body, "select");
    const bool select = !cJSON_IsFalse(sel);
    cJSON_Delete(body);
    if (res != ESP_OK) {
        cJSON_Delete(p.set);
        cJSON_Delete(extra);
        return res;
    }

    lock();
    uint8_t free_id = 0;
    for (uint8_t i = 1; i <= KVM_PROFILES_MAX && !free_id; i++) {
        if (!s_p[i].used) {
            free_id = i;
        }
    }
    if (!free_id) {
        unlock();
        cJSON_Delete(p.set);
        cJSON_Delete(extra);
        snprintf(err, err_len, "there are already %d profiles", KVM_PROFILES_MAX);
        return ESP_ERR_NO_MEM;
    }
    if (strcmp(from_s, "current") == 0 && s_active != KVM_PROFILE_DEFAULT) {
        cJSON_Delete(p.set);
        p.set = cJSON_Duplicate(s_p[s_active].set, true);
    }
    while (extra->child) {
        cJSON *v = cJSON_DetachItemViaPointer(extra, extra->child);
        cJSON_DeleteItemFromObjectCaseSensitive(p.set, v->string);
        cJSON_AddItemToObject(p.set, v->string, v);
    }
    cJSON_Delete(extra);
    s_p[free_id] = p;
    res = save_profile(free_id);
    bool *changed = NULL;
    if (select) {
        changed = switch_to(free_id, reboot);
        s_manual = true;
        (void)save_active();
        hold_for_this_device();
    }
    unlock();
    finish_switch(changed);
    *id = free_id;
    ESP_LOGI(TAG, "made profile %u \"%s\"", free_id, p.name);
    return res;
}

esp_err_t kvm_profiles_update(const char *json, bool *reboot, char *err, size_t err_len)
{
    *reboot = false;
    cJSON *body;
    esp_err_t res = parse_body(json, &body, err, err_len);
    if (res != ESP_OK) {
        return res;
    }
    const cJSON *idj = cJSON_GetObjectItemCaseSensitive(body, "id");
    const int id = cJSON_IsNumber(idj) ? idj->valueint : -1;
    if (id == KVM_PROFILE_DEFAULT) {
        cJSON_Delete(body);
        snprintf(err, err_len, "Default is changed through /api/v1/settings");
        return ESP_ERR_NOT_ALLOWED;
    }

    lock();
    if (id < 1 || id > KVM_PROFILES_MAX || !s_p[id].used) {
        unlock();
        cJSON_Delete(body);
        snprintf(err, err_len, "no profile %d", id);
        return ESP_ERR_NOT_FOUND;
    }
    profile_t next = s_p[id];
    next.set = cJSON_Duplicate(s_p[id].set, true);
    res = take_name(&next, body, err, err_len);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "match");
    if (res == ESP_OK && m) {
        read_match(&next, m);
    }
    const cJSON *set = cJSON_GetObjectItemCaseSensitive(body, "set");
    if (res == ESP_OK && set) {
        res = merge_set(next.set, set, err, err_len);
    }
    const cJSON *unset = cJSON_GetObjectItemCaseSensitive(body, "unset");
    if (res == ESP_OK && unset) {
        if (!cJSON_IsArray(unset)) {
            snprintf(err, err_len, "\"unset\" must be a list of keys");
            res = ESP_ERR_INVALID_ARG;
        }
        const cJSON *k;
        cJSON_ArrayForEach(k, unset)
        {
            if (cJSON_IsString(k)) {
                cJSON_DeleteItemFromObjectCaseSensitive(next.set, k->valuestring);
            }
        }
    }
    cJSON_Delete(body);
    if (res != ESP_OK) {
        unlock();
        cJSON_Delete(next.set);
        return res;
    }
    cJSON_Delete(s_p[id].set);
    s_p[id] = next;
    res = save_profile((uint8_t)id);
    bool *changed = id == s_active ? switch_to((uint8_t)id, reboot) : NULL;
    unlock();
    finish_switch(changed);
    return res;
}

esp_err_t kvm_profiles_delete(uint8_t id, bool *reboot)
{
    *reboot = false;
    if (id == KVM_PROFILE_DEFAULT) {
        return ESP_ERR_NOT_ALLOWED;
    }
    lock();
    if (id > KVM_PROFILES_MAX || !s_p[id].used) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    bool *changed = NULL;
    if (id == s_active) {
        changed = switch_to(KVM_PROFILE_DEFAULT, reboot);
        s_manual = false;
        (void)save_active();
    }
    cJSON_Delete(s_p[id].set);
    memset(&s_p[id], 0, sizeof(s_p[id]));
    esp_err_t res = save_profile(id);
    unlock();
    finish_switch(changed);
    return res;
}

esp_err_t kvm_profiles_select(uint8_t id, bool manual, bool *reboot)
{
    *reboot = false;
    lock();
    if (id > KVM_PROFILES_MAX || !s_p[id].used) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    bool *changed = NULL;
    if (id != s_active || manual != s_manual) {
        if (id != s_active) {
            changed = switch_to(id, reboot);
        }
        s_manual = manual;
        (void)save_active();
        if (manual) {
            hold_for_this_device();
        }
        ESP_LOGI(TAG, "profile %u \"%s\" (%s)", id, s_p[id].name, manual ? "by hand" : "auto");
    }
    unlock();
    finish_switch(changed);
    return ESP_OK;
}

uint8_t kvm_profiles_pick(const kvm_prof_seen_t *seen)
{
    kvm_prof_rule_t rules[KVM_PROFILES_MAX];
    uint8_t ids[KVM_PROFILES_MAX];
    size_t n = 0;
    lock();
    for (uint8_t id = 1; id <= KVM_PROFILES_MAX; id++) {
        if (s_p[id].used) {
            rules[n] = (kvm_prof_rule_t){s_p[id].cec_name, s_p[id].cec_vendor, s_p[id].usb_os};
            ids[n++] = id;
        }
    }
    const int best = kvm_prof_pick(rules, n, seen);
    unlock();
    return best < 0 ? KVM_PROFILE_DEFAULT : ids[best];
}

void kvm_profiles_welcome(void)
{
    lock();
    s_welcome = true;
    (void)nvs_write(write_asks, NULL);
    unlock();
}

esp_err_t kvm_profiles_dismiss(const char *json, char *err, size_t err_len)
{
    cJSON *body;
    esp_err_t res = parse_body(json, &body, err, err_len);
    if (res != ESP_OK) {
        return res;
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(body, "welcome");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(body, "tour");
    const cJSON *pre = cJSON_GetObjectItemCaseSensitive(body, "preset");
    const preset_t *k = cJSON_IsString(pre) ? find_preset(pre->valuestring) : NULL;
    if (pre && !k) {
        snprintf(err, err_len, "no such preset");
        cJSON_Delete(body);
        return ESP_ERR_INVALID_ARG;
    }
    if (t && (!cJSON_IsString(t) || strlen(t->valuestring) >= TOUR_VER_LEN)) {
        snprintf(err, err_len, "tour: a version string");
        cJSON_Delete(body);
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (cJSON_IsTrue(w)) {
        s_welcome = false;
    }
    if (t) {
        strlcpy(s_tour_ver, t->valuestring, sizeof(s_tour_ver));
    }
    if (k) {
        s_declined |= (uint8_t)(1u << (k - k_presets));
    }
    res = nvs_write(write_asks, NULL);
    unlock();
    cJSON_Delete(body);
    return res;
}

bool kvm_profiles_any(void)
{
    for (uint8_t id = 1; id <= KVM_PROFILES_MAX; id++) {
        if (s_p[id].used) {
            return true;
        }
    }
    return false;
}

void kvm_profiles_note_seen(const kvm_prof_seen_t *seen, const char *fp)
{
    lock();
    strlcpy(s_seen_fp, fp, sizeof(s_seen_fp));
    strlcpy(s_seen_name, seen->cec_name, sizeof(s_seen_name));
    strlcpy(s_seen_vendor, seen->cec_vendor, sizeof(s_seen_vendor));
    strlcpy(s_seen_os, seen->usb_os, sizeof(s_seen_os));
    unlock();
}


/* The CEC part of a fingerprint ("name|vendor"), or "" when CEC said nothing.
 * Unlike the USB part, a restart does not change it. */
static void cec_part(const char *fp, char *out, size_t len)
{
    const char *bar = strrchr(fp, '|');
    size_t n = bar ? (size_t)(bar - fp) : 0;
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, fp, n);
    out[n] = '\0';
    if (strcmp(out, "|") == 0) {
        out[0] = '\0';
    }
}

#define RESTART_CEC_KEY "rst_cec"

static void write_restart_cec(nvs_handle_t nvs, void *arg)
{
    (void)nvs_set_str(nvs, RESTART_CEC_KEY, (const char *)arg);
}

static void write_restarts(nvs_handle_t nvs, void *arg);

/* Another source over CEC than the one the last restarts were for: a person
 * swapped them, not two profiles undoing each other, so the count starts again. */
static void restarts_new_source(const char *fp)
{
    char cec[FP_LEN], was[FP_LEN] = "";
    cec_part(fp, cec, sizeof(cec));
    if (!cec[0]) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(PROF_NS, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(was);
        (void)nvs_get_str(nvs, RESTART_CEC_KEY, was, &len);
        nvs_close(nvs);
    }
    if (was[0] && strcmp(was, cec) != 0) {
        uint8_t n = 0;
        (void)nvs_write(write_restarts, &n);
    }
}

bool kvm_profiles_device_settled(const kvm_prof_seen_t *seen, const char *fp)
{
    restarts_new_source(fp);
    char last[FP_LEN] = "";
    nvs_handle_t nvs;
    if (nvs_open(PROF_NS, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(last);
        (void)nvs_get_str(nvs, SEEN_KEY, last, &len);
        nvs_close(nvs);
    }
    const bool same = strcmp(last, fp) == 0;
    if (same && s_manual) {
        return false; /* the same device as before: a choice by hand holds */
    }
    if (!same) {
        (void)nvs_write(write_seen, (void *)fp);
    }

    /* The same device with a choice made here: the rules may have changed
       since (a profile made for it), so they are asked again. */
    const uint8_t id = kvm_profiles_pick(seen);
    const uint8_t was = s_active;
    if (same && id == was) {
        return false;
    }
    bool reboot;
    ESP_LOGI(TAG, "%s device \"%s\": profile %u", same ? "same" : "new", fp, id);
    (void)kvm_profiles_select(id, false, &reboot);
    return id != was;
}

static void write_restarts(nvs_handle_t nvs, void *arg)
{
    (void)nvs_set_u8(nvs, "restarts", *(const uint8_t *)arg);
}

static uint8_t read_restarts(void)
{
    uint8_t n = 0;
    nvs_handle_t nvs;
    if (nvs_open(PROF_NS, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, "restarts", &n);
        nvs_close(nvs);
    }
    return n;
}

bool kvm_profiles_restart_allowed(void)
{
    uint8_t n = read_restarts();
    if (n >= 2) {
        ESP_LOGW(TAG, "restarted for a profile %u times in a row; not again until it settles", n);
        return false;
    }
    n++;
    (void)nvs_write(write_restarts, &n);
    char cec[FP_LEN];
    lock();
    cec_part(s_seen_fp, cec, sizeof(cec));
    unlock();
    (void)nvs_write(write_restart_cec, cec);
    return true;
}

void kvm_profiles_uptime_ok(void)
{
    if (read_restarts() != 0) {
        uint8_t n = 0;
        (void)nvs_write(write_restarts, &n);
    }
}
