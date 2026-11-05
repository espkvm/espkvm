/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web Push from the device. A browser subscribes in the console and gives us
 * its push service URL and two keys. To notify it, the message is encrypted
 * for that browser (RFC 8291, aes128gcm), signed with our VAPID key (RFC 8292)
 * and POSTed to the URL. The push service wakes the browser, and its service
 * worker shows the notification.
 *
 * The VAPID key is made here on first use and kept in NVS. The subscriptions
 * live in NVS too, in fixed slots, so a waiting event can say which browsers
 * it still has to reach.
 */
#include "kvm_push.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kvm_settings.h"
#include "nvs.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "push";

#define NVS_NS "push"
#define ENDPOINT_MAX 512
#define SUB_NAME_MAX 40
#define HTTP_TIMEOUT_MS 15000
/* One record: the whole message must fit, and push services take 4 KB. */
#define RECORD_SIZE 4096
#define PAYLOAD_MAX 1024
/* How long a VAPID token is good for. Services take up to 24 hours. */
#define JWT_LIFE_S (12 * 3600)
/* Before this the clock was never set. */
#define CLOCK_VALID 1700000000

typedef struct {
    uint8_t used;
    char endpoint[ENDPOINT_MAX];
    uint8_t p256dh[65];
    uint8_t auth[16];
    char name[SUB_NAME_MAX];
    int64_t added;
} sub_t;

static sub_t *s_subs; /* KVM_PUSH_MAX slots, PSRAM, loaded on first use */
static SemaphoreHandle_t s_lock;
static uint8_t s_vapid[32];
static uint8_t s_vapid_pub[65];
static bool s_vapid_ok;
static int64_t s_clock_offset; /* wall clock minus uptime, from a Date header */

/* --- base64url ------------------------------------------------------------ */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < len ? in[i + 2] : 0);
        const int n = len - i >= 3 ? 4 : (int)(len - i) + 1;
        for (int k = 0; k < n; k++) {
            if (o + 1 >= cap) {
                out[o] = '\0';
                return o;
            }
            out[o++] = B64[(v >> (18 - 6 * k)) & 63];
        }
    }
    out[o] = '\0';
    return o;
}

/* Takes both alphabets and no padding, or padding. Returns the length, or -1. */
static int b64_decode(const char *in, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in && *in != '='; in++) {
        int v;
        const char c = *in;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

/* --- crypto --------------------------------------------------------------- */

static bool hmac(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, (size_t)klen * 8);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    mbedtls_svc_key_id_t id;
    if (psa_import_key(&a, key, klen, &id) != PSA_SUCCESS) {
        return false;
    }
    size_t n = 0;
    const psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, mlen, out, 32, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 32;
}

/* HKDF-Expand for one block (up to 32 bytes), which is all this needs. */
static bool hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t len)
{
    uint8_t buf[160];
    if (ilen + 1 > sizeof(buf) || len > 32) {
        return false;
    }
    memcpy(buf, info, ilen);
    buf[ilen] = 1;
    uint8_t t[32];
    if (!hmac(prk, 32, buf, ilen + 1, t)) {
        return false;
    }
    memcpy(out, t, len);
    return true;
}

static mbedtls_svc_key_id_t vapid_key(void)
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&a, s_vapid, sizeof(s_vapid), &id) != PSA_SUCCESS) {
        return MBEDTLS_SVC_KEY_ID_INIT;
    }
    return id;
}

/* The device's VAPID key: read from NVS, or made and saved. Under s_lock. */
static bool vapid_load(void)
{
    if (s_vapid_ok) {
        return true;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    nvs_handle_t h;
    size_t len = sizeof(s_vapid);
    bool have = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    if (have) {
        have = nvs_get_blob(h, "vapid", s_vapid, &len) == ESP_OK && len == sizeof(s_vapid);
        nvs_close(h);
    }
    if (!have) {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&a, 256);
        psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT);
        mbedtls_svc_key_id_t id;
        if (psa_generate_key(&a, &id) != PSA_SUCCESS) {
            return false;
        }
        size_t n = 0;
        const psa_status_t st = psa_export_key(id, s_vapid, sizeof(s_vapid), &n);
        psa_destroy_key(id);
        if (st != PSA_SUCCESS || n != sizeof(s_vapid)) {
            return false;
        }
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "vapid", s_vapid, sizeof(s_vapid));
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "made a new VAPID key");
    }
    mbedtls_svc_key_id_t id = vapid_key();
    size_t n = 0;
    const bool ok = !mbedtls_svc_key_id_is_null(id) &&
                    psa_export_public_key(id, s_vapid_pub, sizeof(s_vapid_pub), &n) == PSA_SUCCESS &&
                    n == sizeof(s_vapid_pub);
    psa_destroy_key(id);
    s_vapid_ok = ok;
    return ok;
}

/*
 * Encrypt one message for one browser (RFC 8291). Writes the whole body:
 * salt, record size, our ephemeral public key, then the sealed record.
 */
static int seal(const sub_t *s, const char *msg, size_t mlen, uint8_t *out, size_t cap)
{
    const size_t need = 16 + 4 + 1 + 65 + mlen + 1 + 16;
    if (mlen + 1 + 16 > RECORD_SIZE || cap < need) {
        return -1;
    }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
    mbedtls_svc_key_id_t eph;
    if (psa_generate_key(&a, &eph) != PSA_SUCCESS) {
        return -1;
    }
    uint8_t as_pub[65], secret[32];
    size_t n1 = 0, n2 = 0;
    const bool ok = psa_export_public_key(eph, as_pub, sizeof(as_pub), &n1) == PSA_SUCCESS && n1 == 65 &&
                    psa_raw_key_agreement(PSA_ALG_ECDH, eph, s->p256dh, 65, secret, sizeof(secret), &n2) ==
                        PSA_SUCCESS &&
                    n2 == 32;
    psa_destroy_key(eph);
    if (!ok) {
        return -1;
    }

    /* IKM from the shared secret and the browser's auth secret. */
    uint8_t prk_key[32], ikm[32];
    uint8_t info[14 + 65 + 65];
    memcpy(info, "WebPush: info\0", 14);
    memcpy(info + 14, s->p256dh, 65);
    memcpy(info + 79, as_pub, 65);
    if (!hmac(s->auth, 16, secret, 32, prk_key) || !hkdf_expand(prk_key, info, sizeof(info), ikm, 32)) {
        return -1;
    }
    /* Content key and nonce from a fresh salt. */
    uint8_t salt[16], prk[32], cek[16], nonce[12];
    if (psa_generate_random(salt, sizeof(salt)) != PSA_SUCCESS || !hmac(salt, 16, ikm, 32, prk) ||
        !hkdf_expand(prk, (const uint8_t *)"Content-Encoding: aes128gcm\0", 28, cek, 16) ||
        !hkdf_expand(prk, (const uint8_t *)"Content-Encoding: nonce\0", 24, nonce, 12)) {
        return -1;
    }

    size_t o = 0;
    memcpy(out, salt, 16);
    o += 16;
    out[o++] = (RECORD_SIZE >> 24) & 0xff;
    out[o++] = (RECORD_SIZE >> 16) & 0xff;
    out[o++] = (RECORD_SIZE >> 8) & 0xff;
    out[o++] = RECORD_SIZE & 0xff;
    out[o++] = 65;
    memcpy(out + o, as_pub, 65);
    o += 65;

    /* The one and last record: the text, then the 0x02 delimiter. */
    uint8_t *plain = malloc(mlen + 1);
    if (!plain) {
        return -1;
    }
    memcpy(plain, msg, mlen);
    plain[mlen] = 2;
    psa_key_attributes_t ka = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&ka, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&ka, 128);
    psa_set_key_algorithm(&ka, PSA_ALG_GCM);
    psa_set_key_usage_flags(&ka, PSA_KEY_USAGE_ENCRYPT);
    mbedtls_svc_key_id_t kid;
    size_t clen = 0;
    bool sealed = psa_import_key(&ka, cek, 16, &kid) == PSA_SUCCESS;
    if (sealed) {
        sealed = psa_aead_encrypt(kid, PSA_ALG_GCM, nonce, 12, NULL, 0, plain, mlen + 1, out + o, cap - o,
                                  &clen) == PSA_SUCCESS;
        psa_destroy_key(kid);
    }
    free(plain);
    return sealed ? (int)(o + clen) : -1;
}

/* --- the clock ------------------------------------------------------------ */

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* "Tue, 04 Nov 2026 21:30:00 GMT" -> seconds, or 0. */
static int64_t parse_http_date(const char *s)
{
    static const char MON[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d, y, hh, mm, ss;
    const char *comma = strchr(s, ',');
    if (!comma || sscanf(comma + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) {
        return 0;
    }
    const char *p = strstr(MON, mon);
    if (!p || (p - MON) % 3) {
        return 0;
    }
    const unsigned m = (unsigned)((p - MON) / 3 + 1);
    return days_from_civil(y, m, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
}

static int64_t now_s(void)
{
    const time_t t = time(NULL);
    if (t > CLOCK_VALID) {
        return t;
    }
    if (s_clock_offset) {
        return s_clock_offset + esp_timer_get_time() / 1000000;
    }
    return 0;
}

typedef struct {
    char date[40];
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data && strcasecmp(e->header_key, "Date") == 0) {
        strlcpy(((resp_t *)e->user_data)->date, e->header_value, sizeof(((resp_t *)e->user_data)->date));
    }
    return ESP_OK;
}

/* No clock: ask the push service for the time, from its Date header. */
static void clock_from(const char *origin)
{
    resp_t r = {0};
    esp_http_client_config_t cfg = {
        .url = origin,
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http,
        .user_data = &r,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return;
    }
    (void)esp_http_client_perform(c);
    esp_http_client_cleanup(c);
    const int64_t t = parse_http_date(r.date);
    if (t > CLOCK_VALID) {
        s_clock_offset = t - esp_timer_get_time() / 1000000;
        ESP_LOGI(TAG, "clock taken from %s", origin);
    }
}

/* --- subscriptions -------------------------------------------------------- */

/*
 * One NVS entry per browser ("s0".."s7"), sized to what it holds: the keys,
 * the name, then the endpoint. One blob for all eight was 5 KB, and NVS needs
 * room for a second copy while it rewrites one: on a full partition the
 * rewrite failed and a removed browser came back after a restart.
 */
typedef struct {
    int64_t added;
    uint8_t p256dh[65];
    uint8_t auth[16];
    char name[SUB_NAME_MAX];
} sub_head_t;

static void slot_key(int i, char out[4])
{
    out[0] = 's';
    out[1] = (char)('0' + i);
    out[2] = '\0';
}

/* Under s_lock. */
static esp_err_t subs_save(int i)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    char key[4];
    slot_key(i, key);
    if (!s_subs[i].used) {
        e = nvs_erase_key(h, key);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
    } else {
        const size_t elen = strlen(s_subs[i].endpoint) + 1;
        uint8_t buf[sizeof(sub_head_t) + ENDPOINT_MAX];
        sub_head_t hd = {.added = s_subs[i].added};
        memcpy(hd.p256dh, s_subs[i].p256dh, sizeof(hd.p256dh));
        memcpy(hd.auth, s_subs[i].auth, sizeof(hd.auth));
        memcpy(hd.name, s_subs[i].name, sizeof(hd.name));
        memcpy(buf, &hd, sizeof(hd));
        memcpy(buf + sizeof(hd), s_subs[i].endpoint, elen);
        e = nvs_set_blob(h, key, buf, sizeof(hd) + elen);
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "could not save browser %d: %s", i, esp_err_to_name(e));
    }
    return e;
}

/* Under s_lock. */
static bool subs_load(void)
{
    if (s_subs) {
        return true;
    }
    s_subs = heap_caps_calloc(KVM_PUSH_MAX, sizeof(sub_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_subs) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return true; /* nothing saved yet */
    }
    /* The old single blob (never released): move it over, then drop it. */
    size_t len = 0;
    if (nvs_get_blob(h, "subs", NULL, &len) == ESP_OK) {
        if (len == KVM_PUSH_MAX * sizeof(sub_t)) {
            nvs_get_blob(h, "subs", s_subs, &len);
        }
        nvs_erase_key(h, "subs");
        nvs_commit(h);
        nvs_close(h);
        for (int i = 0; i < KVM_PUSH_MAX; i++) {
            subs_save(i);
        }
        return true;
    }
    for (int i = 0; i < KVM_PUSH_MAX; i++) {
        char key[4];
        slot_key(i, key);
        uint8_t buf[sizeof(sub_head_t) + ENDPOINT_MAX];
        len = sizeof(buf);
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK || len <= sizeof(sub_head_t)) {
            continue;
        }
        sub_head_t hd;
        memcpy(&hd, buf, sizeof(hd));
        sub_t *s = &s_subs[i];
        s->used = 1;
        s->added = hd.added;
        memcpy(s->p256dh, hd.p256dh, sizeof(s->p256dh));
        memcpy(s->auth, hd.auth, sizeof(s->auth));
        memcpy(s->name, hd.name, sizeof(s->name));
        s->name[sizeof(s->name) - 1] = '\0';
        const size_t elen = len - sizeof(hd);
        memcpy(s->endpoint, buf + sizeof(hd), elen < ENDPOINT_MAX ? elen : ENDPOINT_MAX);
        s->endpoint[ENDPOINT_MAX - 1] = '\0';
    }
    nvs_close(h);
    return true;
}

static void lock(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    if (!s_lock) {
        taskENTER_CRITICAL(&mux);
        if (!s_lock) {
            s_lock = xSemaphoreCreateMutex();
        }
        taskEXIT_CRITICAL(&mux);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* "https://fcm.googleapis.com/fcm/send/..." -> "https://fcm.googleapis.com". */
static void origin_of(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    const size_t n = p ? (size_t)(p - url) : strlen(url);
    snprintf(out, cap, "%.*s", (int)n, url);
}

esp_err_t kvm_push_public_key(char *out, size_t cap)
{
    lock();
    const bool ok = vapid_load();
    if (ok) {
        b64url(s_vapid_pub, sizeof(s_vapid_pub), out, cap);
    }
    unlock();
    return ok ? ESP_OK : ESP_FAIL;
}

int kvm_push_find(const char *endpoint)
{
    int slot = -1;
    lock();
    if (subs_load()) {
        for (int i = 0; i < KVM_PUSH_MAX; i++) {
            if (s_subs[i].used && strcmp(s_subs[i].endpoint, endpoint) == 0) {
                slot = i;
                break;
            }
        }
    }
    unlock();
    return slot;
}

esp_err_t kvm_push_subscribe(const char *endpoint, const char *p256dh, const char *auth, const char *name,
                             int *slot)
{
    if (!endpoint || strlen(endpoint) >= ENDPOINT_MAX ||
        (strncmp(endpoint, "https://", 8) != 0 && strncmp(endpoint, "http://", 7) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    sub_t s = {.used = 1};
    if (!p256dh || !auth || b64_decode(p256dh, s.p256dh, sizeof(s.p256dh)) != 65 || s.p256dh[0] != 4 ||
        b64_decode(auth, s.auth, sizeof(s.auth)) != 16) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s.endpoint, endpoint, sizeof(s.endpoint));
    strlcpy(s.name, name ? name : "", sizeof(s.name));
    s.added = now_s();

    const int known = kvm_push_find(endpoint);
    esp_err_t r = ESP_ERR_NO_MEM;
    lock();
    if (subs_load()) {
        int at = known;
        for (int i = 0; at < 0 && i < KVM_PUSH_MAX; i++) {
            if (!s_subs[i].used) {
                at = i;
            }
        }
        if (at >= 0) {
            s_subs[at] = s;
            r = subs_save(at);
            if (r == ESP_OK) {
                *slot = at;
            } else {
                memset(&s_subs[at], 0, sizeof(sub_t)); /* not kept: not there */
            }
        }
    }
    unlock();
    return r;
}

esp_err_t kvm_push_remove(int slot)
{
    if (slot < 0 || slot >= KVM_PUSH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t r = ESP_ERR_NOT_FOUND;
    if (subs_load() && s_subs[slot].used) {
        memset(&s_subs[slot], 0, sizeof(sub_t));
        r = subs_save(slot);
    }
    unlock();
    return r;
}

uint32_t kvm_push_mask(void)
{
    uint32_t m = 0;
    lock();
    if (subs_load()) {
        for (int i = 0; i < KVM_PUSH_MAX; i++) {
            if (s_subs[i].used) {
                m |= 1u << i;
            }
        }
    }
    unlock();
    return m;
}

size_t kvm_push_list_json(char *out, size_t cap)
{
    cJSON *arr = cJSON_CreateArray();
    lock();
    if (subs_load()) {
        for (int i = 0; i < KVM_PUSH_MAX; i++) {
            if (!s_subs[i].used) {
                continue;
            }
            char host[96];
            origin_of(s_subs[i].endpoint, host, sizeof(host));
            const char *h = strstr(host, "://");
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", i);
            cJSON_AddStringToObject(o, "name", s_subs[i].name);
            cJSON_AddStringToObject(o, "host", h ? h + 3 : host);
            cJSON_AddNumberToObject(o, "added", (double)s_subs[i].added);
            cJSON_AddItemToArray(arr, o);
        }
    }
    unlock();
    const bool ok = cJSON_PrintPreallocated(arr, out, (int)cap, false);
    cJSON_Delete(arr);
    return ok ? strlen(out) : 0;
}

/* --- sending -------------------------------------------------------------- */

typedef enum { SENT, AGAIN, REFUSED, GONE } result_t;

static bool make_jwt(const char *aud, int64_t now, char *out, size_t cap)
{
    char claims[260];
    const int cl = snprintf(claims, sizeof(claims), "{\"aud\":\"%s\",\"exp\":%lld,\"sub\":\"https://espkvm.io\"}",
                            aud, (long long)(now + JWT_LIFE_S));
    if (cl <= 0 || cl >= (int)sizeof(claims)) {
        return false;
    }
    char input[420];
    size_t o = b64url((const uint8_t *)"{\"typ\":\"JWT\",\"alg\":\"ES256\"}", 27, input, sizeof(input));
    input[o++] = '.';
    o += b64url((const uint8_t *)claims, strlen(claims), input + o, sizeof(input) - o);

    mbedtls_svc_key_id_t id = vapid_key();
    if (mbedtls_svc_key_id_is_null(id)) {
        return false;
    }
    uint8_t sig[64];
    size_t n = 0;
    const psa_status_t st =
        psa_sign_message(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), (const uint8_t *)input, o, sig, sizeof(sig), &n);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || n != 64) {
        return false;
    }
    char s64[100];
    b64url(sig, 64, s64, sizeof(s64));
    return snprintf(out, cap, "%s.%s", input, s64) < (int)cap;
}

static result_t send_one(const sub_t *s, const char *msg, size_t mlen)
{
    char origin[160];
    origin_of(s->endpoint, origin, sizeof(origin));
    int64_t now = now_s();
    if (!now) {
        clock_from(origin);
        now = now_s();
        if (!now) {
            return AGAIN; /* no network, most likely */
        }
    }
    const size_t cap = 16 + 4 + 1 + 65 + mlen + 1 + 16;
    uint8_t *body = malloc(cap);
    char *auth = malloc(700);
    if (!body || !auth) {
        free(body);
        free(auth);
        return AGAIN;
    }
    const int blen = seal(s, msg, mlen, body, cap);
    char jwt[560], k[100];
    b64url(s_vapid_pub, sizeof(s_vapid_pub), k, sizeof(k));
    if (blen < 0 || !make_jwt(origin, now, jwt, sizeof(jwt))) {
        free(body);
        free(auth);
        return REFUSED;
    }
    snprintf(auth, 700, "vapid t=%s, k=%s", jwt, k);

    esp_http_client_config_t cfg = {
        .url = s->endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(body);
        free(auth);
        return AGAIN;
    }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Encoding", "aes128gcm");
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(c, "TTL", "86400");
    esp_http_client_set_header(c, "Urgency", "high");
    esp_http_client_set_post_field(c, (const char *)body, blen);
    (void)esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(body);
    free(auth);

    char host[80];
    origin_of(s->endpoint, host, sizeof(host));
    ESP_LOGI(TAG, "%s: %d", host, status);
    if (status >= 200 && status < 300) {
        return SENT;
    }
    if (status == 404 || status == 410) {
        return GONE; /* the browser unsubscribed or the subscription expired */
    }
    if (status >= 400 && status < 500 && status != 408 && status != 429) {
        return REFUSED;
    }
    return AGAIN;
}

bool kvm_push_send(const char *title, const char *body, time_t at, uint32_t *mask)
{
    /* What the service worker shows. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", title ? title : "");
    cJSON_AddStringToObject(o, "body", body ? body : "");
    const char *host = kvm_setting_str("net_hostname");
    cJSON_AddStringToObject(o, "device", host[0] ? host : "espkvm");
    if (at > CLOCK_VALID) {
        cJSON_AddNumberToObject(o, "at", (double)at);
    }
    char msg[PAYLOAD_MAX];
    const bool printed = cJSON_PrintPreallocated(o, msg, sizeof(msg), false);
    cJSON_Delete(o);
    if (!printed) {
        *mask = 0;
        return true;
    }
    const size_t mlen = strlen(msg);

    lock();
    const bool ready = vapid_load() && subs_load();
    unlock();
    /* A copy per send, so the web server is never held up behind a slow push
       service. Off the stack: the notify task's is small. */
    sub_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ready || !s) {
        free(s);
        return false; /* try again later */
    }
    bool refused = false;
    for (int i = 0; i < KVM_PUSH_MAX; i++) {
        if (!(*mask & (1u << i))) {
            continue;
        }
        lock();
        *s = s_subs[i];
        unlock();
        if (!s->used) {
            *mask &= ~(1u << i);
            continue;
        }
        switch (send_one(s, msg, mlen)) {
        case SENT:
            *mask &= ~(1u << i);
            break;
        case GONE:
            ESP_LOGW(TAG, "'%s' is no longer subscribed, removed", s->name);
            lock();
            if (strcmp(s_subs[i].endpoint, s->endpoint) == 0) {
                memset(&s_subs[i], 0, sizeof(sub_t));
                subs_save(i);
            }
            unlock();
            *mask &= ~(1u << i);
            break;
        case REFUSED:
            refused = true;
            *mask &= ~(1u << i);
            break;
        case AGAIN:
            break;
        }
    }
    free(s);
    return refused;
}
