/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "capture.h"
#include "ethernet.h"
#include "http_server.h"
#include "kvm_auth.h"
#include "kvm_caps.h"
#include "kvm_storage.h"
#include "kvm_thermal.h"
#include "kvm_settings.h"
#include "usb_hid.h"
#include "video_frame.h"

static const char *TAG = "espkvm";

/*
 * Features that are compiled in but have no implementation yet report
 * themselves unavailable, so the web UI shows a disabled control with a reason
 * instead of one that silently does nothing.
 */
/** Log verbosity is a setting so a field problem can be traced without a
 *  rebuild, and turned back down afterwards. */
static void apply_log_level(void)
{
    static const esp_log_level_t levels[] = {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
                                             ESP_LOG_DEBUG};
    const int32_t choice = kvm_setting_int("log_level");
    const size_t n = sizeof(levels) / sizeof(levels[0]);
    esp_log_level_set("*", levels[(choice >= 0 && (size_t)choice < n) ? (size_t)choice : 2]);
}

static void apply_media_selection(void);

static void on_setting_changed(const char *key, void *user)
{
    (void)user;
    if (strcmp(key, "log_level") == 0 || strcmp(key, "*") == 0) {
        apply_log_level();
    }
    if (strcmp(key, "msc_enable") == 0 || strcmp(key, "msc_image") == 0 ||
        strcmp(key, "*") == 0) {
        apply_media_selection();
    }
}

/*
 * Reconcile the virtual drive with the settings: offer the chosen image to the
 * target when the card is mounted, the feature is on, and an image is named;
 * otherwise show an empty drive. Called at boot and whenever a storage setting
 * changes, so inserting or ejecting from the console takes effect at once
 * without a USB re-enumeration.
 */
static void apply_media_selection(void)
{
    kvm_storage_status_t sd;
    kvm_storage_status(&sd);
    kvm_cap_report(KVM_CAP_MSC, sd.mounted, "no microSD card in the slot");

    const char *image = kvm_setting_str("msc_image");
    if (sd.mounted && kvm_setting_bool("msc_enable") && image && image[0]) {
        esp_err_t err = kvm_storage_media_select(image);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cannot offer image '%s': %s", image, esp_err_to_name(err));
            kvm_storage_media_eject();
        }
    } else {
        kvm_storage_media_eject();
    }
}

static void report_pending_capabilities(void)
{
    apply_media_selection();
    kvm_cap_report(KVM_CAP_ATX, false, "power control not implemented yet");
    kvm_cap_report(KVM_CAP_AUDIO, false, "audio capture not implemented yet");
    kvm_cap_report(KVM_CAP_NET_STATIC, false, "static addressing not implemented yet; the "
                                              "device takes an address by DHCP");
    /* HTTPS reports itself from the web server, which knows whether the TLS
     * listener actually came up. */

    const esp_partition_t *ota = esp_ota_get_next_update_partition(NULL);
    kvm_cap_report(KVM_CAP_OTA, ota != NULL, "partition table has no second app slot");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Settings first: capability enable flags and every module's defaults read
     * from here. */
    ESP_ERROR_CHECK(kvm_settings_init());
    apply_log_level();
    ESP_ERROR_CHECK(kvm_settings_subscribe(on_setting_changed, NULL));
    kvm_caps_init();
    /* Before the capture path: the guard should be watching from the first
     * frame, not from whenever the web server happens to start. */
    kvm_thermal_init();

    /* The microSD card, if any. A KVM without one is still a KVM, so a missing
     * or unreadable card never holds up start-up. */
    ESP_ERROR_CHECK(kvm_storage_init());

    /*
     * Before the network: the button shares its pin with the Ethernet
     * interface, so this is the only moment it can be read without cost.
     * Holding it now clears a forgotten password.
     */
    kvm_auth_check_reset_button();

    ESP_ERROR_CHECK(ethernet_init());
    ESP_ERROR_CHECK(usb_hid_init());

    /* Before the web server, which reads published frames, and before the
     * capture task, which installs the codec's buffers into it. */
    video_frame_store_init();
    (void)http_server_start();

    capture_start();

    report_pending_capabilities();
    kvm_caps_log();

    /*
     * Confirm the image only after everything above came up. With rollback
     * enabled the bootloader reverts to the previous slot unless it is told
     * the new one works, which is the behaviour we want on a device that is
     * often the only way to reach the machine it is attached to. Reaching this
     * line means USB, the web server and the capture task all started.
     */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "image confirmed, rollback cancelled");
        }
    }

    ESP_LOGI(TAG, "ready");
}
