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
    KVM_SF_PIN = 1u << 2,    /**< an INT that is a GPIO number: the console renders it
                                 as a pin picker (free GPIOs + "None"), and it feeds the
                                 pin map. -1 means unassigned. */
    KVM_SF_PROFILE = 1u << 3, /**< a device profile may hold its own value (kvm_profiles.h) */
    KVM_SF_SHOW_NE = 1u << 4, /**< shown while setting[visible_key] is NOT visible_val */
    KVM_SF_SHOW_IN = 1u << 5, /**< visible_val is a bit mask: shown while bit
                                 setting[visible_key] is set in it (with SHOW_NE: clear) */
    KVM_SF_MASK = 1u << 6,    /**< an INT that is a set of bits, one per entry of
                                 @p choices (bit 0 first). The console draws the
                                 mask settings of one group as a table: a row per
                                 bit, a column per setting. */
};

typedef struct {
    const char *key;     /**< NVS key, <= 15 chars, also the JSON field name */
    const char *section; /**< groups settings into UI tabs */
    /** A heading within the section, for the console to gather rows under.
     *  NULL puts the setting above the first heading. */
    const char *group;
    const char *title;
    const char *help;
    kvm_val_type_t type;
    int32_t min;
    int32_t max;
    int32_t def;                /**< default for BOOL/INT/ENUM (enum: index) */
    const char *const *choices; /**< ENUM: @p max + 1 entries. KVM_SF_MASK: one per bit */
    const char *def_str;        /**< STR only */
    uint16_t max_len;           /**< STR only, excluding the terminator */
    int8_t requires_cap;        /**< kvm_cap_t, or -1 when always applicable */
    uint8_t flags;              /**< KVM_SF_* */
    const char *visible_key;    /**< when non-NULL, the console shows this setting only
                                     while setting[visible_key] equals @p visible_val
                                     (or differs from it, with KVM_SF_SHOW_NE), and only
                                     while that setting is shown itself
                                     (e.g. the GC9A01 pins only for the round LCD type) */
    int32_t visible_val;
    const char *const *labels;  /**< ENUM or KVM_SF_MASK, optional: what the console shows
                                     for each choice; @p choices stay the names the API takes */
    const char *unit;           /**< INT only, optional: shown after the number ("ms") */
} kvm_setting_t;

/**
 * A section of the settings, as the console draws it: the tabs, their order and
 * what each one is for. Declared beside the settings so the console needs no
 * list of its own - a new section appears by being written here.
 */
typedef struct {
    const char *id;    /**< matches kvm_setting_t::section */
    const char *title; /**< what the tab says */
    const char *blurb; /**< one line under the title, or NULL */
} kvm_section_t;

const kvm_section_t *kvm_settings_sections(size_t *out_count);

/** Called after a value changed and was persisted. @p key is never NULL. */
typedef void (*kvm_settings_cb_t)(const char *key, void *user);

/** Load every setting from NVS, filling in defaults for missing keys. */
esp_err_t kvm_settings_init(void);

const kvm_setting_t *kvm_settings_table(size_t *out_count);
const kvm_setting_t *kvm_setting_find(const char *key);

/* Reads give the value in effect: the active profile's own one when it has it,
 * else the stored (Default) one. */
int32_t kvm_setting_int(const char *key);
bool kvm_setting_bool(const char *key);
/** @return the stored string, never NULL; valid until the next set on that key. */
const char *kvm_setting_str(const char *key);

/* A KVM_SF_PROFILE key is written to the active profile unless that is Default. */
esp_err_t kvm_setting_set_int(const char *key, int32_t value);
esp_err_t kvm_setting_set_str(const char *key, const char *value);

#define KVM_INSTALL_ID_LEN 32

/**
 * This install's id: 128 random bits as hex, made on first use and kept in NVS.
 * Not derived from the hardware; a settings reset makes a new one. The console
 * sends it with the update check so devices in use can be counted.
 */
void kvm_install_id(char out[KVM_INSTALL_ID_LEN + 1]);

/** Restore every setting to its declared default and erase the NVS namespace,
 *  the profiles with it. */
esp_err_t kvm_settings_reset(void);

esp_err_t kvm_settings_subscribe(kvm_settings_cb_t cb, void *user);

/** malloc'd JSON; caller frees. Schema is static, values reflect current state. */
char *kvm_settings_schema_json(void);
char *kvm_settings_values_json(void);
/** The values Default holds, under whatever profile is active. */
char *kvm_settings_default_values_json(void);

/**
 * Apply a flat JSON object of key/value pairs. All-or-nothing: the request is
 * validated in full before anything is written.
 * @param err  buffer for a human-readable rejection message
 */
esp_err_t kvm_settings_apply_json(const char *json, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
