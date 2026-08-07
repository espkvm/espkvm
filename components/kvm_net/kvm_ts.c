/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_ts.h. microlink is a native Tailscale (ts2021) client: it registers
 * with the control plane over a Noise channel, relays through DERP, and punches
 * direct WireGuard paths to peers with DISCO/STUN. We drive it entirely from the
 * ts_* settings - microlink's own config web server (CONFIG_ML_ENABLE_CONFIG_HTTPD)
 * stays off so it never collides with the KVM console. Bring-up is deferred until
 * the Ethernet link has an IP, since microlink expects the network to be up.
 *
 * Like kvm_wg, the blocking work (init/start/stop can each take seconds) runs on
 * a dedicated worker task; kvm_ts_apply() only nudges it, and kvm_ts_status()
 * never blocks behind it, so an HTTP handler asking for status cannot stall.
 */
#include "kvm_ts.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "microlink.h"

#include "kvm_caps.h"
#include "kvm_settings.h"

#define TAG "kvm_ts"

static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_apply_sem;
static TaskHandle_t s_task;
static microlink_t *s_ml;
static bool s_started;
static volatile bool s_net_up;
static kvm_ts_identity_cb_t s_identity_cb;

/* The config strings must outlive the session: microlink_init copies the config
 * struct but keeps the pointers, so the auth key and hostname it uses are these
 * buffers. They double as the snapshot we compare against to decide whether a
 * settings change actually needs a costly rejoin. */
static char s_auth[64];
static char s_name[64];
static bool s_tls;
static char s_ctrl_host[64]; /* self-hosted control server (Headscale); "" = Tailscale */
static uint16_t s_ctrl_port;

static void ts_task(void *arg);

/* True once the interface has an address. We both catch the GOT_IP event and
 * check the live netif, so a config change that arrives after boot (when the
 * event is long past) still sees the network as up. */
static bool net_is_up(void)
{
    if (s_net_up) {
        return true;
    }
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        s_net_up = true;
    }
    return s_net_up;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_net_up = true;
    kvm_ts_apply(); /* nudge: settings may have wanted the tailnet before IP */
}

/* Tear the session down. Flip s_started under the lock first so a concurrent
 * status read never touches s_ml while it is being destroyed, then do the slow
 * stop/destroy unlocked. */
static void stop_session(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool was = s_started;
    s_started = false;
    microlink_t *ml = s_ml;
    s_ml = NULL;
    xSemaphoreGive(s_mtx);
    if (was && ml) {
        microlink_stop(ml);
        microlink_destroy(ml);
    }
}

esp_err_t kvm_ts_init(void)
{
    if (s_task) {
        return ESP_OK; /* already initialised */
    }
    s_mtx = xSemaphoreCreateMutex();
    s_apply_sem = xSemaphoreCreateBinary();
    if (!s_mtx || !s_apply_sem) {
        return ESP_ERR_NO_MEM;
    }
    /* microlink must not start before the link has an address; a second GOT_IP
     * handler alongside the one in ethernet.c is fine (esp_event fans out). */
    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not watch for GOT_IP: %s", esp_err_to_name(err));
    }
    if (xTaskCreate(ts_task, "kvm_ts", 8192, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * Runs only on ts_task, the single writer of s_ml / s_started. The blocking
 * microlink_* calls run WITHOUT s_mtx held; the lock is taken only for brief
 * pointer/flag swaps so kvm_ts_status never blocks behind a bring-up.
 */
static void ts_reconcile(void)
{
    if (!kvm_setting_bool("ts_enable")) {
        stop_session();
        kvm_cap_report(KVM_CAP_TS, true, NULL); /* available, just off */
        return;
    }

    const char *auth = kvm_setting_str("ts_auth_key");
    if (!auth || !auth[0]) {
        stop_session();
        /* Enabled but no key yet: this is a state, not a lost capability. The cap
         * stays AVAILABLE so the console keeps the auth-key field (and the enable
         * toggle) editable - gating them on this cap is what would otherwise wedge
         * the user, since the very field they need to fix it would be disabled.
         * "Not connected" is surfaced through kvm_ts_status, not here. */
        kvm_cap_report(KVM_CAP_TS, true, NULL);
        return;
    }

    if (!net_is_up()) {
        /* Wait for the Ethernet IP; on_got_ip() will nudge us again. */
        kvm_cap_report(KVM_CAP_TS, true, NULL);
        return;
    }

    /* Hostname on the tailnet: the dedicated setting, else the mDNS hostname. */
    const char *name = kvm_setting_str("ts_hostname");
    if (!name || !name[0]) {
        name = kvm_setting_str("net_hostname");
    }
    const bool tls = kvm_setting_bool("ts_ctrl_tls");
    /* Self-hosted control server (Headscale/Ionscale); empty = hosted Tailscale. */
    const char *ctrl = kvm_setting_str("ts_control_url");
    const uint16_t ctrl_port = (uint16_t)kvm_setting_int("ts_control_port");

    /* Already joined with these exact parameters? Nothing to do. */
    if (s_started && strcmp(auth, s_auth) == 0 && strcmp(name ? name : "", s_name) == 0 &&
        tls == s_tls && strcmp(ctrl ? ctrl : "", s_ctrl_host) == 0 && ctrl_port == s_ctrl_port) {
        kvm_cap_report(KVM_CAP_TS, true, NULL);
        return;
    }

    /* A parameter changed (or first join): drop any old session, then join. */
    stop_session();

    snprintf(s_auth, sizeof(s_auth), "%s", auth);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_tls = tls;
    snprintf(s_ctrl_host, sizeof(s_ctrl_host), "%s", ctrl ? ctrl : "");
    s_ctrl_port = ctrl_port;

    microlink_config_t cfg = {
        .auth_key = s_auth,
        .device_name = s_name[0] ? s_name : NULL,
        .enable_derp = true,
        /* DISCO/STUN NAT-traversal is deliberately OFF: its constant probing of
         * every tailnet peer burns CPU this device needs for real-time video
         * capture and encoding. We relay through DERP instead - higher latency,
         * but a KVM's control channel is low-bandwidth and this keeps the video
         * pipeline fed. The heartbeat/stun intervals below are a backstop in case
         * a build re-enables them. */
        .enable_stun = false,
        .enable_disco = false,
        .disco_heartbeat_ms = 30000,
        .stun_interval_ms = 60000,
        /* Reach the hosted control plane, which is HTTPS-only. Operators pointing
         * at a plain-HTTP Headscale can turn this off. */
        .ctrl_tls = tls,
        /* A self-hosted control server (Headscale/Ionscale), or NULL for Tailscale.
         * s_ctrl_host is a static buffer so it outlives the session, like s_auth. */
        .ctrl_host = s_ctrl_host[0] ? s_ctrl_host : NULL,
        .ctrl_port = s_ctrl_port,
    };

    /* A failed init/start is a runtime error to log and retry on the next apply,
     * not a lost capability: keep the cap AVAILABLE so the settings stay editable
     * (e.g. to correct a bad key). The failure shows up as "not up" in the status. */
    microlink_t *ml = microlink_init(&cfg);
    if (!ml) {
        ESP_LOGE(TAG, "microlink_init failed");
        kvm_cap_report(KVM_CAP_TS, true, NULL);
        return;
    }
    esp_err_t err = microlink_start(ml);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start: %s", esp_err_to_name(err));
        microlink_destroy(ml);
        kvm_cap_report(KVM_CAP_TS, true, NULL);
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_ml = ml;
    s_started = true;
    xSemaphoreGive(s_mtx);

    kvm_cap_report(KVM_CAP_TS, true, NULL);
    ESP_LOGI(TAG, "Tailscale client started as '%s' (control plane over %s)",
             s_name[0] ? s_name : "(default hostname)", tls ? "TLS" : "plain TCP");
}

/* Once the tailnet is up, teach the TLS layer our 100.x address and MagicDNS name
 * so the console's certificate is valid when reached over Tailscale. The cert is
 * only applied at boot, so if this is new information the device restarts once to
 * re-issue it; thereafter the stored values match and nothing happens. Guarded so
 * a single boot can trigger at most one restart. */
static bool s_ts_cert_synced;

static void sync_tailnet_cert(void)
{
    if (s_ts_cert_synced) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    microlink_t *ml = s_started ? s_ml : NULL;
    xSemaphoreGive(s_mtx);
    if (!ml || !microlink_is_connected(ml)) {
        return;
    }
    const uint32_t ip = microlink_get_vpn_ip(ml);
    if (!ip) {
        return; /* not assigned yet */
    }
    char ip_str[24];
    microlink_ip_to_str(ip, ip_str);
    const char *fqdn = microlink_get_self_name(ml); /* "" if MagicDNS is off */

    s_ts_cert_synced = true; /* attempt once per boot */
    /* The TLS layer lives in another component that already depends on this one,
     * so it is reached through a callback the composition root registers rather
     * than a direct call (which would be a dependency cycle). */
    if (s_identity_cb && s_identity_cb(ip_str, fqdn)) {
        ESP_LOGW(TAG, "tailnet identity learned (%s / %s); restarting to re-issue the "
                      "TLS certificate so it is valid over Tailscale",
                 ip_str, fqdn[0] ? fqdn : "no-name");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static void ts_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Wake on a settings change, or every few seconds to catch the tailnet
         * address/name appearing after the client connects. On the periodic wake,
         * if Tailscale is enabled but not up, re-run the reconcile: a transient
         * init/start failure (e.g. the control plane briefly unreachable) would
         * otherwise leave it down until the operator re-saves a setting. */
        if (xSemaphoreTake(s_apply_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
            ts_reconcile();
        } else if (kvm_setting_bool("ts_enable") && !s_started) {
            ts_reconcile();
        }
        sync_tailnet_cert();
    }
}

esp_err_t kvm_ts_apply(void)
{
    if (!s_apply_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_apply_sem); /* non-blocking: the task does the slow work */
    return ESP_OK;
}

void kvm_ts_set_identity_cb(kvm_ts_identity_cb_t cb)
{
    s_identity_cb = cb;
}

void kvm_ts_status(kvm_ts_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mtx) {
        return;
    }
    /* Never block the caller (an HTTP handler). If the worker holds the lock mid
     * bring-up, report "not up" rather than stalling the web server. */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    microlink_t *ml = s_ml;
    out->enabled = s_started;
    if (s_started && ml) {
        out->up = microlink_is_connected(ml);
        out->peers = microlink_get_peer_count(ml);
        const uint32_t ip = microlink_get_vpn_ip(ml);
        if (ip) {
            microlink_ip_to_str(ip, out->address);
        }
    }
    xSemaphoreGive(s_mtx);
}
