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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "microlink.h"

#include "kvm_caps.h"
#include "kvm_notify.h"
#include "kvm_ipv6.h"
#include "kvm_settings.h"

#define TAG "kvm_ts"

static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_apply_sem;
static TaskHandle_t s_task;
static microlink_t *s_ml;
static bool s_started;
static volatile bool s_net_up;
static volatile bool s_rebind;      /* the device's address changed: move the tailnet to it */
static volatile bool s_logout;      /* forget the keys before the next start */
static volatile bool s_force;       /* restart the session even if nothing changed */
static uint32_t s_local_ip;         /* the address the session started on */
static kvm_ts_identity_cb_t s_identity_cb;
static volatile kvm_ts_c2n_cb_t s_c2n_cb;

static int c2n(const char *method, const char *path, char *body, size_t cap, void *arg)
{
    (void)arg;
    kvm_ts_c2n_cb_t cb = s_c2n_cb;
    return cb ? cb(method, path, body, cap) : 404;
}

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
    } else if (kvm_ipv6_routable(NULL, 0)) {
        /* An IPv6-only network still counts as up. Whether the peer can actually
         * be reached over it is the tunnel's problem to report - refusing to
         * start at all would leave the operator with no error to read. */
        s_net_up = true;
    }
    return s_net_up;
}

/* The default link's IPv4 address now, 0 when it has none. */
static uint32_t local_ip(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_net_up = true;
    /* Ethernet and Wi-Fi can take turns (net_mode "auto"), and a new lease can
     * change the address. The session's sockets and the endpoints the peers
     * know are tied to the old one, so the task moves it over. */
    s_rebind = true;
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
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not watch for GOT_IP: %s", esp_err_to_name(err));
    }
    if (xTaskCreate(ts_task, "kvm_ts", 5120, NULL, 5, &s_task) != pdPASS) {
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

    /* No auth key is fine: the control plane then hands out a login URL and
     * the console shows it (or the device's keys are already approved). */
    const char *auth = kvm_setting_str("ts_auth_key");
    if (!auth) {
        auth = "";
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
    bool tls = kvm_setting_bool("ts_ctrl_tls");
    /* Self-hosted control server (Headscale/Ionscale); empty = hosted Tailscale.
     * Taken as a host, or as a URL: https://hs.example.com:8443/ sets TLS and
     * the port from the URL (after Csontikka cbdf160). */
    char ctrl_buf[64];
    const char *ctrl = kvm_setting_str("ts_control_url");
    uint16_t ctrl_port = (uint16_t)kvm_setting_int("ts_control_port");
    if (ctrl && ctrl[0]) {
        const char *h = ctrl;
        if (strncmp(h, "https://", 8) == 0) {
            tls = true;
            h += 8;
        } else if (strncmp(h, "http://", 7) == 0) {
            tls = false;
            h += 7;
        }
        snprintf(ctrl_buf, sizeof(ctrl_buf), "%s", h);
        char *slash = strchr(ctrl_buf, '/');
        if (slash) {
            *slash = '\0';
        }
        char *colon = strrchr(ctrl_buf, ':');
        if (colon && !strchr(ctrl_buf, ']')) {
            const int p = atoi(colon + 1);
            if (p > 0 && p < 65536) {
                ctrl_port = (uint16_t)p;
            }
            *colon = '\0';
        }
        ctrl = ctrl_buf;
    }

    /* Already joined with these exact parameters? Nothing to do. */
    if (s_started && !s_force && !s_logout && strcmp(auth, s_auth) == 0 && strcmp(name ? name : "", s_name) == 0 &&
        tls == s_tls && strcmp(ctrl ? ctrl : "", s_ctrl_host) == 0 && ctrl_port == s_ctrl_port) {
        kvm_cap_report(KVM_CAP_TS, true, NULL);
        return;
    }

    /* A parameter changed (or first join): drop any old session, then join. */
    stop_session();
    s_force = false;
    if (s_logout) {
        s_logout = false;
        esp_err_t r = microlink_factory_reset();
        ESP_LOGI(TAG, "logged out, keys forgotten: %s", esp_err_to_name(r));
    }

    snprintf(s_auth, sizeof(s_auth), "%s", auth);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_tls = tls;
    snprintf(s_ctrl_host, sizeof(s_ctrl_host), "%s", ctrl ? ctrl : "");
    s_ctrl_port = ctrl_port;

    microlink_config_t cfg = {
        .auth_key = s_auth,
        .device_name = s_name[0] ? s_name : NULL,
        .app_name = "ESP-KVM",
        .app_version = esp_app_get_description()->version,
        .enable_derp = true,
        /* Direct paths. microlink ignores these two flags and always runs
         * STUN and DISCO. The heartbeat has to beat inside the 15 s a direct
         * path is trusted for, or the path drops to DERP every 15 s. */
        .enable_stun = true,
        .enable_disco = true,
        .disco_heartbeat_ms = 5000,
        .stun_interval_ms = 60000,
        /* Reach the hosted control plane, which is HTTPS-only. Operators pointing
         * at a plain-HTTP Headscale can turn this off. */
        .ctrl_tls = tls,
        /* A self-hosted control server (Headscale/Ionscale), or NULL for Tailscale.
         * s_ctrl_host is a static buffer so it outlives the session, like s_auth. */
        .ctrl_host = s_ctrl_host[0] ? s_ctrl_host : NULL,
        .ctrl_port = s_ctrl_port,
        .c2n_handler = c2n,
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
    s_local_ip = local_ip();
    s_rebind = false;
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

/*
 * A tailnet key lasts six months at most, and a device nobody logs into drops
 * off the tailnet the day it lapses. The control plane tells us the date; this
 * says so in good time, once a day, through whatever notifications are set up.
 */
static time_t s_key_warned;

static void check_key_expiry(void)
{
    const int warn_days = (int)kvm_setting_int("ts_key_warn");
    if (warn_days <= 0) {
        return;
    }
    const time_t now = time(NULL);
    if (now < 1735689600) {
        return; /* before 2025: the clock is not set, so no date can be judged */
    }
    if (s_key_warned && now - s_key_warned < 24 * 3600) {
        return;
    }
    kvm_ts_status_t st;
    kvm_ts_status(&st);
    if (!st.enabled || (!st.key_expiry && !st.key_expired)) {
        return;
    }

    char body[192];
    if (st.key_expired) {
        snprintf(body, sizeof(body),
                 "The tailnet key has run out. The device is off the tailnet until it is "
                 "authorised again: sign in under Settings > VPN, or put in a fresh auth key.");
    } else {
        const int64_t left = st.key_expiry - (int64_t)now;
        if (left > (int64_t)warn_days * 86400) {
            return;
        }
        const int days = (int)((left + 86399) / 86400);
        char when[32] = "soon";
        struct tm tm;
        const time_t expiry = (time_t)st.key_expiry;
        if (localtime_r(&expiry, &tm)) {
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        }
        snprintf(body, sizeof(body),
                 "The tailnet key runs out on %s, in %d day%s. Authorise the device again "
                 "before then, or it drops off the tailnet.",
                 when, days, days == 1 ? "" : "s");
    }
    s_key_warned = now;
    ESP_LOGW(TAG, "%s", body);
    kvm_notify_send(KVM_NOTIFY_TSKEY, "Tailscale key", body, false);
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
        if (s_rebind) {
            s_rebind = false;
            const uint32_t now_ip = local_ip();
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            microlink_t *ml = s_started ? s_ml : NULL;
            const bool moved = ml && now_ip && now_ip != s_local_ip;
            if (moved) {
                s_local_ip = now_ip;
            }
            xSemaphoreGive(s_mtx);
            if (moved) {
                ESP_LOGI(TAG, "the device's address changed, moving the tailnet session");
                if (microlink_rebind(ml) != ESP_OK) {
                    /* Not far enough along to move: start over instead. */
                    s_force = true;
                    ts_reconcile();
                }
            }
        }
        sync_tailnet_cert();
        check_key_expiry();
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

void kvm_ts_set_c2n_cb(kvm_ts_c2n_cb_t cb)
{
    s_c2n_cb = cb;
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
        out->key_expiry = microlink_get_key_expiry(ml);
        out->key_expired = microlink_key_expired(ml);
        const uint32_t ip = microlink_get_vpn_ip(ml);
        if (ip) {
            microlink_ip_to_str(ip, out->address);
        }
        snprintf(out->name, sizeof(out->name), "%s", microlink_get_self_name(ml));
        snprintf(out->cert_domain, sizeof(out->cert_domain), "%s", microlink_get_cert_domain(ml));
        snprintf(out->login_url, sizeof(out->login_url), "%s", microlink_get_login_url(ml));
        snprintf(out->error, sizeof(out->error), "%s", microlink_get_register_error(ml));
        microlink_get_derp_home(ml, out->derp);
        out->derp_up = microlink_derp_connected(ml);
    }
    xSemaphoreGive(s_mtx);
}

static void fill_peer(microlink_t *ml, const microlink_peer_info_t *pi, kvm_ts_peer_t *out)
{
    memset(out, 0, sizeof(*out));
    microlink_ip_to_str(pi->vpn_ip, out->ip);
    snprintf(out->name, sizeof(out->name), "%s", pi->hostname);
    snprintf(out->user, sizeof(out->user), "%s", pi->user);
    snprintf(out->user_display, sizeof(out->user_display), "%s", pi->user_display);
    out->online = pi->net_online;
    out->direct = pi->direct_path && pi->endpoint_ip;
    if (out->direct) {
        char ip[16];
        microlink_ip_to_str(pi->endpoint_ip, ip);
        snprintf(out->endpoint, sizeof(out->endpoint), "%s:%u", ip, (unsigned)pi->endpoint_port);
    }
    microlink_get_derp_code(ml, pi->derp_region, out->derp);
    out->rtt_ms = pi->rtt_ms;
    out->rtt_direct = pi->rtt_direct;
}

int kvm_ts_peer_count(void)
{
    int n = 0;
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_started && s_ml) {
            n = microlink_get_peer_count(s_ml);
        }
        xSemaphoreGive(s_mtx);
    }
    return n;
}

bool kvm_ts_peer(int index, kvm_ts_peer_t *out)
{
    bool ok = false;
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        microlink_peer_info_t pi;
        if (s_started && s_ml && microlink_get_peer_info(s_ml, index, &pi) == ESP_OK && pi.online) {
            fill_peer(s_ml, &pi, out);
            ok = true;
        }
        xSemaphoreGive(s_mtx);
    }
    return ok;
}

bool kvm_ts_peer_by_ip(const char *ip, kvm_ts_peer_t *out)
{
    if (!ip || strncmp(ip, "100.", 4) != 0) {
        return false; /* not a tailnet address: skip the lock */
    }
    const int n = kvm_ts_peer_count();
    for (int i = 0; i < n; i++) {
        if (kvm_ts_peer(i, out) && strcmp(out->ip, ip) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t kvm_ts_logout(void)
{
    kvm_setting_set_str("ts_auth_key", "");
    s_logout = true;
    return kvm_ts_apply();
}

esp_err_t kvm_ts_set_txt(const char *name, const char *value, char *err, size_t err_len)
{
    if (!s_mtx) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Held for the whole call, so the client cannot be stopped under it. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t res;
    if (!s_started || !s_ml || !microlink_is_connected(s_ml)) {
        if (err && err_len) {
            snprintf(err, err_len, "Tailscale is not connected");
        }
        res = ESP_ERR_INVALID_STATE;
    } else {
        res = microlink_set_dns(s_ml, name, "TXT", value, 60000, err, err_len);
    }
    xSemaphoreGive(s_mtx);
    return res;
}
