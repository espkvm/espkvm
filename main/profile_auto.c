/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Picks the device profile for the attached device, without a console open.
 *
 * Once a second it looks at the CEC target (name, vendor) and the OS guessed
 * from USB. A device that looks the same for SETTLE_S seconds is handed to
 * kvm_profiles_device_settled(), which acts only when it differs from the last
 * device acted on - so a restart, or a profile chosen by hand, does not undo
 * itself. The gamepad and the EDID are read once at boot: when the profile in
 * effect moved one of them, the device restarts, but only once things have
 * been still for a while, and never more than twice in a row.
 */
#include "profile_auto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_cec.h"
#include "kvm_profiles.h"
#include "kvm_settings.h"
#include "usb_hid.h"

static const char *TAG = "prof_auto";

#define SETTLE_S 3
/* How long a device must stay before the restart that applies its profile. */
#define RESTART_STILL_S 10
/* Not earlier after boot: CEC names take a few seconds to arrive. */
#define RESTART_UPTIME_S 20
/* Up this long without a restart for a profile: the count starts again. */
#define CALM_UPTIME_S 300

/* Settings that take effect only at boot and a profile may hold. */
static const char *const k_boot_keys[] = {"usb_pad", "usb_legacy", "edid_prof"};
static int32_t s_boot_val[sizeof(k_boot_keys) / sizeof(k_boot_keys[0])];
/* The profile those values came with. */
static uint8_t s_boot_profile;

static void take_boot_values(void)
{
    for (size_t i = 0; i < sizeof(k_boot_keys) / sizeof(k_boot_keys[0]); i++) {
        s_boot_val[i] = kvm_setting_int(k_boot_keys[i]);
    }
    s_boot_profile = kvm_profiles_active();
}

static bool boot_values_moved(void)
{
    for (size_t i = 0; i < sizeof(k_boot_keys) / sizeof(k_boot_keys[0]); i++) {
        if (kvm_setting_int(k_boot_keys[i]) != s_boot_val[i]) {
            return true;
        }
    }
    return false;
}

static void look(kvm_cec_status_t *st, char *name, size_t name_len, char *vendor, size_t vendor_len,
                 char *os, size_t os_len)
{
    name[0] = vendor[0] = os[0] = '\0';
    kvm_cec_status(st);
    const int la = kvm_cec_target();
    if (st->running && la >= 0 && la < KVM_CEC_DEVICES && st->dev[la].present) {
        strlcpy(name, st->dev[la].name, name_len);
        if (st->dev[la].vendor != 0xffffffff) {
            snprintf(vendor, vendor_len, "%06lx", (unsigned long)st->dev[la].vendor);
        }
    }
    const char *guess = usb_hid_target_os();
    if (guess && strcmp(guess, "unknown") != 0) {
        strlcpy(os, guess, os_len);
    }
}

static void task(void *arg)
{
    (void)arg;
    kvm_cec_status_t *st = malloc(sizeof(*st));
    if (!st) {
        ESP_LOGE(TAG, "no memory; profiles are picked by hand only");
        vTaskDelete(NULL);
        return;
    }
    char name[16], vendor[9], os[12], fp[48], last[48] = "", acted[48] = "";
    int still = 0;
    bool calm_done = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t up_s = esp_timer_get_time() / 1000000;
        if (!calm_done && up_s >= CALM_UPTIME_S) {
            kvm_profiles_uptime_ok();
            calm_done = true;
        }
        if (!kvm_profiles_any()) {
            continue; /* nothing to choose from, and nothing written */
        }

        look(st, name, sizeof(name), vendor, sizeof(vendor), os, sizeof(os));
        if (name[0] || vendor[0] || os[0]) {
            snprintf(fp, sizeof(fp), "%s|%s|%s", name, vendor, os);
        } else {
            fp[0] = '\0';
        }
        const kvm_prof_seen_t seen = {name, vendor, os};
        kvm_profiles_note_seen(&seen, fp);

        if (strcmp(fp, last) != 0) {
            strlcpy(last, fp, sizeof(last));
            still = 0;
            continue;
        }
        still++;
        if (fp[0] && still >= SETTLE_S && strcmp(fp, acted) != 0) {
            strlcpy(acted, fp, sizeof(acted));
            (void)kvm_profiles_device_settled(&seen, fp);
        }
        /* Same profile as at boot: a value that moved was set by hand in
         * Settings, which marks it "restart" and leaves the restart to the
         * user. Restarting for it here rebooted the device 10 s after an EDID
         * change. */
        if (kvm_profiles_active() == s_boot_profile) {
            take_boot_values();
        }
        /* After a choice by hand the console asks about the restart itself. */
        if (still >= RESTART_STILL_S && up_s >= RESTART_UPTIME_S && !kvm_profiles_manual() &&
            boot_values_moved()) {
            if (kvm_profiles_restart_allowed()) {
                ESP_LOGW(TAG, "the profile changes the gamepad or the EDID: restarting to apply it");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            /* Refused: take what is in effect now as the boot values, so it
             * is not asked again every second. */
            take_boot_values();
        }
    }
}

void profile_auto_start(void)
{
    take_boot_values();
    /* Internal stack: it writes NVS, which a stack in PSRAM must not do. */
    if (xTaskCreate(task, "prof_auto", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task not started; profiles are picked by hand only");
    }
}
