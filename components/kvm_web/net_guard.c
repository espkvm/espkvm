/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The safety net for a device nobody can walk up to (see kvm_net_guard.h).
 *
 * The trial. A settings PUT that changes a link setting keeps the values it
 * replaces, in NVS, so they survive the restart most of these settings need.
 * The trial starts when the change takes effect: at once for the live ones
 * (Tailscale, WireGuard), at the next boot for the rest. It ends well when
 * the device is back - the gateway answers, and the VPN is up where the change
 * touched it or came over it - or when somebody signs in to the console from
 * 30 s after the change on. After 10 minutes without either, the old values go
 * back and the device restarts. Only the first change of a run of them is
 * kept as the way back: that is the one known to have worked.
 *
 * The watchdog. Every minute: is there a working network? A gateway that
 * answers ARP, a VPN that is up, or a signed-in request in the last 10
 * minutes all say yes. With none for long enough, restart. The wait grows
 * with each restart in a row (kept across warm restarts) and starts over once
 * the network has been fine for 30 minutes. A restart is the only cure for a
 * Wi-Fi chip that did not start or went quiet: esp-hosted cannot be torn down
 * and brought up again in place.
 */
#include "kvm_net_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "nvs.h"

#include "ethernet.h"
#include "fw_install.h"
#include "kvm_ipv6.h"
#include "kvm_record.h"
#include "kvm_settings.h"
#include "kvm_ts.h"
#include "kvm_wg.h"
#include "web_priv.h"
#include "wifi.h"

static const char *TAG = "net_guard";

#define NVS_NS "net_trial"
#define KEY_SNAP "snap"   /* the values before the change, a JSON object */
#define KEY_AREAS "areas" /* AREA_* the change touched */
#define KEY_VIA "via"     /* VIA_* the change came over */
#define KEY_REV "rev"     /* what the last revert said */

#define TRIAL_S 600
#define CONFIRM_AFTER_S 30 /* a sign-in sooner may still be the old link's */
#define SETTLE_S 15        /* before the health check counts */
#define SEEN_RECENT_S 600  /* a sign-in this recent: the network works */
#define HEALTHY_RESET_S 1800

enum { AREA_NET = 1, AREA_TS = 2, AREA_WG = 4 };
enum { VIA_TS = 1, VIA_WG = 2 };

/* Settings that decide whether the device can be reached. Not the hostname
 * (mDNS only), not ts_only (it lets go by itself while Tailscale is down), not
 * the hotspot's own settings (whoever changes them is standing next to it). */
static const char *const LINK_KEYS[] = {
    "net_dhcp",       "net_ip",          "net_mask",        "net_gw",      "net_dns",
    "net_mode",       "wifi_ssid",       "wifi_pass",       "wifi2_ssid",  "wifi2_pass",
    "wifi3_ssid",     "wifi3_pass",      "ts_enable",       "ts_auth_key", "ts_hostname",
    "ts_control_url", "ts_control_port", "ts_ctrl_tls",     "wg_enable",   "wg_address",
    "wg_private_key", "wg_peer_key",     "wg_endpoint",
};
#define LINK_KEY_COUNT (sizeof(LINK_KEYS) / sizeof(LINK_KEYS[0]))

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_effect_us;   /* when the change took effect; 0 = no trial */
static int64_t s_deadline_us; /* when it is undone */
static volatile uint8_t s_trial_via; /* the running trial's VIA_* */
static volatile int64_t s_seen_us;     /* the last signed-in request */
static volatile int64_t s_seen_vpn_us; /* the last one over a VPN, during a via trial */
static char s_rev[160];

/* What the PUT in progress changes; the HTTP server runs one request at a time. */
static struct {
    bool changes;
    bool live; /* a change takes effect without a restart */
    uint8_t areas;
    uint8_t via;
    char *snap;
} s_put;

/* The watchdog's step, kept across warm restarts. */
#define WD_MAGIC 0x4e475744u
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t step;
} s_wd;

static const uint16_t WD_WAIT_MIN[] = {2, 5, 15, 30, 60, 120, 240, 360};
#define WD_STEPS (sizeof(WD_WAIT_MIN) / sizeof(WD_WAIT_MIN[0]))
#define WD_FIRST_NORMAL 3 /* 30 minutes; a Wi-Fi chip that never started begins at 2 */

static int area_of(const char *key)
{
    if (strncmp(key, "ts_", 3) == 0) {
        return AREA_TS;
    }
    if (strncmp(key, "wg_", 3) == 0) {
        return AREA_WG;
    }
    return AREA_NET;
}

static void value_str(const kvm_setting_t *d, char *out, size_t cap)
{
    if (d->type == KVM_VT_STR) {
        snprintf(out, cap, "%s", kvm_setting_str(d->key));
    } else {
        snprintf(out, cap, "%ld", (long)kvm_setting_int(d->key));
    }
}

/* ---- the path a request came by ------------------------------------------ */

static bool local_addr(httpd_req_t *req, char *out, size_t cap)
{
    const int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&ss, &len) != 0) {
        return false;
    }
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, cap);
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        /* An IPv4 client on the dual-stack socket arrives as ::ffff:a.b.c.d.
         * Read from the bytes: lwIP prints that prefix in capitals. */
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&ss;
        const uint8_t *b = (const uint8_t *)&s6->sin6_addr;
        static const uint8_t v4map[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, v4map, sizeof(v4map)) == 0) {
            snprintf(out, cap, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        } else {
            inet_ntop(AF_INET6, &s6->sin6_addr, out, cap);
        }
        return true;
    }
    return false;
}

static uint8_t via_of(httpd_req_t *req)
{
    char me[48];
    if (!req || !local_addr(req, me, sizeof(me))) {
        return 0;
    }
    if (strncasecmp(me, "fd7a:115c:a1e0:", 15) == 0) {
        return VIA_TS;
    }
    kvm_ts_status_t ts;
    kvm_ts_status(&ts);
    if (ts.address[0] && strcmp(me, ts.address) == 0) {
        return VIA_TS;
    }
    kvm_wg_status_t wg;
    kvm_wg_status(&wg);
    if (wg.address[0]) {
        char a[sizeof(wg.address)];
        snprintf(a, sizeof(a), "%s", wg.address);
        char *slash = strchr(a, '/');
        if (slash) {
            *slash = '\0';
        }
        if (strcmp(me, a) == 0) {
            return VIA_WG;
        }
    }
    return 0;
}

bool kvm_net_guard_via_tailscale(httpd_req_t *req)
{
    return via_of(req) == VIA_TS;
}

/* ---- health --------------------------------------------------------------- */

/* The interface has an address and its gateway answers ARP. ARP, not ping:
 * every router answers it, and a wrong static address in the wrong subnet
 * gets no answer at all. A stable entry can outlive the gateway by up to the
 * ARP cache age (5 minutes), which is shorter than any wait here. */
static bool gateway_answers(esp_netif_t *nif)
{
    esp_netif_ip_info_t ii;
    if (!nif || esp_netif_get_ip_info(nif, &ii) != ESP_OK || ii.ip.addr == 0) {
        return false;
    }
    if (ii.gw.addr == 0) {
        return true; /* no gateway to ask: a network of its own, take the address */
    }
    struct netif *ln = esp_netif_get_netif_impl(nif);
    if (!ln) {
        return false;
    }
    ip4_addr_t gw;
    gw.addr = ii.gw.addr;
    LOCK_TCPIP_CORE();
    (void)etharp_request(ln, &gw);
    UNLOCK_TCPIP_CORE();
    vTaskDelay(pdMS_TO_TICKS(1500));
    struct eth_addr *ea = NULL;
    const ip4_addr_t *ia = NULL;
    LOCK_TCPIP_CORE();
    const ssize_t idx = etharp_find_addr(ln, &gw, &ea, &ia);
    UNLOCK_TCPIP_CORE();
    return idx >= 0;
}

static bool net_ok(void)
{
    kvm_wifi_status_t w;
    kvm_wifi_status(&w);
    if (w.mode == KVM_NET_WIFI_AP) {
        return w.up; /* its own hotspot: nothing further to reach */
    }
    if (gateway_answers(esp_netif_get_default_netif())) {
        return true;
    }
    return kvm_ipv6_routable(NULL, 0); /* an IPv6-only network */
}

static bool ts_up(void)
{
    if (!kvm_setting_bool("ts_enable")) {
        return false;
    }
    kvm_ts_status_t st;
    kvm_ts_status(&st);
    return st.up;
}

static bool wg_up(void)
{
    if (!kvm_setting_bool("wg_enable")) {
        return false;
    }
    kvm_wg_status_t st;
    kvm_wg_status(&st);
    return st.up;
}

static bool seen_since(int64_t t)
{
    return s_seen_us && s_seen_us >= t;
}

static bool seen_vpn_since(int64_t t)
{
    return s_seen_vpn_us && s_seen_vpn_us >= t;
}

/* Back where the change can be undone from: on the network, and on a VPN if
 * the change came over one (any VPN: moving from Tailscale to WireGuard over
 * Tailscale is fine once WireGuard is up), or on each VPN it touched. */
static bool trial_healthy(uint8_t areas, uint8_t via)
{
    if (!net_ok()) {
        return false;
    }
    if (via) {
        return ts_up() || wg_up();
    }
    if ((areas & AREA_TS) && kvm_setting_bool("ts_enable") && !ts_up()) {
        return false;
    }
    if ((areas & AREA_WG) && kvm_setting_bool("wg_enable") && !wg_up()) {
        return false;
    }
    return true;
}

/* ---- the trial ------------------------------------------------------------ */

static void trial_arm(uint8_t via)
{
    s_trial_via = via;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_effect_us = now;
    s_deadline_us = now + (int64_t)TRIAL_S * 1000000;
    portEXIT_CRITICAL(&s_lock);
}

static void trial_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_effect_us = 0;
    s_deadline_us = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void nvs_forget_trial(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, KEY_SNAP);
        (void)nvs_erase_key(h, KEY_AREAS);
        (void)nvs_erase_key(h, KEY_VIA);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

void kvm_net_guard_forget(void)
{
    nvs_forget_trial();
    trial_clear();
}

/* The stored snapshot, malloc'd, or NULL. */
static char *snap_load(uint8_t *areas, uint8_t *via)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    char *snap = NULL;
    if (nvs_get_str(h, KEY_SNAP, NULL, &len) == ESP_OK && len > 1) {
        snap = malloc(len);
        if (snap && nvs_get_str(h, KEY_SNAP, snap, &len) != ESP_OK) {
            free(snap);
            snap = NULL;
        }
    }
    uint8_t a = 0, v = 0;
    (void)nvs_get_u8(h, KEY_AREAS, &a);
    (void)nvs_get_u8(h, KEY_VIA, &v);
    nvs_close(h);
    if (areas) {
        *areas = a;
    }
    if (via) {
        *via = v;
    }
    return snap;
}

void kvm_net_guard_before_put(const char *json, httpd_req_t *req)
{
    free(s_put.snap);
    memset(&s_put, 0, sizeof(s_put));
    if (!json || !kvm_setting_bool("net_trial")) {
        return;
    }
    cJSON *in = cJSON_Parse(json);
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        return;
    }
    cJSON *snap = cJSON_CreateObject();
    for (size_t i = 0; i < LINK_KEY_COUNT && snap; i++) {
        const kvm_setting_t *d = kvm_setting_find(LINK_KEYS[i]);
        if (!d) {
            continue;
        }
        char cur[160];
        value_str(d, cur, sizeof(cur));
        cJSON_AddStringToObject(snap, d->key, cur);

        const cJSON *v = cJSON_GetObjectItemCaseSensitive(in, d->key);
        char next[160];
        if (cJSON_IsString(v)) {
            snprintf(next, sizeof(next), "%s", v->valuestring);
        } else if (cJSON_IsBool(v)) {
            snprintf(next, sizeof(next), "%d", cJSON_IsTrue(v) ? 1 : 0);
        } else if (cJSON_IsNumber(v)) {
            snprintf(next, sizeof(next), "%ld", (long)v->valuedouble);
        } else {
            continue;
        }
        if (strcmp(cur, next) == 0) {
            continue;
        }
        s_put.changes = true;
        s_put.areas |= (uint8_t)area_of(d->key);
        if (!(d->flags & KVM_SF_REBOOT)) {
            s_put.live = true;
        }
    }
    cJSON_Delete(in);
    if (s_put.changes && snap) {
        s_put.snap = cJSON_PrintUnformatted(snap);
        s_put.via = via_of(req);
    }
    cJSON_Delete(snap);
    if (s_put.changes && !s_put.snap) {
        s_put.changes = false; /* no memory to keep the way back: no trial either */
    }
}

void kvm_net_guard_after_put(bool applied)
{
    if (!applied || !s_put.changes) {
        free(s_put.snap);
        memset(&s_put, 0, sizeof(s_put));
        return;
    }
    uint8_t areas = 0, via = 0;
    char *old = snap_load(&areas, &via);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (!old) {
            err = nvs_set_str(h, KEY_SNAP, s_put.snap); /* the first change keeps the way back */
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_AREAS, areas | s_put.areas);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_VIA, via | s_put.via);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(old);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not keep the old network settings (%s); this change is not on trial",
                 esp_err_to_name(err));
    } else if (s_put.live) {
        trial_arm(via | s_put.via);
        ESP_LOGW(TAG, "network change on trial for %d s%s", TRIAL_S,
                 s_put.via == VIA_TS   ? " (made over Tailscale)"
                 : s_put.via == VIA_WG ? " (made over WireGuard)"
                                       : "");
    } else {
        ESP_LOGW(TAG, "network change goes on trial for %d s after the restart", TRIAL_S);
    }
    free(s_put.snap);
    memset(&s_put, 0, sizeof(s_put));
}

static void trial_keep(const char *why)
{
    nvs_forget_trial();
    /* A change that worked: the note about an older revert is history now. */
    nvs_handle_t h;
    if (s_rev[0] && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, KEY_REV);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    s_rev[0] = '\0';
    trial_clear();
    ESP_LOGW(TAG, "network change kept: %s", why);
}

static void trial_revert(uint8_t areas)
{
    char *snap = snap_load(NULL, NULL);
    cJSON *o = snap ? cJSON_Parse(snap) : NULL;
    free(snap);
    /* Forgotten first: whatever happens next, the device must not come back
     * up and revert again. */
    nvs_forget_trial();
    trial_clear();
    int put_back = 0;
    for (const cJSON *it = o ? o->child : NULL; it; it = it->next) {
        const kvm_setting_t *d = it->string ? kvm_setting_find(it->string) : NULL;
        if (!d || !cJSON_IsString(it)) {
            continue;
        }
        char cur[160];
        value_str(d, cur, sizeof(cur));
        if (strcmp(cur, it->valuestring) == 0) {
            continue;
        }
        const esp_err_t err = d->type == KVM_VT_STR ? kvm_setting_set_str(d->key, it->valuestring)
                                                    : kvm_setting_set_int(d->key, atoi(it->valuestring));
        if (err == ESP_OK) {
            put_back++;
        } else {
            ESP_LOGE(TAG, "could not put %s back: %s", d->key, esp_err_to_name(err));
        }
    }
    cJSON_Delete(o);
    char what[48] = "";
    static const struct {
        uint8_t bit;
        const char *name;
    } names[] = {{AREA_NET, "network"}, {AREA_TS, "Tailscale"}, {AREA_WG, "WireGuard"}};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (areas & names[i].bit) {
            snprintf(what + strlen(what), sizeof(what) - strlen(what), "%s%s", what[0] ? ", " : "",
                     names[i].name);
        }
    }
    snprintf(s_rev, sizeof(s_rev), "Old %s settings put back: not on the network within %d minutes",
             what[0] ? what : "network", TRIAL_S / 60);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_str(h, KEY_REV, s_rev);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGE(TAG, "%s (%d values); restarting", s_rev, put_back);
    /* Always restart: most of these settings need it, and Tailscale and
     * WireGuard share one stack that does not like changing hands live. */
    kvm_web_restart_soon(1000);
}

static void trial_tick(void)
{
    int64_t effect, deadline;
    portENTER_CRITICAL(&s_lock);
    effect = s_effect_us;
    deadline = s_deadline_us;
    portEXIT_CRITICAL(&s_lock);
    if (!effect) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    uint8_t areas = 0, via = 0;
    char *snap = snap_load(&areas, &via);
    if (!snap) {
        trial_clear(); /* forgotten elsewhere (a settings reset) */
        return;
    }
    free(snap);
    /* A change made over a VPN is kept by a sign-in over a VPN: a LAN client
     * polling the API proves nothing to somebody on the other side of the world. */
    const int64_t after = effect + (int64_t)CONFIRM_AFTER_S * 1000000;
    if (via ? seen_vpn_since(after) : seen_since(after)) {
        trial_keep("the console was opened");
        return;
    }
    if (now - effect >= (int64_t)SETTLE_S * 1000000 && trial_healthy(areas, via)) {
        trial_keep("the device is back on the network");
        return;
    }
    if (now >= deadline) {
        trial_revert(areas);
    }
}

uint32_t kvm_net_guard_trial_left(void)
{
    int64_t deadline;
    portENTER_CRITICAL(&s_lock);
    deadline = s_effect_us ? s_deadline_us : 0;
    portEXIT_CRITICAL(&s_lock);
    if (!deadline) {
        return 0;
    }
    const int64_t left = deadline - esp_timer_get_time();
    return left > 0 ? (uint32_t)((left + 999999) / 1000000) : 1;
}

const char *kvm_net_guard_last_revert(void)
{
    return s_rev;
}

void kvm_net_guard_seen(httpd_req_t *req)
{
    const int64_t now = esp_timer_get_time();
    s_seen_us = now;
    if (s_trial_via && s_effect_us && via_of(req)) {
        s_seen_vpn_us = now; /* only worked out while it matters */
    }
}

/* ---- the watchdog --------------------------------------------------------- */

/* Whether a restart could help at all. Not for a cable that is out (a restart
 * will not plug it in) or the device's own hotspot (whoever uses it is next to
 * it), unless the Wi-Fi chip never came up. */
static bool wd_applies(void)
{
    if (!kvm_setting_bool("net_watchdog")) {
        return false;
    }
    kvm_wifi_status_t w;
    kvm_wifi_status(&w);
    if (kvm_wifi_coproc_failed()) {
        if (w.mode != KVM_NET_AUTO) {
            return true;
        }
    }
    if (w.mode == KVM_NET_WIFI_AP) {
        return false;
    }
    if (w.mode == KVM_NET_ETHERNET) {
        bool up = false;
        int mbps = 0;
        kvm_eth_link(&up, &mbps);
        return up;
    }
    return true; /* a Wi-Fi station, or Ethernet with Wi-Fi as the backup */
}

static bool wd_healthy(void)
{
    const int64_t now = esp_timer_get_time();
    if (seen_since(now - (int64_t)SEEN_RECENT_S * 1000000)) {
        return true;
    }
    return ts_up() || wg_up() || net_ok();
}

static bool wd_must_wait(void)
{
    kvm_record_status_t rec;
    kvm_record_status(&rec);
    return rec.recording || fw_install_busy() || kvm_net_guard_trial_left() > 0;
}

static void guard_task(void *arg)
{
    (void)arg;
    int64_t bad_since = 0;
    int64_t good_since = esp_timer_get_time();
    int64_t next_wd = esp_timer_get_time() + 60LL * 1000000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        trial_tick();

        const int64_t now = esp_timer_get_time();
        if (now < next_wd) {
            continue;
        }
        next_wd = now + 60LL * 1000000;
        if (!wd_applies()) {
            bad_since = 0;
            continue;
        }
        if (wd_healthy()) {
            if (bad_since) {
                ESP_LOGI(TAG, "network back after %lld s", (long long)((now - bad_since) / 1000000));
                good_since = now;
            }
            bad_since = 0;
            if (s_wd.step && now - good_since >= (int64_t)HEALTHY_RESET_S * 1000000) {
                s_wd.step = 0;
            }
            continue;
        }
        if (!bad_since) {
            bad_since = now;
            ESP_LOGW(TAG, "no working network");
        }
        const uint32_t first = kvm_wifi_coproc_failed() ? 0 : WD_FIRST_NORMAL;
        uint32_t i = first + s_wd.step;
        if (i >= WD_STEPS) {
            i = WD_STEPS - 1;
        }
        const int64_t wait = (int64_t)WD_WAIT_MIN[i] * 60 * 1000000;
        if (now - bad_since < wait || wd_must_wait()) {
            continue;
        }
        s_wd.step++;
        ESP_LOGE(TAG, "no working network for %u minutes; restarting (restart %u in a row)",
                 (unsigned)WD_WAIT_MIN[i], (unsigned)s_wd.step);
        kvm_web_restart_soon(1000);
        bad_since = 0;
        next_wd = now + 3600LL * 1000000; /* the restart is on its way */
    }
}

void kvm_net_guard_start(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;

    if (s_wd.magic != WD_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        s_wd.magic = WD_MAGIC;
        s_wd.step = 0;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_rev);
        if (nvs_get_str(h, KEY_REV, s_rev, &len) != ESP_OK) {
            s_rev[0] = '\0';
        }
        nvs_close(h);
    }

    char *snap = snap_load(NULL, NULL);
    if (snap) {
        free(snap);
        uint8_t via = 0;
        free(snap_load(NULL, &via));
        if (kvm_setting_bool("net_trial")) {
            trial_arm(via);
            ESP_LOGW(TAG, "network change on trial for %d s", TRIAL_S);
        } else {
            nvs_forget_trial();
        }
    }

    if (xTaskCreate(guard_task, "net_guard", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the network guard");
    }
}
