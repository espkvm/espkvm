/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi on boards that carry an ESP32-C6 co-processor. The ESP32-P4 has no radio
 * of its own, so the standard esp_wifi API here is serviced by esp_wifi_remote,
 * which forwards to the C6 over SDIO (esp-hosted). The device uses one link at a
 * time (see main.c): Ethernet, WiFi station, or its own access point.
 */
#include "wifi.h"

#include "esp_log.h"
#include "sdkconfig.h"

__attribute__((unused)) static const char *TAG = "wifi";

#if CONFIG_KVM_WIFI

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "ethernet.h"
#include "kvm_caps.h"
#include "kvm_settings.h"

static esp_netif_t *s_netif;
static volatile bool s_up;
static volatile int s_rssi;
static kvm_net_mode_t s_mode = KVM_NET_ETHERNET;
static char s_ssid[33];
static unsigned s_retries;

void kvm_wifi_announce(void)
{
    /* The radio hardware is present on this board; being connected is a runtime
     * state reported separately (kvm_wifi_status), so the console shows the
     * Connection switcher and WiFi settings in every mode. The C6 itself is only
     * initialised when a WiFi mode is chosen (kvm_wifi_init). */
    kvm_cap_report(KVM_CAP_WIFI, true, NULL);
}

void kvm_wifi_release_sdio(void)
{
    /* esp-hosted's constructor ran before app_main and claimed the P4's single SD
     * host controller for the C6's SDIO link. In Ethernet mode the C6 is unused,
     * so hand the controller back (esp_hosted_deinit tears the SDIO transport down
     * and calls sdmmc_host_deinit) before the microSD tries to claim it. */
    esp_err_t err = esp_hosted_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hosted_deinit: %s (microSD may be unavailable)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "released the C6 SDIO transport for the microSD (Ethernet mode)");
    }
}

static const char *wifi_hostname(void)
{
    const char *h = kvm_setting_str("net_hostname");
    return (h && h[0]) ? h : CONFIG_KVM_MDNS_HOSTNAME;
}

/* The hotspot name: a fixed prefix plus the device's own MAC tail, so two devices
 * making an AP at once do not present the same SSID. */
static void derive_ap_ssid(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    (void)esp_efuse_mac_get_default(mac);
    snprintf(out, len, "ESP-KVM-%02x%02x", mac[4], mac[5]);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = false;
        s_rssi = 0;
        if ((s_retries++ % 20u) == 0u) {
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        }
        esp_wifi_connect();
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    s_up = true;
    s_retries = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    const char *scheme = kvm_setting_bool("sec_https") ? "https" : "http";
    ESP_LOGI(TAG, "WiFi got IP: " IPSTR " - open %s://" IPSTR "/ or %s://%s.local/",
             IP2STR(&e->ip_info.ip), scheme, IP2STR(&e->ip_info.ip), scheme, wifi_hostname());
    /* WiFi is the sole active link when it associates (Ethernet is not started in
     * that mode), so it advertises the console over mDNS itself. */
    kvm_net_advertise(wifi_hostname());
}

static esp_err_t wifi_start_sta(void)
{
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        kvm_cap_report(KVM_CAP_WIFI, false, "WiFi station netif creation failed");
        return ESP_FAIL;
    }
    /* Sent as the DHCP client hostname (option 12) so the router lists the device
     * by name, matching Ethernet. */
    (void)esp_netif_set_hostname(s_netif, wifi_hostname());
    (void)esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);

    const char *ssid = kvm_setting_str("wifi_ssid");
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    const char *pass = kvm_setting_str("wifi_pass");
    if (pass) {
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    }
    /* The threshold is the weakest acceptable auth, not a requirement, so this
     * accepts anything from an open network up. */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_FALSE(esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK, ESP_FAIL, TAG, "sta mode");
    (void)esp_wifi_set_config(WIFI_IF_STA, &wc);
    kvm_cap_report(KVM_CAP_WIFI, true, NULL);
    if (!s_ssid[0]) {
        ESP_LOGW(TAG, "WiFi mode but no SSID set");
        return ESP_OK; /* nothing to join, but the driver is up for a scan */
    }
    ESP_RETURN_ON_FALSE(esp_wifi_start() == ESP_OK, ESP_FAIL, TAG, "sta start");
    ESP_LOGI(TAG, "WiFi joining \"%s\"", s_ssid);
    return ESP_OK;
}

static esp_err_t wifi_start_ap(void)
{
    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) {
        kvm_cap_report(KVM_CAP_WIFI, false, "WiFi AP netif creation failed");
        return ESP_FAIL;
    }
    derive_ap_ssid(s_ssid, sizeof(s_ssid));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    const char *pass = kvm_setting_str("ap_pass");
    if (pass && strlen(pass) >= 8) {
        strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN; /* a too-short password would be rejected */
    }

    ESP_RETURN_ON_FALSE(esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK, ESP_FAIL, TAG, "ap mode");
    (void)esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_RETURN_ON_FALSE(esp_wifi_start() == ESP_OK, ESP_FAIL, TAG, "ap start");
    s_up = true;
    kvm_cap_report(KVM_CAP_WIFI, true, NULL);
    /* The default AP netif runs a DHCP server; the device is at 192.168.4.1. */
    kvm_net_advertise(wifi_hostname());
    ESP_LOGI(TAG, "WiFi hotspot \"%s\" up (%s) - connect and open http://192.168.4.1/", s_ssid,
             ap.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    return ESP_OK;
}

esp_err_t kvm_wifi_init(void)
{
    const int32_t m = kvm_setting_int("net_mode");
    s_mode = (m == KVM_NET_WIFI_AP) ? KVM_NET_WIFI_AP : KVM_NET_WIFI_STA;

    /* esp_netif and the default event loop are not up yet in WiFi mode (Ethernet,
     * which normally creates them, is not started); create them here. Harmless if
     * already present. */
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        /* Most likely the C6 co-processor is not answering over SDIO (missing, or
         * esp-hosted slave firmware absent/incompatible). WiFi is optional, so log
         * and let the device be recovered via the reset button. */
        ESP_LOGW(TAG, "esp_wifi_init failed (%s) - is the C6 esp-hosted firmware up?",
                 esp_err_to_name(err));
        kvm_cap_report(KVM_CAP_WIFI, false, "WiFi co-processor not responding (%s)",
                       esp_err_to_name(err));
        return ESP_OK;
    }

    err = (s_mode == KVM_NET_WIFI_AP) ? wifi_start_ap() : wifi_start_sta();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi %s start failed", s_mode == KVM_NET_WIFI_AP ? "AP" : "station");
    }
    return ESP_OK; /* non-fatal regardless */
}

void kvm_wifi_status(kvm_wifi_status_t *out)
{
    if (!out) {
        return;
    }
    out->mode = s_mode;
    out->up = s_up;
    out->rssi = s_rssi;
    out->ap_clients = 0;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    if (s_mode == KVM_NET_WIFI_AP) {
        wifi_sta_list_t list = {0};
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            out->ap_clients = list.num;
        }
    }
}

#else /* !CONFIG_KVM_WIFI */

esp_err_t kvm_wifi_init(void)
{
    return ESP_OK;
}

void kvm_wifi_announce(void)
{
}

void kvm_wifi_release_sdio(void)
{
}

void kvm_wifi_status(kvm_wifi_status_t *out)
{
    if (out) {
        out->mode = KVM_NET_ETHERNET;
        out->up = false;
        out->rssi = 0;
        out->ap_clients = 0;
        out->ssid[0] = '\0';
    }
}

#endif
