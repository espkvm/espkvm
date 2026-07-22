/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_caps.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "kvm_settings.h"

static const char *TAG = "kvm_caps";

#define REASON_LEN 96

typedef struct {
    const char *key;
    bool compiled;
    /** Setting that switches this capability off, or NULL when it has none. */
    const char *enable_key;
} cap_desc_t;

/* Capabilities the user cannot turn off (video, HID) have no enable_key: a KVM
 * without a keyboard is not a mode worth offering. */
static const cap_desc_t s_desc[KVM_CAP_COUNT] = {
    [KVM_CAP_VIDEO] = {"video", true, NULL},
    [KVM_CAP_MJPEG] = {"mjpeg", true, NULL},
    [KVM_CAP_H264] = {"h264",
#if CONFIG_KVM_ENABLE_H264
                      true,
#else
                      false,
#endif
                      NULL},
    [KVM_CAP_HID] = {"hid", true, NULL},
    [KVM_CAP_MSC] = {"msc",
#if CONFIG_KVM_ENABLE_MSC
                     true,
#else
                     false,
#endif
                     "msc_enable"},
    [KVM_CAP_ATX] = {"atx",
#if CONFIG_KVM_ENABLE_ATX
                     true,
#else
                     false,
#endif
                     "atx_enable"},
    [KVM_CAP_AUDIO] = {"audio",
#if CONFIG_KVM_ENABLE_AUDIO
                       true,
#else
                       false,
#endif
                       "aud_enable"},
    [KVM_CAP_HTTPS] = {"https",
#if CONFIG_KVM_ENABLE_HTTPS
                       true,
#else
                       false,
#endif
                       "sec_https"},
    [KVM_CAP_OTA] = {"ota", true, NULL},
    [KVM_CAP_NET_STATIC] = {"net_static", true, NULL},
};

typedef struct {
    bool available;
    char reason[REASON_LEN];
} cap_state_t;

static cap_state_t s_state[KVM_CAP_COUNT];

static bool cap_valid(kvm_cap_t cap)
{
    return cap >= 0 && cap < KVM_CAP_COUNT;
}

void kvm_caps_init(void)
{
    for (int i = 0; i < KVM_CAP_COUNT; i++) {
        s_state[i].available = false;
        if (s_desc[i].compiled) {
            snprintf(s_state[i].reason, REASON_LEN, "not probed yet");
        } else {
            snprintf(s_state[i].reason, REASON_LEN, "not built into this firmware");
        }
    }
}

void kvm_cap_report(kvm_cap_t cap, bool available, const char *reason_fmt, ...)
{
    if (!cap_valid(cap)) {
        return;
    }
    if (!s_desc[cap].compiled) {
        /* A module reporting a capability that was compiled out is a bug, but it
         * must not resurrect code that is not linked in. */
        ESP_LOGW(TAG, "%s probed but not compiled in", s_desc[cap].key);
        return;
    }
    s_state[cap].available = available;
    if (available) {
        s_state[cap].reason[0] = '\0';
        return;
    }
    if (reason_fmt) {
        va_list ap;
        va_start(ap, reason_fmt);
        vsnprintf(s_state[cap].reason, REASON_LEN, reason_fmt, ap);
        va_end(ap);
    } else {
        snprintf(s_state[cap].reason, REASON_LEN, "unavailable");
    }
}

bool kvm_cap_compiled(kvm_cap_t cap)
{
    return cap_valid(cap) && s_desc[cap].compiled;
}

bool kvm_cap_available(kvm_cap_t cap)
{
    return cap_valid(cap) && s_desc[cap].compiled && s_state[cap].available;
}

static bool cap_enabled(kvm_cap_t cap)
{
    const char *key = s_desc[cap].enable_key;
    return key ? kvm_setting_bool(key) : true;
}

bool kvm_cap_active(kvm_cap_t cap)
{
    return kvm_cap_available(cap) && cap_enabled(cap);
}

const char *kvm_cap_key(kvm_cap_t cap)
{
    return cap_valid(cap) ? s_desc[cap].key : "?";
}

const char *kvm_cap_reason(kvm_cap_t cap)
{
    return cap_valid(cap) ? s_state[cap].reason : "";
}

void kvm_caps_log(void)
{
    for (int i = 0; i < KVM_CAP_COUNT; i++) {
        if (kvm_cap_active((kvm_cap_t)i)) {
            ESP_LOGI(TAG, "%-6s active", s_desc[i].key);
        } else if (kvm_cap_available((kvm_cap_t)i)) {
            ESP_LOGI(TAG, "%-6s available, switched off in settings", s_desc[i].key);
        } else {
            ESP_LOGI(TAG, "%-6s unavailable: %s", s_desc[i].key, s_state[i].reason);
        }
    }
}

char *kvm_caps_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    for (int i = 0; i < KVM_CAP_COUNT; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddBoolToObject(o, "compiled", s_desc[i].compiled);
        cJSON_AddBoolToObject(o, "available", kvm_cap_available((kvm_cap_t)i));
        cJSON_AddBoolToObject(o, "enabled", cap_enabled((kvm_cap_t)i));
        cJSON_AddBoolToObject(o, "active", kvm_cap_active((kvm_cap_t)i));
        if (s_desc[i].enable_key) {
            cJSON_AddStringToObject(o, "setting", s_desc[i].enable_key);
        }
        if (!kvm_cap_available((kvm_cap_t)i)) {
            cJSON_AddStringToObject(o, "reason", s_state[i].reason);
        }
        cJSON_AddItemToObject(root, s_desc[i].key, o);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
