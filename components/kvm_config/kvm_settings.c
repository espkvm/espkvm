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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

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
static SemaphoreHandle_t s_mu;
static subscriber_t s_subs[MAX_SUBSCRIBERS];
static uint8_t s_sub_count;

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
    ESP_RETURN_ON_FALSE(s_values, ESP_ERR_NO_MEM, TAG, "values");

    for (size_t i = 0; i < s_count; i++) {
        if (!is_numeric(&s_table[i])) {
            s_values[i].s = calloc(1, (size_t)s_table[i].max_len + 1u);
            ESP_RETURN_ON_FALSE(s_values[i].s, ESP_ERR_NO_MEM, TAG, "string %s", s_table[i].key);
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
    nvs_close(nvs);
    ESP_LOGI(TAG, "%u settings loaded", (unsigned)s_count);
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
    return s_values[idx].i;
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
    return s_values[idx].s;
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
    bool changed = s_values[idx].i != value;
    s_values[idx].i = value;
    unlock();

    if (!changed) {
        return ESP_OK;
    }
    esp_err_t err = persist_int(key, value);
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
    bool changed = strcmp(s_values[idx].s, value) != 0;
    if (changed) {
        strlcpy(s_values[idx].s, value, (size_t)d->max_len + 1u);
    }
    unlock();

    if (!changed) {
        return ESP_OK;
    }
    esp_err_t err = persist_str(key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist '%s': %s", key, esp_err_to_name(err));
    }
    notify(key);
    return err;
}

esp_err_t kvm_settings_reset(void)
{
    ESP_RETURN_ON_FALSE(s_values, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    lock();
    for (size_t i = 0; i < s_count; i++) {
        const kvm_setting_t *d = &s_table[i];
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
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
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
        cJSON_AddStringToObject(o, "title", d->title ? d->title : d->key);
        if (d->help) {
            cJSON_AddStringToObject(o, "help", d->help);
        }
        cJSON_AddStringToObject(o, "type", type_name(d->type));
        if (d->type == KVM_VT_INT) {
            cJSON_AddNumberToObject(o, "min", d->min);
            cJSON_AddNumberToObject(o, "max", d->max);
        }
        if (d->type == KVM_VT_ENUM && d->choices) {
            cJSON *ch = cJSON_CreateArray();
            for (int32_t c = 0; c <= d->max; c++) {
                cJSON_AddItemToArray(ch, cJSON_CreateString(d->choices[c]));
            }
            cJSON_AddItemToObject(o, "choices", ch);
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
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

char *kvm_settings_values_json(void)
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
        if (d->type == KVM_VT_BOOL) {
            cJSON_AddBoolToObject(root, d->key, s_values[i].i != 0);
        } else if (is_numeric(d)) {
            cJSON_AddNumberToObject(root, d->key, s_values[i].i);
        } else {
            cJSON_AddStringToObject(root, d->key, s_values[i].s);
        }
    }
    unlock();
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
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
        const kvm_setting_t *d = &s_table[idx];
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
