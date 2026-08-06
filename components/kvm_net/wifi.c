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

#include <stdio.h>

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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ethernet.h"
#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_storage.h"

static esp_netif_t *s_netif;
static volatile bool s_up;
static volatile int s_rssi;
static kvm_net_mode_t s_mode = KVM_NET_ETHERNET;
static char s_ssid[33];
static unsigned s_retries;
/* True once esp_wifi is up (a WiFi mode is running), so a scan can reuse it
 * instead of borrowing the SD bus to bring the co-processor up. */
static bool s_wifi_running;

/* ---- network scan (async, so the single web-server task never blocks) ---- */
typedef enum { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE, SCAN_ERROR } scan_state_t;
#define SCAN_MAX_APS 24
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth; /* wifi_auth_mode_t: 0 = open */
} scan_ap_t;
static scan_state_t s_scan_state = SCAN_IDLE;
static scan_ap_t s_scan_aps[SCAN_MAX_APS];
static int s_scan_count;
static SemaphoreHandle_t s_scan_mu;

/*
 * esp-hosted auto-initialises its SDIO transport from a C constructor that runs
 * before app_main, which immediately claims the P4's single SD host controller.
 * On this board that controller is shared with the microSD, so in Ethernet mode
 * (where WiFi is unused) the eager init makes the card unmountable - and it
 * cannot be undone afterwards, because esp_hosted_deinit() races the co-processor's
 * async bring-up and asserts, boot-looping the device.
 *
 * So block the constructor's init with a linker wrap (-Wl,--wrap=esp_hosted_init,
 * set in CMakeLists) and bring the co-processor up ourselves, once, only when a
 * WiFi mode actually needs it. In Ethernet mode it never starts and the microSD
 * has the bus to itself.
 */
int __real_esp_hosted_init(void);
static bool s_hosted_allowed;
int __wrap_esp_hosted_init(void)
{
    if (!s_hosted_allowed) {
        return 0; /* ESP_OK: swallow the constructor's eager init */
    }
    return __real_esp_hosted_init();
}

void kvm_wifi_announce(void)
{
    /* The radio hardware is present on this board; being connected is a runtime
     * state reported separately (kvm_wifi_status), so the console shows the
     * Connection switcher and WiFi settings in every mode. The C6 itself is only
     * initialised when a WiFi mode is chosen (kvm_wifi_init). */
    kvm_cap_report(KVM_CAP_WIFI, true, NULL);
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
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "WiFi disconnected (reason %d), reconnecting", e ? e->reason : -1);
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

    /* The co-processor's constructor init was blocked so Ethernet mode could keep
     * the SD bus; bring it up now, which is the point a WiFi mode needs it. */
    s_hosted_allowed = true;
    int herr = esp_hosted_init();
    if (herr != 0) {
        ESP_LOGW(TAG, "esp_hosted_init failed (%d) - is the C6 present?", herr);
        kvm_cap_report(KVM_CAP_WIFI, false, "WiFi co-processor did not start");
        return ESP_OK;
    }

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
    s_wifi_running = true; /* esp_wifi is up; a scan can reuse it, no bus borrow */

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

static esp_err_t scan_collect(void)
{
    wifi_scan_config_t cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&cfg, true); /* blocking, on the worker task */
    if (err != ESP_OK) {
        return err;
    }
    uint16_t n = SCAN_MAX_APS;
    static wifi_ap_record_t recs[SCAN_MAX_APS]; /* static: too big for the task stack */
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_scan_mu, portMAX_DELAY);
    s_scan_count = (n > SCAN_MAX_APS) ? SCAN_MAX_APS : n;
    for (int i = 0; i < s_scan_count; i++) {
        strlcpy(s_scan_aps[i].ssid, (const char *)recs[i].ssid, sizeof(s_scan_aps[i].ssid));
        s_scan_aps[i].rssi = recs[i].rssi;
        s_scan_aps[i].auth = (uint8_t)recs[i].authmode;
    }
    xSemaphoreGive(s_scan_mu);
    return ESP_OK;
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    esp_err_t err;
    if (s_wifi_running && s_mode == KVM_NET_WIFI_STA) {
        /* A WiFi station is already up - scan on it (harmless if it is
         * associated; the scan briefly hops channels). No co-processor bring-up
         * and no teardown, so nothing to race. */
        (void)esp_wifi_start();
        err = scan_collect();
    } else {
        /*
         * Not scannable here. In Ethernet mode the co-processor is down and the
         * microSD holds the shared SD bus; borrowing it would mean bringing the C6
         * up and tearing it back down, but esp_hosted_deinit() cannot be called
         * safely (it races the async transport init and asserts). In AP mode the
         * radio is a hotspot, not a scanner. Either way, switch to WiFi (station)
         * to scan.
         */
        err = ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_scan_mu, portMAX_DELAY);
    s_scan_state = (err == ESP_OK) ? SCAN_DONE : SCAN_ERROR;
    const int found = s_scan_count;
    xSemaphoreGive(s_scan_mu);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi scan finished: %d networks", found);
    }
    vTaskDelete(NULL);
}

esp_err_t kvm_wifi_scan_start(void)
{
    if (!s_scan_mu) {
        s_scan_mu = xSemaphoreCreateMutex();
        if (!s_scan_mu) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_scan_mu, portMAX_DELAY);
    if (s_scan_state == SCAN_RUNNING) {
        xSemaphoreGive(s_scan_mu);
        return ESP_ERR_INVALID_STATE; /* one at a time */
    }
    s_scan_state = SCAN_RUNNING;
    s_scan_count = 0;
    xSemaphoreGive(s_scan_mu);

    /* A worker, not this caller: the scan takes several seconds (a bus borrow and
     * a co-processor bring-up), and the web server runs on one task. */
    if (xTaskCreate(wifi_scan_task, "wifiscan", 6144, NULL, 5, NULL) != pdPASS) {
        s_scan_state = SCAN_ERROR;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void kvm_wifi_scan_json(char *buf, size_t len)
{
    if (!buf || len < 32) {
        return;
    }
    const char *st = "idle";
    if (s_scan_mu) {
        xSemaphoreTake(s_scan_mu, portMAX_DELAY);
    }
    switch (s_scan_state) {
    case SCAN_RUNNING: st = "scanning"; break;
    case SCAN_DONE: st = "done"; break;
    case SCAN_ERROR: st = "error"; break;
    default: st = "idle"; break;
    }
    int o = snprintf(buf, len, "{\"status\":\"%s\",\"aps\":[", st);
    for (int i = 0; i < s_scan_count && o > 0 && o < (int)len - 80; i++) {
        /* Escape the two characters that would break the JSON string; SSIDs are
         * otherwise printable. */
        char esc[65];
        int e = 0;
        for (const char *p = s_scan_aps[i].ssid; *p && e < (int)sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') {
                esc[e++] = '\\';
            }
            esc[e++] = *p;
        }
        esc[e] = '\0';
        o += snprintf(buf + o, len - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%u}", i ? "," : "",
                      esc, s_scan_aps[i].rssi, s_scan_aps[i].auth);
    }
    if (o > 0 && o < (int)len - 3) {
        snprintf(buf + o, len - o, "]}");
    }
    if (s_scan_mu) {
        xSemaphoreGive(s_scan_mu);
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

esp_err_t kvm_wifi_scan_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void kvm_wifi_scan_json(char *buf, size_t len)
{
    if (buf && len >= 28) {
        snprintf(buf, len, "{\"status\":\"idle\",\"aps\":[]}");
    }
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
