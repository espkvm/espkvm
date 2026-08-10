/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "capture.h"
#include "kvm_display.h"
#include "ethernet.h"
#include "wifi.h"
#include "http_server.h"
#include "kvm_atx.h"
#include "kvm_auth.h"
#include "kvm_caps.h"
#include "kvm_mqtt.h"
#include "kvm_storage.h"
#include "kvm_ts.h"
#include "kvm_tls.h"
#include "kvm_wg.h"
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
    if (strncmp(key, "atx_", 4) == 0 || strcmp(key, "*") == 0) {
        kvm_atx_apply();
    }
    if (strncmp(key, "mqtt_", 5) == 0 || strcmp(key, "*") == 0) {
        kvm_mqtt_apply();
    }
    /* WireGuard and Tailscale share one WireGuard stack, so only one may be
     * active at a time; turning one on turns the other off. */
    if (strcmp(key, "wg_enable") == 0 && kvm_setting_bool("wg_enable")) {
        kvm_setting_set_int("ts_enable", 0);
    } else if (strcmp(key, "ts_enable") == 0 && kvm_setting_bool("ts_enable")) {
        kvm_setting_set_int("wg_enable", 0);
    }
    if (strncmp(key, "wg_", 3) == 0 || strcmp(key, "*") == 0) {
        kvm_wg_apply();
    }
    if (strncmp(key, "ts_", 3) == 0 || strcmp(key, "*") == 0) {
        kvm_ts_apply();
    }
    /* ATX or WoL availability changing alters which Home Assistant entities
     * should exist; refresh discovery (kvm_atx_apply above already ran, so the
     * capability is current by now). */
    if (strncmp(key, "atx_", 4) == 0 || strcmp(key, "pwr_wol_mac") == 0) {
        kvm_mqtt_notify();
    }
}

/*
 * Whether the named medium should be served as a CD-ROM rather than a disk.
 * msc_mode is the operator's override: 0 = auto, 1 = force CD-ROM, 2 = force disk.
 * Auto picks CD-ROM for a .iso file and a disk for everything else; the reserved
 * "@" media (rescue, whole card) have no extension, so auto leaves them a disk.
 */
static bool media_is_cdrom(const char *image)
{
    switch (kvm_setting_int("msc_mode")) {
    case 1:
        return true;
    case 2:
        return false;
    default: /* auto */
        break;
    }
    if (image[0] == '@') {
        return false;
    }
    const size_t n = strlen(image);
    return n >= 4 && strcasecmp(image + n - 4, ".iso") == 0;
}

/*
 * Reconcile the virtual drive with the settings: offer the chosen medium to the
 * target when the feature is on and something is named; otherwise show an empty
 * drive. Called at boot and whenever a storage setting changes, so swapping the
 * medium from the console takes effect at once. The device type (CD-ROM vs disk)
 * is handed to the USB layer, which re-attaches only if it actually changed.
 */
static void apply_media_selection(void)
{
    /* Reserved names: "@rescue" is the on-flash image, "@wholesd" the whole card.
     * Any other name is a file on the card. */
    static char prev_image[64] = "";
    const char *image = kvm_setting_str("msc_image");
    const bool want = kvm_setting_bool("msc_enable") && image && image[0];
    const char *effective = want ? image : "";

    /* Coming out of the whole-card passthrough: the target may have written the
     * card, so re-read its filesystem before we touch it again. */
    if (strcmp(prev_image, "@wholesd") == 0 && strcmp(effective, "@wholesd") != 0) {
        kvm_storage_reread();
    }
    snprintf(prev_image, sizeof(prev_image), "%s", effective);

    kvm_storage_status_t sd;
    kvm_storage_status(&sd);
    kvm_rescue_t rescue;
    kvm_storage_rescue_status(&rescue);
    /* Virtual media is available whenever there is something to serve - a card
     * or the built-in rescue image - so it works on a device with no card. */
    kvm_cap_report(KVM_CAP_MSC, sd.mounted || rescue.supported,
                   "no microSD card and no rescue partition");

    esp_err_t err;
    bool cdrom = false;
    if (want && strcmp(image, "@rescue") == 0) {
        cdrom = media_is_cdrom(image);
        err = kvm_storage_media_select_rescue(cdrom);
    } else if (want && strcmp(image, "@wholesd") == 0) {
        err = kvm_storage_media_select_whole_sd();
    } else if (want && sd.mounted) {
        cdrom = media_is_cdrom(image);
        err = kvm_storage_media_select(image, cdrom);
    } else {
        kvm_storage_media_eject();
        usb_hid_msc_set_type(false);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot offer media '%s': %s", image, esp_err_to_name(err));
        kvm_storage_media_eject();
        usb_hid_msc_set_type(false);
        return;
    }
    usb_hid_msc_set_type(cdrom);
}

/*
 * Tell the bootloader the running image works, so it stops arming a rollback to
 * the previous slot. This is only meaningful right after an OTA: the new image
 * boots as PENDING_VERIFY, and the next reset reverts it unless it confirms
 * itself. A normal boot of an already-confirmed image finds nothing to do.
 *
 * The call is deliberately made the moment the recovery path (network + web
 * server) is up, not after the whole device has started. What a rollback
 * protects against is an image you cannot reach to replace; once the web server
 * answers, the device is reachable and re-flashable, so confirming it there is
 * exactly right - and it means a later step that a warm esp_restart() left in a
 * bad state (the USB stack, the capture peripheral) can no longer cost a
 * reachable unit a spurious revert.
 */
static void confirm_ota_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        return;
    }
    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; /* not a freshly-flashed image; nothing awaiting confirmation */
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA image on %s confirmed; rollback cancelled", running->label);
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }
}

static void report_pending_capabilities(void)
{
    apply_media_selection();
    kvm_atx_apply();
    /* Wake-on-LAN needs only the network, which is up by the time anything can
     * ask for it. */
    kvm_cap_report(KVM_CAP_WOL, true, NULL);
    kvm_cap_report(KVM_CAP_AUDIO, false, "audio capture not implemented yet");
    kvm_cap_report(KVM_CAP_NET_STATIC, true, NULL);
    /* HTTPS reports itself from the web server, which knows whether the TLS
     * listener actually came up. */

    const esp_partition_t *ota = esp_ota_get_next_update_partition(NULL);
    kvm_cap_report(KVM_CAP_OTA, ota != NULL, "partition table has no second app slot");

    /* Every capability the MQTT bridge gates its Home Assistant entities on is
     * settled now, so connect (if enabled) with the right entity set. */
    kvm_mqtt_apply();

    /* Bring up whichever VPN backend is enabled. Both run on their own worker
     * tasks (apply only signals them), so a slow or failing connect never blocks
     * boot or the web server. */
    kvm_wg_apply();
    kvm_ts_apply();
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

    /* ATX power control. Touches no GPIO until report_pending_capabilities()
     * runs kvm_atx_apply(); this only builds the worker task and queue, so it
     * is ready before the web server can accept a power command. */
    ESP_ERROR_CHECK(kvm_atx_init());

    /* The active link, read early because it decides whether the microSD may mount
     * (below) - the WiFi co-processor and the card share one SD host controller. */
    kvm_wifi_announce(); /* show the Connection switcher/settings in every mode */
    const int32_t net_mode = kvm_setting_int("net_mode");

    /* The microSD card, if any. A KVM without one is still a KVM, so a missing
     * or unreadable card never holds up start-up.
     *
     * On a board with a WiFi co-processor the P4's single SD host controller is
     * shared between the C6's SDIO link and the microSD slot, so only one may hold
     * it. In a WiFi mode the C6 needs it - mounting the card here would claim the
     * controller and the co-processor's SDIO init would then assert - so skip the
     * microSD entirely. Ethernet mode mounts it normally. (esp-hosted's own eager
     * constructor init is blocked so it never races for the bus; see wifi.c.) */
    if (net_mode == KVM_NET_ETHERNET) {
        ESP_LOGI(TAG, "boot: storage");
        ESP_ERROR_CHECK(kvm_storage_init());
    } else {
        ESP_LOGI(TAG, "boot: storage skipped (WiFi mode; the co-processor holds the SD bus)");
    }

    /*
     * Before the network: the button shares its pin with the Ethernet
     * interface, so this is the only moment it can be read without cost.
     * Holding it now clears a forgotten password.
     */
    kvm_auth_check_reset_button();

    /*
     * The recovery path first: bring up the network and the web server, then
     * confirm the image the moment they answer. Everything past this point can
     * fail or hang without stranding the device - the operator can always reach
     * the console and flash again. This ordering is the fix for an OTA that
     * came up reachable but was rolled back anyway because a later peripheral,
     * left in a bad state by the warm restart, never finished starting.
     */
    /*
     * One link at a time (net_mode): Ethernet, WiFi station, or WiFi AP. Ethernet
     * is the default and the only mode on a board without a co-processor. WiFi's
     * capability is compiled-available so its settings appear even on Ethernet;
     * the C6 is only spun up (which costs a few seconds) when a WiFi mode is
     * actually chosen. If WiFi cannot be reached, the reset button clears the
     * setting back to Ethernet.
     */
    if (net_mode == KVM_NET_ETHERNET) {
        ESP_LOGI(TAG, "boot: ethernet");
        ESP_ERROR_CHECK(ethernet_init());
    } else {
        ESP_LOGI(TAG, "boot: wifi %s (Ethernet left down)",
                 net_mode == KVM_NET_WIFI_AP ? "AP" : "station");
        ESP_ERROR_CHECK(kvm_wifi_init());
    }

    /* MQTT bridge: build its timer/state now; it connects later, from
     * report_pending_capabilities(), once every capability it advertises to
     * Home Assistant has been settled. */
    ESP_ERROR_CHECK(kvm_mqtt_init());

    /* VPN backends share one WireGuard stack and are selected at runtime (enable
     * one). Classic WireGuard tunnel: */
    ESP_ERROR_CHECK(kvm_wg_init());

    /* Native Tailscale client: build its worker and start watching for the
     * network; it joins later, from report_pending_capabilities() / GOT_IP. The
     * bridge lets it hand its tailnet address/name to the TLS layer (so the
     * console certificate is valid over Tailscale) without a component cycle. */
    kvm_ts_set_identity_cb(kvm_tls_set_tailnet);
    ESP_ERROR_CHECK(kvm_ts_init());

    /* Before the web server, which reads published frames. */
    video_frame_store_init();
    ESP_LOGI(TAG, "boot: web server");
    httpd_handle_t httpd = http_server_start();
    if (httpd) {
        confirm_ota_image();
    } else {
        /* No console means no way to replace a bad image except a cable. Leave
         * it unconfirmed so the bootloader can still roll back to what worked. */
        ESP_LOGE(TAG, "web server did not start; leaving the image unconfirmed for rollback");
    }

    /*
     * Now the peripherals a warm esp_restart() does not power-cycle. USB is not
     * fatal: a device that cannot present a keyboard is degraded, but it is
     * still reachable and re-flashable, which is better than aborting into a
     * rollback of an image that is otherwise fine.
     */
    ESP_LOGI(TAG, "boot: usb");
    esp_err_t hid_err = usb_hid_init();
    if (hid_err != ESP_OK) {
        ESP_LOGE(TAG, "usb_hid_init: %s (continuing without HID)", esp_err_to_name(hid_err));
    }

    ESP_LOGI(TAG, "boot: capture");
    capture_start();

    /* Optional status OLED. Starts after capture so the shared I2C bus exists;
     * the task self-detects a panel and does nothing when none is wired. */
    kvm_display_init();

    report_pending_capabilities();
    kvm_caps_log();

    ESP_LOGI(TAG, "ready");
}
