/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * An update on trial (see kvm_ota_trial.h).
 *
 * The request crosses a restart, so it lives in NVS: the trial length and the
 * ELF hash of the image it is for. The new image honours it only if the hash
 * is its own, so a request left behind by an image that never got this far
 * cannot put a later, unrelated update on trial.
 *
 * Two things end a trial that nobody confirms. A timer rolls back cleanly. The
 * RTC watchdog, armed well past the timer, covers a firmware hung so badly
 * that the timer never runs: its reset finds the image still unconfirmed, and
 * the bootloader starts the old one.
 */
#include "kvm_ota_trial.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "hal/wdt_hal.h"
#include "nvs.h"
#include "soc/rtc.h"
#include "web_priv.h"

static const char *TAG = "ota_trial";

#define NVS_NS "ota_trial"
#define KEY_SECS "secs"
#define KEY_SHA "sha"

/* Well past the timer, so the watchdog only fires when the timer could not.
 * The RTC slow clock is a rough RC oscillator, hence the margin in proportion. */
#define WATCHDOG_AFTER(s) ((s) + (s) / 4 + 60)

static esp_timer_handle_t s_timer;
static int64_t s_deadline_us; /* 0 when no trial runs */
static bool s_rolled_back;    /* decided; only the restart is left */

static void forget_request(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t kvm_ota_trial_arm(const esp_partition_t *target, uint32_t seconds)
{
    if (seconds == 0) {
        forget_request();
        return ESP_OK;
    }
    if (seconds < KVM_OTA_TRIAL_MIN_S || seconds > KVM_OTA_TRIAL_MAX_S) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_app_desc_t d;
    esp_err_t err = esp_ota_get_partition_description(target, &d);
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, KEY_SECS, seconds);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_SHA, d.app_elf_sha256, sizeof(d.app_elf_sha256));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "%s goes on trial for %u s after the restart", d.version, (unsigned)seconds);
    }
    return err;
}

/* The trial length asked for this exact image, 0 when none. Read once: the
 * request is erased either way. */
static uint32_t take_request(void)
{
    uint32_t secs = 0;
    uint8_t sha[32] = {0};
    size_t len = sizeof(sha);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0; /* nothing was ever stored */
    }
    const bool found = nvs_get_u32(h, KEY_SECS, &secs) == ESP_OK &&
                       nvs_get_blob(h, KEY_SHA, sha, &len) == ESP_OK && len == sizeof(sha);
    nvs_close(h);
    forget_request();
    if (!found) {
        return 0;
    }
    if (memcmp(sha, esp_app_get_description()->app_elf_sha256, sizeof(sha)) != 0) {
        ESP_LOGW(TAG, "a trial was asked for another image; ignored");
        return 0;
    }
    return secs;
}

static void watchdog_set(uint32_t seconds)
{
    /* Init leaves it disabled with every stage off, which is also "stop". */
    wdt_hal_context_t ctx;
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    if (!seconds) {
        return;
    }
    const uint32_t ticks = (uint32_t)((uint64_t)seconds * rtc_clk_slow_freq_get_hz());
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

static void on_timeout(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "nobody confirmed the update; going back to the previous image");
    kvm_ota_trial_rollback();
    kvm_web_restart_soon(100);
}

bool kvm_ota_trial_begin(void)
{
    esp_ota_img_states_t state;
    const bool pending =
        esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;
    /* Taken on every boot, so a request left by an image that never booted does
     * not linger. */
    const uint32_t secs = take_request();
    if (!pending || secs == 0) {
        return false;
    }

    const esp_timer_create_args_t args = {.callback = on_timeout, .name = "ota_trial"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK ||
        esp_timer_start_once(s_timer, (uint64_t)secs * 1000000ULL) != ESP_OK) {
        /* No clock means no rollback to promise. The image is up and serving,
         * which is what an update without a trial asks for, so confirm it. */
        ESP_LOGE(TAG, "could not start the trial timer; confirming the image instead");
        return false;
    }
    watchdog_set(WATCHDOG_AFTER(secs));
    s_deadline_us = esp_timer_get_time() + (int64_t)secs * 1000000;
    ESP_LOGW(TAG, "update on trial: confirm it within %u s or it rolls back", (unsigned)secs);
    return true;
}

uint32_t kvm_ota_trial_left(void)
{
    if (!s_deadline_us) {
        return 0;
    }
    const int64_t left = s_deadline_us - esp_timer_get_time();
    return left > 0 ? (uint32_t)((left + 999999) / 1000000) : 1;
}

esp_err_t kvm_ota_trial_keep(void)
{
    if (!s_deadline_us || s_rolled_back) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
        return err;
    }
    esp_timer_stop(s_timer);
    watchdog_set(0);
    s_deadline_us = 0;
    ESP_LOGW(TAG, "update confirmed by the operator; rollback cancelled");
    return ESP_OK;
}

void kvm_ota_trial_rollback(void)
{
    s_rolled_back = true;
    esp_timer_stop(s_timer);
    const esp_err_t err = esp_ota_mark_app_invalid_rollback();
    if (err != ESP_OK) {
        /* Nothing lost: a restart of an unconfirmed image is a rollback too,
         * the bootloader's own. */
        ESP_LOGE(TAG, "esp_ota_mark_app_invalid_rollback: %s", esp_err_to_name(err));
    }
}
