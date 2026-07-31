/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * See kvm_wg.h. esp_wireguard brings up a lwIP netif that tunnels over the
 * existing Ethernet link. We deliberately do NOT call esp_wireguard_set_default:
 * only the device's own tunnel address rides WireGuard, so the console stays
 * reachable on the LAN too (split tunnel). The private key is generated on the
 * device (X25519 via PSA) if the operator does not supply one; its public key is
 * reported so it can be added to the peer.
 */
#include "kvm_wg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"

#include "kvm_caps.h"
#include "kvm_settings.h"

#define TAG "kvm_wg"

static SemaphoreHandle_t s_mtx;
/* Reconciling with esp_wireguard (init/connect) can block, so it runs on its own
 * task rather than on whoever changed a setting - a blocked HTTP handler stalls
 * the whole web server. kvm_wg_apply() just nudges this. */
static SemaphoreHandle_t s_apply_sem;
static TaskHandle_t s_task;
static wireguard_ctx_t s_ctx;
static bool s_started;
static bool s_sntp_started;

/* The config strings must outlive the connection: the ctx keeps the pointers. */
static char s_priv[48];
static char s_peer[48];
static char s_addr[40]; /* >= wg_address max_len (31) + NUL */
static char s_mask[16];
static char s_host[64];
static char s_psk[48];
static char s_pubkey[48]; /* our public key, cached for status */

static void wg_task(void *arg);

/* Logged when SNTP sets the clock, so an operator can confirm the time is real:
 * WireGuard reconnects across reboots depend on it (the peer rejects a handshake
 * whose timestamp is not greater than the last, and without a synced clock a
 * reboot rewinds the timestamp). */
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

/* Tear the tunnel down. MUST be called WITHOUT s_mtx held: esp_wireguard_disconnect
 * can block, and holding the lock across it is exactly what wedged kvm_wg_status
 * (hence the whole web server) before. We flip s_started under the lock first so a
 * concurrent status read never touches s_ctx while it is being torn down. */
static void stop_tunnel(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool was = s_started;
    s_started = false;
    xSemaphoreGive(s_mtx);
    if (was) {
        esp_wireguard_disconnect(&s_ctx);
    }
}

esp_err_t kvm_wg_init(void)
{
    if (s_task) {
        return ESP_OK; /* already initialised */
    }
    s_mtx = xSemaphoreCreateMutex();
    s_apply_sem = xSemaphoreCreateBinary();
    if (!s_mtx || !s_apply_sem) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(wg_task, "kvm_wg", 8192, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * Runs only on the wg_task, so it is the single writer of s_ctx / s_started and
 * never races itself. The rule here: the blocking esp_wireguard_* calls run
 * WITHOUT s_mtx held. The lock is taken only for brief state updates (s_pubkey,
 * s_started), so kvm_wg_status - and therefore an HTTP handler asking for it -
 * can never block behind a tunnel bring-up.
 */
static void wg_reconcile(void)
{
    if (!kvm_setting_bool("wg_enable")) {
        stop_tunnel();
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_pubkey[0] = '\0';
        xSemaphoreGive(s_mtx);
        /* "available" describes that it can be configured; the off state shows
         * through the enabled flag in status, like ATX. */
        kvm_cap_report(KVM_CAP_WG, true, NULL);
        return;
    }

    const char *addr = kvm_setting_str("wg_address");
    const char *peer = kvm_setting_str("wg_peer_key");
    const char *endp = kvm_setting_str("wg_endpoint");
    if (!addr[0] || !peer[0] || !endp[0]) {
        stop_tunnel();
        kvm_cap_report(KVM_CAP_WG, false,
                       "set the tunnel address, peer key and endpoint (Settings -> VPN)");
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
            kvm_cap_report(KVM_CAP_WG, false, "could not generate a private key");
            return;
        }
    }

    char pub[48];
    if (derive_public_key(priv, pub, sizeof(pub))) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        snprintf(s_pubkey, sizeof(s_pubkey), "%s", pub);
        xSemaphoreGive(s_mtx);
    } else {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_pubkey[0] = '\0';
        xSemaphoreGive(s_mtx);
        ESP_LOGW(TAG, "could not derive the public key (is the private key valid base64?)");
    }

    /* Endpoint "host:port". */
    char host[64];
    int port = 51820;
    snprintf(host, sizeof(host), "%s", endp);
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        const int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) {
            port = p;
        }
    }

    /* These config strings must outlive the connection (the ctx keeps the
     * pointers). Only the wg_task writes them and only status reads s_addr, so a
     * brief lock around the batch keeps status coherent. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_priv, sizeof(s_priv), "%s", priv);
    snprintf(s_peer, sizeof(s_peer), "%s", peer);
    snprintf(s_addr, sizeof(s_addr), "%s", addr);
    snprintf(s_mask, sizeof(s_mask), "255.255.255.255");
    snprintf(s_host, sizeof(s_host), "%s", host);
    const char *psk = kvm_setting_str("wg_preshared");
    snprintf(s_psk, sizeof(s_psk), "%s", psk ? psk : "");
    xSemaphoreGive(s_mtx);

    stop_tunnel(); /* reconfigure = tear the old one down first (unlocked) */

    wireguard_config_t cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
    cfg.private_key = s_priv;
    cfg.public_key = s_peer;
    cfg.allowed_ip = s_addr;
    cfg.allowed_ip_mask = s_mask;
    cfg.endpoint = s_host;
    cfg.port = port;
    cfg.persistent_keepalive = (int)kvm_setting_int("wg_keepalive");
    if (s_psk[0]) {
        cfg.preshared_key = s_psk;
    }

    /* Blocking bring-up, deliberately UNLOCKED. */
    esp_err_t err = esp_wireguard_init(&cfg, &s_ctx);
    if (err == ESP_OK) {
        err = esp_wireguard_connect(&s_ctx);
    }
    /* No esp_wireguard_set_default(): split tunnel keeps the LAN route. */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wireguard connect: %s", esp_err_to_name(err));
        kvm_cap_report(KVM_CAP_WG, false, "the tunnel could not start (%s)", esp_err_to_name(err));
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
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
    ESP_LOGI(TAG, "WireGuard started: %s via %s:%d", s_addr, s_host, port);
}

/* The reconcile task: waits to be nudged, then applies the current settings.
 * Coalesces bursts (several wg_* keys changing at once) into one reconcile. */
static void wg_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_apply_sem, portMAX_DELAY) == pdTRUE) {
            wg_reconcile();
        }
    }
}

esp_err_t kvm_wg_apply(void)
{
    if (!s_apply_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_apply_sem); /* non-blocking: the task does the slow work */
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
    /* Never block the caller (an HTTP handler) on the tunnel worker. If the lock
     * is momentarily held, report "not up" rather than stalling the web server. */
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        snprintf(out->address, sizeof(out->address), "%s", kvm_setting_str("wg_address"));
        return;
    }
    const bool started = s_started;
    out->enabled = started;
    /* peer_is_up reads s_ctx; only query it while started (s_ctx valid and not
     * being reconfigured, since the worker clears s_started before teardown). */
    out->up = started && esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK;
    snprintf(out->address, sizeof(out->address), "%s", kvm_setting_str("wg_address"));
    snprintf(out->public_key, sizeof(out->public_key), "%s", s_pubkey);
    xSemaphoreGive(s_mtx);
}
