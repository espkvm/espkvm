/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device profiles: settings of their own for each attached device.
 *
 * Default is the settings as they always were, in the same NVS keys; it cannot
 * be deleted. A profile of one's own keeps only the values it changes, and only
 * for KVM_SF_PROFILE keys, in its own namespace. While it is active, reads see
 * its values over Default's and writes to those keys go to it. A device that
 * never made a profile behaves as before profiles existed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "profile_match.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_PROFILE_DEFAULT 0
/** Profiles of one's own, ids 1..KVM_PROFILES_MAX. */
#define KVM_PROFILES_MAX 8

uint8_t kvm_profiles_active(void);
/** Whether the active profile was chosen by hand rather than by the device. */
bool kvm_profiles_manual(void);

/**
 * malloc'd JSON: the profiles, the active one, the presets, and the console's
 * questions ("welcome", "suggest"). Caller frees.
 */
char *kvm_profiles_json(void);

/**
 * Make a profile from {"name", "from", "match", "set", "select"}. "from" is
 * "current" (the active profile's own values, the default), "empty" or a
 * preset id; "set" adds values of its own over that.
 * "select" (default true) makes it active.
 * @param reboot  set when a value that needs a restart moved
 */
esp_err_t kvm_profiles_create(const char *json, uint8_t *id, bool *reboot, char *err, size_t err_len);

/**
 * Change a profile with {"id", "name", "match", "set", "unset"}: "set" merges
 * values in (null drops one), "unset" is a list of keys to take from Default.
 */
esp_err_t kvm_profiles_update(const char *json, bool *reboot, char *err, size_t err_len);

/** ESP_ERR_NOT_ALLOWED for Default. The active one gives way to Default. */
esp_err_t kvm_profiles_delete(uint8_t id, bool *reboot);

esp_err_t kvm_profiles_select(uint8_t id, bool manual, bool *reboot);

/** The profile whose rule fits @p seen best, or KVM_PROFILE_DEFAULT. */
uint8_t kvm_profiles_pick(const kvm_prof_seen_t *seen);

/**
 * The first password is set: the console asks once which devices this one
 * will see ("welcome" in the JSON) and makes profiles for them.
 */
void kvm_profiles_welcome(void);

/**
 * The console's questions, answered: {"welcome": true} ends the first-run one,
 * {"tour": "<version>"} the overview of features, closed on that firmware
 * version ("tourSeen" in the JSON, "" when never),
 * {"preset": id} stops "suggest" offering that preset again.
 */
esp_err_t kvm_profiles_dismiss(const char *json, char *err, size_t err_len);

/** Whether any profile of one's own exists. */
bool kvm_profiles_any(void);

/**
 * What is attached now, @p fp its fingerprint ("" when nothing is): for GET
 * /api/v1/profiles ("seen"), and so a choice by hand holds for this device.
 */
void kvm_profiles_note_seen(const kvm_prof_seen_t *seen, const char *fp);

/**
 * A device has settled, @p fp being its fingerprint. When it differs from the
 * last one acted on, the choice by hand ends and the best profile is selected.
 * The same device again gets the best profile too, unless one was picked by hand.
 * @return whether the active profile changed
 */
bool kvm_profiles_device_settled(const kvm_prof_seen_t *seen, const char *fp);

/**
 * May the device restart by itself to apply a profile? Counts the restarts in
 * a row and refuses the third, so two profiles that keep choosing each other
 * cannot restart it forever. kvm_profiles_uptime_ok() resets the count.
 */
bool kvm_profiles_restart_allowed(void);
void kvm_profiles_uptime_ok(void);

#ifdef __cplusplus
}
#endif
