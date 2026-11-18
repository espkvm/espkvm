/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Between the settings store and the profiles that sit over it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

/* kvm_settings.c */

size_t settings_count(void);
/**
 * Put a profile's own values (a JSON object, NULL for none) over the stored
 * ones. Keys that are unknown, not KVM_SF_PROFILE or out of range are skipped.
 * @param changed  settings_count() entries: which values in effect moved
 * @return whether a moved value needs a restart
 */
bool settings_overlay_load(const cJSON *set, bool *changed);
/** Tell the subscribers about each moved value. Call with no lock held. */
void settings_overlay_notify(const bool *changed);
/**
 * Check a value for a profile and return it as it is stored: an index for an
 * enum named by its choice, 0/1 for a bool. NULL with @p err filled when it
 * does not fit.
 */
cJSON *settings_profile_value(const char *key, const cJSON *v, char *err, size_t err_len);

/* kvm_profiles.c */

/** Read the profiles and put the active one over the values. Once, at init. */
void profiles_init(void);
bool profiles_custom_active(void);
/** Keep @p key's value in the active profile and save it. */
esp_err_t profiles_store(const char *key, const cJSON *value);
/** Forget every profile; Default becomes active. */
void profiles_reset(void);
