/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_wg.h. A classic single-peer WireGuard client, built on the same
 * bundled wireguard_lwip stack that the Tailscale client (kvm_ts) uses - so the
 * two share one WireGuard implementation and can both live in the firmware,
 * selectable at runtime, instead of colliding as two separate stacks.
 *
 * This is the "classic" mode of wireguard_lwip: an internal UDP socket bound to
 * a listen port sends encrypted traffic straight to the peer's endpoint (no
 * DERP/magicsock, which kvm_ts opts into). Split tunnel: the WireGuard netif is
 * not made the default route, so only the tunnel subnet goes through it and the
 * console stays reachable on the LAN. The private key is generated on the device
 * (X25519 via PSA) if the operator does not supply one.
 *
 * Every wireguard_lwip / lwIP call here runs from the worker task, never from an
 * lwIP callback, so it is wrapped in the TCP/IP core lock (enabled in sdkconfig).
 */
#include "kvm_wg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "wireguardif.h"

#include "kvm_caps.h"
#include "kvm_ipv6.h"
#include "kvm_settings.h"

#define TAG "kvm_wg"
#define WG_LISTEN_PORT 51820

static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_apply_sem;
static TaskHandle_t s_task;

/* Owned by the worker task. The netif is heap-allocated and spliced into lwIP's
 * list by wireguardif_init; s_started gates status reads and the periodic pump. */
static struct netif *s_netif;
static uint8_t s_peer_idx = WIREGUARDIF_INVALID_INDEX;
static bool s_started;
static bool s_sntp_started;

/* Config strings must outlive the interface: wireguardif copies what it needs at
 * add-peer time, but the private key string is held by the init data during
 * init. Keep our own copies and only status reads them, under the lock. */
static char s_priv[48];
static char s_pubkey[48]; /* our public key, cached for status */
static char s_addr[40];

static void wg_task(void *arg);
static void stop_tunnel_netif(struct netif *netif);

/* All wireguard_lwip calls here are from the worker task (never an lwIP
 * callback), so an unconditional core lock is safe and never re-enters. */
static inline void wg_lock(void) { LOCK_TCPIP_CORE(); }
static inline void wg_unlock(void) { UNLOCK_TCPIP_CORE(); }

/* True once the interface has an address. Like kvm_ts, we both catch GOT_IP and
 * check the live netif, so a config change after boot still sees the network. */
static volatile bool s_net_up;

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

/* The tunnel bring-up resolves the peer endpoint, which needs DNS/DHCP up. At
 * cold boot that is not ready yet, so wait for the link and re-nudge the worker
 * when the IP arrives - otherwise a DNS-name endpoint fails to resolve once and
 * the tunnel never comes up (a literal-IP endpoint happened to dodge this). */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_net_up = true;
    kvm_wg_apply();
}

/* See kvm_wg.h. Logged when SNTP sets the clock: a WireGuard reconnect across a
 * reboot needs a real time or the peer rejects a rewound handshake timestamp. */
static void on_time_synced(struct timeval *tv)
{
    const time_t now = tv ? tv->tv_sec : 0;
    struct tm t;
    gmtime_r(&now, &t);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    ESP_LOGI(TAG, "clock synced over SNTP: %s UTC", buf);
}

/* ---- keys (X25519 via PSA) ---------------------------------------------- */

static bool b64_encode(const uint8_t *raw, size_t n, char *out, size_t out_n)
{
    size_t olen = 0;
    return mbedtls_base64_encode((unsigned char *)out, out_n, &olen, raw, n) == 0;
}

/** Generate a Curve25519 private key, returned base64 in @p out. */
static bool gen_private_key(char *out, size_t out_n)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
        return false;
    }
    uint8_t raw[32];
    size_t rn = 0;
    const bool ok = psa_export_key(id, raw, sizeof(raw), &rn) == PSA_SUCCESS && rn == 32 &&
                    b64_encode(raw, 32, out, out_n);
    psa_destroy_key(id);
    return ok;
}

/** Derive the base64 public key from a base64 private key. */
static bool derive_public_key(const char *priv_b64, char *out, size_t out_n)
{
    if (psa_crypto_init() != PSA_SUCCESS || !priv_b64 || !priv_b64[0]) {
        return false;
    }
    uint8_t raw[32];
    size_t rn = 0;
    if (mbedtls_base64_decode(raw, sizeof(raw), &rn, (const unsigned char *)priv_b64,
                              strlen(priv_b64)) != 0 ||
        rn != 32) {
        return false;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attr, raw, rn, &id) != PSA_SUCCESS) {
        return false;
    }
    uint8_t pub[32];
    size_t pn = 0;
    const bool ok = psa_export_public_key(id, pub, sizeof(pub), &pn) == PSA_SUCCESS && pn == 32 &&
                    b64_encode(pub, 32, out, out_n);
    psa_destroy_key(id);
    return ok;
}

/* ---- lifecycle ----------------------------------------------------------- */

/* Tear the tunnel down. Flips s_started under the lock first so a concurrent
 * status read never touches the netif while it is being removed, then does the
 * blocking teardown under the core lock. */
static void stop_tunnel(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool was = s_started;
    struct netif *netif = s_netif;
    s_started = false;
    s_netif = NULL;
    s_peer_idx = WIREGUARDIF_INVALID_INDEX;
    xSemaphoreGive(s_mtx);
    if (was && netif) {
        wg_lock();
        wireguardif_shutdown(netif);
        netif_set_link_down(netif);
        netif_set_down(netif);
        netif_remove(netif);
        wg_unlock();
        free(netif);
    }
}

esp_err_t kvm_wg_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutex();
    s_apply_sem = xSemaphoreCreateBinary();
    if (!s_mtx || !s_apply_sem) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ev = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    if (ev != ESP_OK) {
        ESP_LOGW(TAG, "could not watch for GOT_IP: %s", esp_err_to_name(ev));
    }
    if (xTaskCreate(wg_task, "kvm_wg", 8192, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Resolve "host" (a name or a literal address) to an IPv4 ip_addr_t. */
static bool resolve_ipv4(const char *host, ip_addr_t *out)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    if (lwip_getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return false;
    }
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    ip_addr_set_ip4_u32(out, sa->sin_addr.s_addr);
    lwip_freeaddrinfo(res);
    return true;
}

/*
 * Runs only on the worker task. Blocking wireguard_lwip calls hold the core lock;
 * s_mtx is taken only for brief state updates so kvm_wg_status never blocks behind
 * a bring-up.
 */
static void wg_reconcile(void)
{
    if (!kvm_setting_bool("wg_enable")) {
        stop_tunnel();
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_pubkey[0] = '\0';
        xSemaphoreGive(s_mtx);
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    const char *addr = kvm_setting_str("wg_address");
    const char *peer = kvm_setting_str("wg_peer_key");
    const char *endp = kvm_setting_str("wg_endpoint");
    if (!addr[0] || !peer[0] || !endp[0]) {
        stop_tunnel();
        /* Enabled but not fully configured: a state, not a lost capability. Keep
         * the cap AVAILABLE so the console leaves the wg_* fields editable - gating
         * them on this cap would disable the very inputs needed to finish the
         * config. "Not connected" is reported through kvm_wg_status, not here. */
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    /* Private key: generate and store one on first use. */
    const char *priv = kvm_setting_str("wg_private_key");
    if (!priv[0]) {
        char gen[48];
        if (gen_private_key(gen, sizeof(gen)) &&
            kvm_setting_set_str("wg_private_key", gen) == ESP_OK) {
            ESP_LOGI(TAG, "generated a WireGuard private key");
            priv = kvm_setting_str("wg_private_key");
        } else {
            ESP_LOGE(TAG, "could not generate a WireGuard private key");
            kvm_cap_report(KVM_CAP_WG, true, NULL);
            return;
        }
    }

    char pub[48];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (derive_public_key(priv, pub, sizeof(pub))) {
        snprintf(s_pubkey, sizeof(s_pubkey), "%s", pub);
    } else {
        /* Do not leave a stale key reported: it would no longer match the private
         * key and the operator would register the wrong peer on the hub. */
        ESP_LOGW(TAG, "could not derive the WireGuard public key");
        s_pubkey[0] = '\0';
    }
    xSemaphoreGive(s_mtx);

    /* Wait for the link before resolving the endpoint. At cold boot DHCP/DNS is
     * not ready, so a DNS-name endpoint would fail to resolve once and, with the
     * worker then idle, never retry. on_got_ip() nudges us when the IP lands, and
     * wg_task also retries periodically. The public key is derived above first, so
     * it is available to copy onto the hub even before the link is up. */
    if (!net_is_up()) {
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    /* Endpoint "host:port"; the port defaults to the standard WireGuard port
     * when the endpoint omits it. */
    char host[64];
    int port = WG_LISTEN_PORT;
    snprintf(host, sizeof(host), "%s", endp);
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        const int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) {
            port = p;
        }
    }
    ip_addr_t endpoint_ip;
    if (!resolve_ipv4(host, &endpoint_ip)) {
        /* Runtime/config errors below keep the cap AVAILABLE (fields stay editable
         * so a wrong value can be corrected); they log and show as "not connected". */
        ESP_LOGW(TAG, "WireGuard endpoint '%s' could not be resolved", host);
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    /* Tunnel address, and the subnet that routes through WireGuard. Without a
     * separate mask setting a /24 is assumed - enough to reach the hub and other
     * spokes while leaving the LAN (and the console) on the default route. */
    ip_addr_t tun_ip;
    if (!resolve_ipv4(addr, &tun_ip)) {
        ESP_LOGW(TAG, "WireGuard tunnel address '%s' is not valid", addr);
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    stop_tunnel(); /* reconfigure = tear the old one down first */

    snprintf(s_priv, sizeof(s_priv), "%s", priv);
    snprintf(s_addr, sizeof(s_addr), "%s", addr);

    struct netif *netif = calloc(1, sizeof(struct netif));
    if (!netif) {
        ESP_LOGE(TAG, "out of memory bringing up the WireGuard interface");
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    struct wireguardif_init_data init = {
        .private_key = s_priv,
        .listen_port = WG_LISTEN_PORT,
        .bind_netif = NULL,
    };
    struct wireguardif_peer wgpeer;
    wireguardif_peer_init(&wgpeer);
    wgpeer.public_key = peer;
    wgpeer.preshared_key = NULL;
    /* Accept any tunnel-side address from the hub; outbound routing is bounded by
     * the netif subnet below, so this stays a split tunnel. */
    ip_addr_set_any(false, &wgpeer.allowed_ip);
    ip_addr_set_any(false, &wgpeer.allowed_mask);
    wgpeer.endpoint_ip = endpoint_ip;
    wgpeer.endport_port = (u16_t)port;
    wgpeer.keep_alive = (u16_t)kvm_setting_int("wg_keepalive");

    wg_lock();
    netif->state = &init;
    err_t err = wireguardif_init(netif);
    if (err != ERR_OK) {
        wg_unlock();
        ESP_LOGE(TAG, "wireguardif_init: %d", err);
        free(netif);
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }
    /* /24 tunnel subnet, split route (not the default netif). */
    ip4_addr_t ip4, mask, gw;
    ip4_addr_copy(ip4, *ip_2_ip4(&tun_ip));
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    netif_set_addr(netif, &ip4, &mask, &gw);
    netif_set_up(netif);
    netif_set_link_up(netif);

    uint8_t idx = WIREGUARDIF_INVALID_INDEX;
    err = wireguardif_add_peer(netif, &wgpeer, &idx);
    if (err == ERR_OK && idx != WIREGUARDIF_INVALID_INDEX) {
        err = wireguardif_connect(netif, idx);
    }
    wg_unlock();

    if (err != ERR_OK || idx == WIREGUARDIF_INVALID_INDEX) {
        ESP_LOGE(TAG, "wireguardif peer/connect failed: %d", err);
        stop_tunnel_netif(netif);
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_netif = netif;
    s_peer_idx = idx;
    s_started = true;
    xSemaphoreGive(s_mtx);

    /* Optional SNTP so the handshake timestamp survives a reboot. Started once. */
    if (kvm_setting_bool("wg_sntp") && !s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, kvm_setting_str("wg_sntp_srv"));
        esp_sntp_set_time_sync_notification_cb(on_time_synced);
        esp_sntp_init();
        s_sntp_started = true;
    }

    kvm_cap_report(KVM_CAP_WG, true, NULL);
    ESP_LOGI(TAG, "WireGuard started: %s via %s:%d", s_addr, host, port);
}

/* Remove a half-brought-up netif (add_peer/connect failed after init). */
static void stop_tunnel_netif(struct netif *netif)
{
    wg_lock();
    wireguardif_shutdown(netif);
    netif_set_down(netif);
    netif_remove(netif);
    wg_unlock();
    free(netif);
}

static void wg_task(void *arg)
{
    (void)arg;
    for (;;) {
        /*
         * While up: wake every second to drive WireGuard's handshake retries and
         * keepalive. While down but enabled: wake every few seconds to retry the
         * bring-up, so a transient failure (network not up yet, endpoint not yet
         * resolvable, a failed connect) recovers on its own instead of waiting for
         * the operator to re-save a setting. While disabled: sleep until nudged.
         */
        const bool want = kvm_setting_bool("wg_enable");
        TickType_t wait;
        if (s_started) {
            wait = pdMS_TO_TICKS(1000);
        } else if (want) {
            wait = pdMS_TO_TICKS(5000);
        } else {
            wait = portMAX_DELAY;
        }
        if (xSemaphoreTake(s_apply_sem, wait) == pdTRUE) {
            wg_reconcile(); /* nudged by a settings change or GOT_IP */
        } else if (!s_started && want) {
            wg_reconcile(); /* periodic retry while it should be up but is not */
        }
        if (s_started && s_netif) {
            wg_lock();
            wireguardif_periodic(s_netif);
            wg_unlock();
        }
    }
}

esp_err_t kvm_wg_apply(void)
{
    if (!s_apply_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_apply_sem);
    return ESP_OK;
}

void kvm_wg_status(kvm_wg_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        snprintf(out->address, sizeof(out->address), "%s", kvm_setting_str("wg_address"));
        return;
    }
    const bool started = s_started;
    out->enabled = started;
    if (started && s_netif && s_peer_idx != WIREGUARDIF_INVALID_INDEX) {
        wg_lock();
        out->up = wireguardif_peer_is_up(s_netif, s_peer_idx, NULL, NULL) == ERR_OK;
        wg_unlock();
    }
    snprintf(out->address, sizeof(out->address), "%s", kvm_setting_str("wg_address"));
    snprintf(out->public_key, sizeof(out->public_key), "%s", s_pubkey);
    xSemaphoreGive(s_mtx);
}
