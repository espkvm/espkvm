/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Schema-driven settings, persisted in NVS.
 *
 * Every setting is declared once in kvm_settings_table.c with its type, range,
 * default, owning section and required capability. The REST API serves that
 * schema verbatim, so the settings panel in the browser renders itself and a
 * new setting needs no UI work - only a table row.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kvm_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_VT_BOOL,
    KVM_VT_INT,
    KVM_VT_ENUM,
    KVM_VT_STR,
} kvm_val_type_t;

enum {
    KVM_SF_REBOOT = 1u << 0, /**< takes effect only after a restart */
    KVM_SF_SECRET = 1u << 1, /**< never serialise the value (passwords) */
};

typedef struct {
    const char *key;     /**< NVS key, <= 15 chars, also the JSON field name */
    const char *section; /**< groups settings into UI tabs */
    const char *title;
    const char *help;
    kvm_val_type_t type;
    int32_t min;
    int32_t max;
    int32_t def;                /**< default for BOOL/INT/ENUM (enum: index) */
    const char *const *choices; /**< ENUM only, @p max + 1 entries */
    const char *def_str;        /**< STR only */
    uint16_t max_len;           /**< STR only, excluding the terminator */
    int8_t requires_cap;        /**< kvm_cap_t, or -1 when always applicable */
    uint8_t flags;              /**< KVM_SF_* */
} kvm_setting_t;

/** Called after a value changed and was persisted. @p key is never NULL. */
typedef void (*kvm_settings_cb_t)(const char *key, void *user);

/** Load every setting from NVS, filling in defaults for missing keys. */
esp_err_t kvm_settings_init(void);

const kvm_setting_t *kvm_settings_table(size_t *out_count);
const kvm_setting_t *kvm_setting_find(const char *key);

int32_t kvm_setting_int(const char *key);
bool kvm_setting_bool(const char *key);
/** @return the stored string, never NULL; valid until the next set on that key. */
const char *kvm_setting_str(const char *key);

esp_err_t kvm_setting_set_int(const char *key, int32_t value);
esp_err_t kvm_setting_set_str(const char *key, const char *value);

/** Restore every setting to its declared default and erase the NVS namespace. */
esp_err_t kvm_settings_reset(void);

esp_err_t kvm_settings_subscribe(kvm_settings_cb_t cb, void *user);

/** malloc'd JSON; caller frees. Schema is static, values reflect current state. */
char *kvm_settings_schema_json(void);
char *kvm_settings_values_json(void);

/**
 * Apply a flat JSON object of key/value pairs. All-or-nothing: the request is
 * validated in full before anything is written.
 * @param err  buffer for a human-readable rejection message
 */
esp_err_t kvm_settings_apply_json(const char *json, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
