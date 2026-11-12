/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_settings.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kvm_board_header.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pin_conflict.h"
#include "settings_priv.h"

static const char *TAG = "kvm_cfg";

#define NVS_NAMESPACE "kvm"
#define MAX_SUBSCRIBERS 8

typedef struct {
    int32_t i;
    /* Strings live in a buffer owned for the lifetime of the process, so a
     * reader never holds a pointer that a concurrent write could free. */
    char *s;
} value_t;

typedef struct {
    kvm_settings_cb_t cb;
    void *user;
} subscriber_t;

static const kvm_setting_t *s_table;
static size_t s_count;
static value_t *s_values;
/* The active profile's own values, over s_values where s_over_set says so.
 * Empty while Default is active. Strings get a buffer only for the keys a
 * profile may hold. */
static value_t *s_over;
static bool *s_over_set;
static SemaphoreHandle_t s_mu;
static subscriber_t s_subs[MAX_SUBSCRIBERS];
static uint8_t s_sub_count;
/* This install's random id, read or made on first use; a reset forgets it. */
static char s_install_id[KVM_INSTALL_ID_LEN + 1];

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

static int find_index(const char *key)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_table[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_numeric(const kvm_setting_t *d)
{
    return d->type != KVM_VT_STR;
}

static int32_t numeric_min(const kvm_setting_t *d)
{
    return d->type == KVM_VT_BOOL ? 0 : d->min;
}

static int32_t numeric_max(const kvm_setting_t *d)
{
    return d->type == KVM_VT_BOOL ? 1 : d->max;
}

/** The value in effect. Call under the lock. */
static const value_t *eff(size_t idx)
{
    return s_over_set[idx] ? &s_over[idx] : &s_values[idx];
}

static void notify(const char *key)
{
    for (uint8_t i = 0; i < s_sub_count; i++) {
        s_subs[i].cb(key, s_subs[i].user);
    }
}

static void load_one(nvs_handle_t nvs, size_t idx)
{
    const kvm_setting_t *d = &s_table[idx];
    value_t *v = &s_values[idx];

    if (is_numeric(d)) {
        int32_t stored = 0;
        if (nvs_get_i32(nvs, d->key, &stored) == ESP_OK && stored >= numeric_min(d) &&
            stored <= numeric_max(d)) {
            v->i = stored;
        } else {
            v->i = d->def;
        }
        return;
    }

    const size_t cap = (size_t)d->max_len + 1u;
    size_t len = cap;
    if (nvs_get_str(nvs, d->key, v->s, &len) != ESP_OK) {
        const char *def = d->def_str ? d->def_str : "";
        strlcpy(v->s, def, cap);
    }
}

/*
 * Releases moved from espkvm.github.io/espkvm/ to fw.espkvm.io (2026-10). A
 * device that never touched the update address follows the new default by
 * itself, but one whose settings were loaded from a file has the old default
 * stored - which works only while GitHub keeps redirecting it. Such a value is
 * dropped so the default applies again; an address of the operator's own (a
 * fork, a mirror) does not start with the old one and stays.
 */
static void migrate_update_url(nvs_handle_t nvs)
{
    static const char old_prefix[] = "https://espkvm.github.io/espkvm/";
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_table[i].key, "upd_url") != 0 || is_numeric(&s_table[i])) {
            continue;
        }
        if (strncmp(s_values[i].s, old_prefix, sizeof(old_prefix) - 1) == 0) {
            if (nvs_erase_key(nvs, "upd_url") == ESP_OK) {
                (void)nvs_commit(nvs);
            }
            const char *def = s_table[i].def_str ? s_table[i].def_str : "";
            strlcpy(s_values[i].s, def, (size_t)s_table[i].max_len + 1u);
            ESP_LOGI(TAG, "update address moved to %s", def);
        }
    }
}

/*
 * Notifications used to have three switches for what to send (a watched
 * phrase, a blank screen, a dashcam clip) and sent it everywhere. Now each
 * channel has a mask of what it takes. A device that had those switches
 * stored gets the same choice in every channel, once, and the old keys go.
 */
static void migrate_notify_routes(nvs_handle_t nvs)
{
    static const struct {
        const char *key;
        int bit;
    } old[] = {{"notify_watch", 0}, {"notify_flat", 1}, {"notify_clip", 3}};
    static const char *const masks[] = {"notify_tg_ev", "notify_hook_ev", "notify_push_ev"};
    int32_t on = 0, off = 0;
    bool any = false;
    for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
        int32_t v;
        if (nvs_get_i32(nvs, old[i].key, &v) == ESP_OK) {
            any = true;
            if (v) {
                on |= 1 << old[i].bit;
            } else {
                off |= 1 << old[i].bit;
            }
            (void)nvs_erase_key(nvs, old[i].key);
        }
    }
    if (!any) {
        return;
    }
    for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
        const int idx = find_index(masks[m]);
        if (idx < 0) {
            continue;
        }
        const int32_t v = (s_values[idx].i | on) & ~off;
        if (v != s_values[idx].i && nvs_set_i32(nvs, masks[m], v) == ESP_OK) {
            s_values[idx].i = v;
        }
    }
    (void)nvs_commit(nvs);
    ESP_LOGI(TAG, "notifications: the old switches moved into the per-channel table");
}

/*
 * "New firmware" (bit 6) joined the notification table in 0.82.0, ticked for
 * every channel. A device that had saved the table has its old mask stored,
 * which would leave the new row unticked where a fresh device has it ticked.
 * So, once: a channel that takes anything at all takes this too. A channel
 * with every box cleared stays cleared.
 */
static void migrate_notify_release(nvs_handle_t nvs)
{
    uint8_t done;
    if (nvs_get_u8(nvs, "nev_release", &done) == ESP_OK) {
        return;
    }
    static const char *const masks[] = {"notify_tg_ev", "notify_hook_ev", "notify_push_ev"};
    for (size_t m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
        const int idx = find_index(masks[m]);
        int32_t stored;
        if (idx < 0 || nvs_get_i32(nvs, masks[m], &stored) != ESP_OK || stored == 0) {
            continue;
        }
        const int32_t v = stored | (1 << 6);
        if (v != stored && nvs_set_i32(nvs, masks[m], v) == ESP_OK) {
            s_values[idx].i = v;
            ESP_LOGI(TAG, "notifications: New firmware ticked for %s", masks[m]);
        }
    }
    (void)nvs_set_u8(nvs, "nev_release", 1);
    (void)nvs_commit(nvs);
}

/*
 * The picture page had its own switch, disp_picture. It is bit 3 of the page
 * set now; a device that had it on gets that bit, once, and the old key goes.
 */
static void migrate_disp_picture(nvs_handle_t nvs)
{
    int32_t on;
    if (nvs_get_i32(nvs, "disp_picture", &on) != ESP_OK) {
        return;
    }
    const int idx = find_index("disp_pages");
    if (on && idx >= 0 && nvs_set_i32(nvs, "disp_pages", s_values[idx].i | (1 << 3)) == ESP_OK) {
        s_values[idx].i |= 1 << 3;
    }
    (void)nvs_erase_key(nvs, "disp_picture");
    (void)nvs_commit(nvs);
    ESP_LOGI(TAG, "display: the picture switch moved into the page set");
}

esp_err_t kvm_settings_init(void)
{
    if (s_values) {
        return ESP_OK;
    }
    s_table = kvm_settings_table(&s_count);
    ESP_RETURN_ON_FALSE(s_table && s_count, ESP_ERR_INVALID_STATE, TAG, "empty settings table");

    s_mu = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mu, ESP_ERR_NO_MEM, TAG, "mutex");

    s_values = calloc(s_count, sizeof(value_t));
    s_over = calloc(s_count, sizeof(value_t));
    s_over_set = calloc(s_count, sizeof(bool));
    ESP_RETURN_ON_FALSE(s_values && s_over && s_over_set, ESP_ERR_NO_MEM, TAG, "values");

    for (size_t i = 0; i < s_count; i++) {
        if (!is_numeric(&s_table[i])) {
            s_values[i].s = calloc(1, (size_t)s_table[i].max_len + 1u);
            ESP_RETURN_ON_FALSE(s_values[i].s, ESP_ERR_NO_MEM, TAG, "string %s", s_table[i].key);
            if (s_table[i].flags & KVM_SF_PROFILE) {
                s_over[i].s = calloc(1, (size_t)s_table[i].max_len + 1u);
                ESP_RETURN_ON_FALSE(s_over[i].s, ESP_ERR_NO_MEM, TAG, "string %s", s_table[i].key);
            }
        }
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s - using defaults", esp_err_to_name(err));
        for (size_t i = 0; i < s_count; i++) {
            const kvm_setting_t *d = &s_table[i];
            if (is_numeric(d)) {
                s_values[i].i = d->def;
            } else {
                strlcpy(s_values[i].s, d->def_str ? d->def_str : "", (size_t)d->max_len + 1u);
            }
        }
        return ESP_OK;
    }

    for (size_t i = 0; i < s_count; i++) {
        load_one(nvs, i);
    }
    migrate_update_url(nvs);
    migrate_notify_routes(nvs);
    migrate_notify_release(nvs);
    migrate_disp_picture(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "%u settings loaded", (unsigned)s_count);
    profiles_init();
    return ESP_OK;
}

const kvm_setting_t *kvm_setting_find(const char *key)
{
    if (!key || !s_table) {
        return NULL;
    }
    int idx = find_index(key);
    return idx < 0 ? NULL : &s_table[idx];
}

int32_t kvm_setting_int(const char *key)
{
    if (!s_values) {
        return 0;
    }
    int idx = find_index(key);
    if (idx < 0 || !is_numeric(&s_table[idx])) {
        ESP_LOGW(TAG, "read of unknown numeric setting '%s'", key ? key : "(null)");
        return 0;
    }
    lock();
    int32_t v = eff((size_t)idx)->i;
    unlock();
    return v;
}

bool kvm_setting_bool(const char *key)
{
    return kvm_setting_int(key) != 0;
}

const char *kvm_setting_str(const char *key)
{
    if (!s_values) {
        return "";
    }
    int idx = find_index(key);
    if (idx < 0 || is_numeric(&s_table[idx])) {
        ESP_LOGW(TAG, "read of unknown string setting '%s'", key ? key : "(null)");
        return "";
    }
    lock();
    const char *v = eff((size_t)idx)->s;
    unlock();
    return v;
}

static esp_err_t persist_int(const char *key, int32_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t persist_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t kvm_setting_set_int(const char *key, int32_t value)
{
    ESP_RETURN_ON_FALSE(s_values, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    int idx = find_index(key);
    ESP_RETURN_ON_FALSE(idx >= 0, ESP_ERR_NOT_FOUND, TAG, "unknown setting '%s'", key);
    const kvm_setting_t *d = &s_table[idx];
    ESP_RETURN_ON_FALSE(is_numeric(d), ESP_ERR_INVALID_ARG, TAG, "'%s' is a string", key);
    ESP_RETURN_ON_FALSE(value >= numeric_min(d) && value <= numeric_max(d), ESP_ERR_INVALID_ARG, TAG,
                        "'%s' out of range", key);

    lock();
    const bool to_profile = (d->flags & KVM_SF_PROFILE) && profiles_custom_active();
    bool changed = eff((size_t)idx)->i != value;
    if (!to_profile) {
        s_values[idx].i = value;
    } else if (changed) {
        s_over[idx].i = value;
        s_over_set[idx] = true;
    }
    unlock();

    if (!changed) {
        return ESP_OK;
    }
    esp_err_t err;
    if (to_profile) {
        cJSON *n = cJSON_CreateNumber(value);
        err = n ? profiles_store(key, n) : ESP_ERR_NO_MEM;
        cJSON_Delete(n);
    } else {
        err = persist_int(key, value);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist '%s': %s", key, esp_err_to_name(err));
    }
    notify(key);
    return err;
}

esp_err_t kvm_setting_set_str(const char *key, const char *value)
{
    ESP_RETURN_ON_FALSE(s_values && value, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    int idx = find_index(key);
    ESP_RETURN_ON_FALSE(idx >= 0, ESP_ERR_NOT_FOUND, TAG, "unknown setting '%s'", key);
    const kvm_setting_t *d = &s_table[idx];
    ESP_RETURN_ON_FALSE(!is_numeric(d), ESP_ERR_INVALID_ARG, TAG, "'%s' is numeric", key);
    ESP_RETURN_ON_FALSE(strlen(value) <= d->max_len, ESP_ERR_INVALID_SIZE, TAG, "'%s' too long", key);

    lock();
    const bool to_profile = (d->flags & KVM_SF_PROFILE) && profiles_custom_active();
    bool changed = strcmp(eff((size_t)idx)->s, value) != 0;
    if (changed) {
        value_t *v = to_profile ? &s_over[idx] : &s_values[idx];
        strlcpy(v->s, value, (size_t)d->max_len + 1u);
        if (to_profile) {
            s_over_set[idx] = true;
        }
    }
    unlock();

    if (!changed) {
        return ESP_OK;
    }
    esp_err_t err;
    if (to_profile) {
        cJSON *n = cJSON_CreateString(value);
        err = n ? profiles_store(key, n) : ESP_ERR_NO_MEM;
        cJSON_Delete(n);
    } else {
        err = persist_str(key, value);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist '%s': %s", key, esp_err_to_name(err));
    }
    notify(key);
    return err;
}

/* Not a setting: an export must not carry it to another device. */
#define INSTALL_ID_KEY "install_id"

void kvm_install_id(char out[KVM_INSTALL_ID_LEN + 1])
{
    lock();
    if (!s_install_id[0]) {
        nvs_handle_t nvs;
        size_t len = sizeof(s_install_id);
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            if (nvs_get_str(nvs, INSTALL_ID_KEY, s_install_id, &len) != ESP_OK ||
                strlen(s_install_id) != KVM_INSTALL_ID_LEN) {
                uint8_t raw[KVM_INSTALL_ID_LEN / 2];
                esp_fill_random(raw, sizeof(raw));
                for (size_t i = 0; i < sizeof(raw); i++) {
                    snprintf(&s_install_id[i * 2], 3, "%02x", raw[i]);
                }
                if (nvs_set_str(nvs, INSTALL_ID_KEY, s_install_id) == ESP_OK) {
                    nvs_commit(nvs);
                }
            }
            nvs_close(nvs);
        }
    }
    strlcpy(out, s_install_id, KVM_INSTALL_ID_LEN + 1);
    unlock();
}

esp_err_t kvm_settings_reset(void)
{
    ESP_RETURN_ON_FALSE(s_values, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    /* The install id is not a setting: it survives a reset, so a reset device is
     * not counted twice. Erasing the flash makes a new one. */
    char id[KVM_INSTALL_ID_LEN + 1];
    kvm_install_id(id);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            (void)nvs_set_str(nvs, INSTALL_ID_KEY, id);
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    profiles_reset();
    lock();
    for (size_t i = 0; i < s_count; i++) {
        const kvm_setting_t *d = &s_table[i];
        s_over_set[i] = false;
        if (is_numeric(d)) {
            s_values[i].i = d->def;
        } else {
            strlcpy(s_values[i].s, d->def_str ? d->def_str : "", (size_t)d->max_len + 1u);
        }
    }
    unlock();

    ESP_LOGW(TAG, "settings reset to defaults");
    notify("*");
    return err;
}

esp_err_t kvm_settings_subscribe(kvm_settings_cb_t cb, void *user)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "cb");
    ESP_RETURN_ON_FALSE(s_sub_count < MAX_SUBSCRIBERS, ESP_ERR_NO_MEM, TAG, "too many subscribers");
    s_subs[s_sub_count].cb = cb;
    s_subs[s_sub_count].user = user;
    s_sub_count++;
    return ESP_OK;
}

static const char *type_name(kvm_val_type_t t)
{
    switch (t) {
    case KVM_VT_BOOL:
        return "bool";
    case KVM_VT_INT:
        return "int";
    case KVM_VT_ENUM:
        return "enum";
    default:
        return "string";
    }
}

char *kvm_settings_schema_json(void)
{
    /*
     * The schema carries its own sections: their titles, their order and what
     * each is for. The console draws whatever comes, so a new section or a new
     * heading is a line in the table here and nothing in the console at all.
     */
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON_AddItemToObject(root, "settings", arr);
    cJSON *secs = cJSON_AddArrayToObject(root, "sections");
    size_t section_count = 0;
    const kvm_section_t *sections = kvm_settings_sections(&section_count);
    for (size_t i = 0; secs && i < section_count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            break;
        }
        cJSON_AddStringToObject(o, "id", sections[i].id);
        cJSON_AddStringToObject(o, "title", sections[i].title);
        if (sections[i].blurb) {
            cJSON_AddStringToObject(o, "blurb", sections[i].blurb);
        }
        cJSON_AddItemToArray(secs, o);
    }
    for (size_t i = 0; i < s_count; i++) {
        const kvm_setting_t *d = &s_table[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(o, "key", d->key);
        cJSON_AddStringToObject(o, "section", d->section);
        if (d->group) {
            cJSON_AddStringToObject(o, "group", d->group);
        }
        cJSON_AddStringToObject(o, "title", d->title ? d->title : d->key);
        if (d->help) {
            cJSON_AddStringToObject(o, "help", d->help);
        }
        cJSON_AddStringToObject(o, "type", type_name(d->type));
        if (d->type == KVM_VT_INT) {
            cJSON_AddNumberToObject(o, "min", d->min);
            cJSON_AddNumberToObject(o, "max", d->max);
        }
        /* An enum names each value, a mask each bit: max is all bits set. */
        const bool mask = d->type == KVM_VT_INT && (d->flags & KVM_SF_MASK);
        int32_t names = d->type == KVM_VT_ENUM ? d->max + 1 : 0;
        while (mask && names < 31 && ((uint32_t)d->max >> names)) {
            names++;
        }
        if (names && d->choices) {
            cJSON *ch = cJSON_CreateArray();
            for (int32_t c = 0; c < names; c++) {
                cJSON_AddItemToArray(ch, cJSON_CreateString(d->choices[c]));
            }
            cJSON_AddItemToObject(o, "choices", ch);
        }
        if (names && d->labels) {
            cJSON *lb = cJSON_CreateArray();
            for (int32_t c = 0; c < names; c++) {
                cJSON_AddItemToArray(lb, cJSON_CreateString(d->labels[c]));
            }
            cJSON_AddItemToObject(o, "labels", lb);
        }
        if (mask) {
            cJSON_AddBoolToObject(o, "mask", true);
        }
        if (d->unit) {
            cJSON_AddStringToObject(o, "unit", d->unit);
        }
        if (d->type == KVM_VT_STR) {
            cJSON_AddNumberToObject(o, "maxLength", d->max_len);
            cJSON_AddStringToObject(o, "default", d->def_str ? d->def_str : "");
        } else {
            cJSON_AddNumberToObject(o, "default", d->def);
        }
        if (d->requires_cap >= 0) {
            cJSON_AddStringToObject(o, "requires", kvm_cap_key((kvm_cap_t)d->requires_cap));
        }
        if (d->flags & KVM_SF_REBOOT) {
            cJSON_AddBoolToObject(o, "reboot", true);
        }
        if (d->flags & KVM_SF_SECRET) {
            /* Write-only: the value is never read back (see kvm_settings_values_json),
             * so the console renders it as a password field and submits it only when
             * the operator types a new one. */
            cJSON_AddBoolToObject(o, "secret", true);
        }
        if (d->flags & KVM_SF_PIN) {
            cJSON_AddBoolToObject(o, "pin", true);
        }
        if (d->flags & KVM_SF_PROFILE) {
            cJSON_AddBoolToObject(o, "profile", true);
        }
        if (d->visible_key) {
            cJSON *si = cJSON_CreateObject();
            cJSON_AddStringToObject(si, "key", d->visible_key);
            if (d->flags & KVM_SF_SHOW_IN) {
                /* A set of values: the SPI LCD pins belong to two panel types. */
                cJSON *vals = cJSON_CreateArray();
                for (int b = 0; b < 31; b++) {
                    if ((uint32_t)d->visible_val & (1u << b)) {
                        cJSON_AddItemToArray(vals, cJSON_CreateNumber(b));
                    }
                }
                cJSON_AddItemToObject(si, (d->flags & KVM_SF_SHOW_NE) ? "notIn" : "in", vals);
            } else {
                cJSON_AddNumberToObject(si, (d->flags & KVM_SF_SHOW_NE) ? "ne" : "eq",
                                        d->visible_val);
            }
            cJSON_AddItemToObject(o, "showIf", si);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static char *values_json(bool with_profile)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    lock();
    for (size_t i = 0; i < s_count; i++) {
        const kvm_setting_t *d = &s_table[i];
        if (d->flags & KVM_SF_SECRET) {
            continue;
        }
        const value_t *v = with_profile ? eff(i) : &s_values[i];
        if (d->type == KVM_VT_BOOL) {
            cJSON_AddBoolToObject(root, d->key, v->i != 0);
        } else if (is_numeric(d)) {
            cJSON_AddNumberToObject(root, d->key, v->i);
        } else {
            cJSON_AddStringToObject(root, d->key, v->s);
        }
    }
    unlock();
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *kvm_settings_values_json(void)
{
    return values_json(true);
}

char *kvm_settings_default_values_json(void)
{
    return values_json(false);
}

/* The most pin settings this can weigh at once. There are nine today (three
 * ATX, six for the round LCD); anything past this is dropped rather than
 * checked, so raise it if the table ever grows that far. */
#define MAX_PIN_CLAIMS 32

/** The value @p idx will hold once @p root is applied. */
static int32_t resulting_int(const cJSON *root, size_t idx)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, s_table[idx].key);
    if (it) {
        if (cJSON_IsBool(it)) {
            return cJSON_IsTrue(it) ? 1 : 0;
        }
        if (cJSON_IsNumber(it)) {
            return (int32_t)it->valuedouble;
        }
    }
    return eff(idx)->i;
}

/*
 * Whether a pin setting counts.
 *
 * A setting the console hides is one the operator cannot see, so holding them
 * to it would be unfair: the round LCD's five pins are stored even on a device
 * running an I2C OLED, where they drive nothing at all. The rule here is the
 * console's rule - a setting with a showIf condition counts only while that
 * condition holds.
 */
static bool pin_setting_applies(const cJSON *root, const kvm_setting_t *d)
{
    /* A chain: the round LCD's pins hang on the panel type, which hangs on the
     * display being on. Bounded, so a table that loops cannot hang the write. */
    for (int depth = 0; d && d->visible_key && depth < 8; depth++) {
        int j = find_index(d->visible_key);
        if (j < 0) {
            return true;
        }
        const int32_t v = resulting_int(root, (size_t)j);
        const bool eq = (d->flags & KVM_SF_SHOW_IN)
                            ? (v >= 0 && v < 31 && ((uint32_t)d->visible_val & (1u << v)))
                            : v == d->visible_val;
        if (eq == ((d->flags & KVM_SF_SHOW_NE) != 0)) {
            return false;
        }
        d = &s_table[j];
    }
    return true;
}

/**
 * Refuse a write that would put two things on one pin.
 *
 * Pins are weighed as a set and after the request is imagined applied, because
 * moving the display off a pin the buttons want arrives as one body: each half
 * is legal only because the other changed with it.
 *
 * @return ESP_OK when the resulting wiring is sound.
 */
/* An mDNS / DNS label: letters, digits and '-', not at either end. */
static bool hostname_ok(const char *h)
{
    const size_t n = strlen(h);
    if (n == 0 || h[0] == '-' || h[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t check_pins(const cJSON *root, char *err, size_t err_len)
{
    kvm_pin_claim_t claims[MAX_PIN_CLAIMS];
    size_t n = 0;

    lock();
    for (size_t i = 0; i < s_count && n < MAX_PIN_CLAIMS; i++) {
        const kvm_setting_t *d = &s_table[i];
        if (!(d->flags & KVM_SF_PIN) || !pin_setting_applies(root, d)) {
            continue;
        }
        claims[n].key = d->key;
        claims[n].gpio = (int)resulting_int(root, i);
        /* A pin that comes back into view with this request (its switch just
         * turned on) is checked as if set now: it was never weighed while
         * hidden. */
        claims[n].changing = cJSON_GetObjectItemCaseSensitive(root, d->key) != NULL ||
                             !pin_setting_applies(NULL, d);
        n++;
    }
    unlock();

    kvm_pin_conflict_t c;
    if (!kvm_pin_conflict_find(claims, n, kvm_board_reserved_by, &c)) {
        return ESP_OK;
    }

    char msg[128];
    if (c.held_by) {
        snprintf(msg, sizeof(msg), "'%s' cannot use GPIO %d: it is the %s", c.key, c.gpio,
                 c.held_by);
    } else {
        snprintf(msg, sizeof(msg), "'%s' and '%s' cannot both use GPIO %d", c.key, c.other,
                 c.gpio);
    }
    ESP_LOGW(TAG, "refused: %s", msg);
    if (err && err_len) {
        strlcpy(err, msg, err_len);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t kvm_settings_apply_json(const char *json, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    ESP_RETURN_ON_FALSE(json, ESP_ERR_INVALID_ARG, TAG, "no body");

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        if (err) {
            strlcpy(err, "body must be a JSON object", err_len);
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate everything first: a partially applied settings write leaves the
     * device in a state the user never asked for. */
    for (cJSON *it = root->child; it; it = it->next) {
        int idx = find_index(it->string);
        if (idx < 0) {
            if (err) {
                snprintf(err, err_len, "unknown setting '%s'", it->string);
            }
            cJSON_Delete(root);
            return ESP_ERR_NOT_FOUND;
        }
        /* A key twice: the pin check reads the first, the write takes the last. */
        for (const cJSON *prev = root->child; prev != it; prev = prev->next) {
            if (strcmp(prev->string, it->string) == 0) {
                if (err) {
                    snprintf(err, err_len, "'%s' appears twice", it->string);
                }
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
        }
        const kvm_setting_t *d = &s_table[idx];
        if (d->type == KVM_VT_STR && strcmp(d->key, "net_hostname") == 0 && cJSON_IsString(it) &&
            !hostname_ok(it->valuestring)) {
            if (err) {
                strlcpy(err, "the hostname takes letters, digits and '-', and cannot start or end with '-'",
                        err_len);
            }
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (d->type == KVM_VT_STR) {
            if (!cJSON_IsString(it) || strlen(it->valuestring) > d->max_len) {
                if (err) {
                    snprintf(err, err_len, "'%s' must be a string of at most %u characters",
                             d->key, (unsigned)d->max_len);
                }
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
            continue;
        }
        int32_t v;
        if (cJSON_IsBool(it)) {
            v = cJSON_IsTrue(it) ? 1 : 0;
        } else if (cJSON_IsNumber(it)) {
            v = (int32_t)it->valuedouble;
        } else {
            if (err) {
                snprintf(err, err_len, "'%s' must be a number or boolean", d->key);
            }
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (v < numeric_min(d) || v > numeric_max(d)) {
            if (err) {
                snprintf(err, err_len, "'%s' must be between %ld and %ld", d->key,
                         (long)numeric_min(d), (long)numeric_max(d));
            }
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t pin_err = check_pins(root, err, err_len);
    if (pin_err != ESP_OK) {
        cJSON_Delete(root);
        return pin_err;
    }

    for (cJSON *it = root->child; it; it = it->next) {
        const kvm_setting_t *d = &s_table[find_index(it->string)];
        if (d->type == KVM_VT_STR) {
            (void)kvm_setting_set_str(d->key, it->valuestring);
        } else if (cJSON_IsBool(it)) {
            (void)kvm_setting_set_int(d->key, cJSON_IsTrue(it) ? 1 : 0);
        } else {
            (void)kvm_setting_set_int(d->key, (int32_t)it->valuedouble);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

size_t settings_count(void)
{
    return s_count;
}

/** Whether @p v fits @p d as it is stored: a number for numeric, else a string. */
static bool stored_value_ok(const kvm_setting_t *d, const cJSON *v)
{
    if (!is_numeric(d)) {
        return cJSON_IsString(v) && strlen(v->valuestring) <= d->max_len;
    }
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    const int32_t i = (int32_t)v->valuedouble;
    return i >= numeric_min(d) && i <= numeric_max(d);
}

bool settings_overlay_load(const cJSON *set, bool *changed)
{
    bool reboot = false;
    lock();
    for (size_t i = 0; i < s_count; i++) {
        const kvm_setting_t *d = &s_table[i];
        changed[i] = false;
        if (!(d->flags & KVM_SF_PROFILE)) {
            continue;
        }
        const cJSON *it = set ? cJSON_GetObjectItemCaseSensitive(set, d->key) : NULL;
        const bool own = it && stored_value_ok(d, it);
        if (it && !own) {
            ESP_LOGW(TAG, "profile value for '%s' does not fit; using Default", d->key);
        }
        if (is_numeric(d)) {
            const int32_t nv = own ? (int32_t)it->valuedouble : s_values[i].i;
            changed[i] = eff(i)->i != nv;
            if (own) {
                s_over[i].i = nv;
            }
        } else {
            const char *nv = own ? it->valuestring : s_values[i].s;
            changed[i] = strcmp(eff(i)->s, nv) != 0;
            if (own) {
                strlcpy(s_over[i].s, nv, (size_t)d->max_len + 1u);
            }
        }
        s_over_set[i] = own;
        if (changed[i] && (d->flags & KVM_SF_REBOOT)) {
            reboot = true;
        }
    }
    unlock();
    return reboot;
}

void settings_overlay_notify(const bool *changed)
{
    for (size_t i = 0; i < s_count; i++) {
        if (changed[i]) {
            notify(s_table[i].key);
        }
    }
}

cJSON *settings_profile_value(const char *key, const cJSON *v, char *err, size_t err_len)
{
    int idx = key ? find_index(key) : -1;
    if (idx < 0) {
        snprintf(err, err_len, "unknown setting '%s'", key ? key : "");
        return NULL;
    }
    const kvm_setting_t *d = &s_table[idx];
    if (!(d->flags & KVM_SF_PROFILE)) {
        snprintf(err, err_len, "'%s' is the same for every profile", key);
        return NULL;
    }
    cJSON *out = NULL;
    if (!is_numeric(d)) {
        out = cJSON_IsString(v) ? cJSON_CreateString(v->valuestring) : NULL;
    } else if (cJSON_IsBool(v)) {
        out = cJSON_CreateNumber(cJSON_IsTrue(v) ? 1 : 0);
    } else if (cJSON_IsNumber(v)) {
        out = cJSON_CreateNumber((int32_t)v->valuedouble);
    } else if (cJSON_IsString(v) && d->type == KVM_VT_ENUM && d->choices) {
        /* A preset names its choices: "relative" reads better than 1. */
        for (int32_t c = 0; c <= d->max; c++) {
            if (strcmp(d->choices[c], v->valuestring) == 0) {
                out = cJSON_CreateNumber(c);
                break;
            }
        }
    }
    if (!out || !stored_value_ok(d, out)) {
        cJSON_Delete(out);
        snprintf(err, err_len, "'%s' does not take that value", key);
        return NULL;
    }
    return out;
}
