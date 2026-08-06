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

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* TEMPORARY: per-task CPU profiling to find what taxes video FPS. Samples the
 * FreeRTOS run-time counters over a 5 s window and logs each task's share of it
 * (delta, not cumulative-since-boot, so the boot burst does not skew it). Remove
 * once microlink is tuned; the enabling Kconfig lives in sdkconfig.defaults. */
#include "freertos/task.h"
static void cpu_profile_task(void *arg)
{
    (void)arg;
    const UBaseType_t MAXT = 40;
    TaskStatus_t *a = malloc(sizeof(TaskStatus_t) * MAXT);
    TaskStatus_t *b = malloc(sizeof(TaskStatus_t) * MAXT);
    if (!a || !b) {
        free(a);
        free(b);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        uint32_t t0 = 0, t1 = 0;
        UBaseType_t na = uxTaskGetSystemState(a, MAXT, &t0);
        vTaskDelay(pdMS_TO_TICKS(5000));
        UBaseType_t nb = uxTaskGetSystemState(b, MAXT, &t1);
        uint32_t total = t1 - t0;
        if (total == 0) {
            continue;
        }
        ESP_LOGW("cpuprof", "==== per-task CPU over 5s (delta) ====");
        for (UBaseType_t i = 0; i < nb; i++) {
            uint32_t prev = 0;
            for (UBaseType_t j = 0; j < na; j++) {
                if (a[j].xTaskNumber == b[i].xTaskNumber) {
                    prev = a[j].ulRunTimeCounter;
                    break;
                }
            }
            uint32_t d = b[i].ulRunTimeCounter - prev;
            /* total is the reference timer's wall-clock delta; a task pinned to
             * one core at 100% accumulates ~total, so this is % of a single core
             * (0-100 per task, up to 200 summed across the two cores). */
            unsigned pct = (unsigned)(((uint64_t)d * 100) / total);
            if (pct >= 1) {
                ESP_LOGW("cpuprof", "  %-16s %3u%%", b[i].pcTaskName, pct);
            }
        }
    }
}
#endif

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
    kvm_rescue_t rescue;
    kvm_storage_rescue_status(&rescue);
    /* Virtual media is available whenever there is something to serve - a card
     * or the built-in rescue image - so it works on a device with no card. */
    kvm_cap_report(KVM_CAP_MSC, sd.mounted || rescue.supported,
                   "no microSD card and no rescue partition");

    /* The reserved name "@rescue" selects the on-flash image; any other name is
     * a file on the card. Booting from the card is unchanged. */
    const char *image = kvm_setting_str("msc_image");
    const bool want = kvm_setting_bool("msc_enable") && image && image[0];
    esp_err_t err;
    if (want && strcmp(image, "@rescue") == 0) {
        err = kvm_storage_media_select_rescue();
    } else if (want && sd.mounted) {
        err = kvm_storage_media_select(image);
    } else {
        kvm_storage_media_eject();
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot offer media '%s': %s", image, esp_err_to_name(err));
        kvm_storage_media_eject();
    }
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

    /* The microSD card, if any. A KVM without one is still a KVM, so a missing
     * or unreadable card never holds up start-up.
     *
     * On a board with a WiFi co-processor the P4's single SD host controller is
     * shared between the C6's SDIO link and the microSD slot. esp-hosted grabs it
     * at boot; in Ethernet mode WiFi is unused, so release it here so the card can
     * mount. In a WiFi mode the link keeps it and the microSD is unavailable. */
    if (kvm_setting_int("net_mode") == KVM_NET_ETHERNET) {
        kvm_wifi_release_sdio();
    }
    ESP_LOGI(TAG, "boot: storage");
    ESP_ERROR_CHECK(kvm_storage_init());

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
    kvm_wifi_announce(); /* show the Connection switcher/settings in every mode */
    const int32_t net_mode = kvm_setting_int("net_mode");
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

    report_pending_capabilities();
    kvm_caps_log();

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    xTaskCreatePinnedToCore(cpu_profile_task, "cpuprof", 4096, NULL, 1, NULL, 0);
#endif

    ESP_LOGI(TAG, "ready");
}
