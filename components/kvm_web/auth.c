/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_auth.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp: a header name is case-insensitive and so is its value */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "cJSON.h"

#include "kvm_board.h"
#include "kvm_settings.h"
#include "http_recv.h"
#include "http_server.h" /* kvm_web_security_headers: the auth routes register themselves */
#include "kvm_tls.h"
#include "wifi.h" /* kvm_net_mode_t: AP mode serves the console plain */

#define TAG "auth"

#define NVS_NAMESPACE "kvm_auth"
#define NVS_KEY_SALT "salt"
#define NVS_KEY_HASH "hash"
#define NVS_KEY_ITER "iter"
/* SHA-256 of the viewing token, if one has been made. The token itself is never
   stored: it is shown once, and what is kept only proves a guess wrong. */
#define NVS_KEY_TOKEN "camtok"

#define SALT_LEN 16
#define HASH_LEN 32
#define TOKEN_BYTES 16
#define TOKEN_CHARS (TOKEN_BYTES * 2)

/*
 * Chosen by measurement, and lower than the usual advice: one iteration costs
 * about 80 microseconds here - the PSA hash call, not the SHA itself, is what
 * dominates - so 2500 of them take roughly 200 ms, which is as long as a login
 * can block the web server without feeling broken.
 *
 * What that buys, honestly: it makes guessing through the network pointless
 * (the rate limiter below matters more there), and it makes a dumped flash
 * image inconvenient rather than impossible. A short password would still fall
 * to an offline attack, which is why the minimum length is enforced.
 */
#define PBKDF2_ITERATIONS 2500

/** Until someone sets a password, this one works once and must be changed. */
#define DEFAULT_PASSWORD "admin"

#define COOKIE_NAME "kvm_session"

#define MAX_SESSIONS 4
/** A console left open should not have to log in again mid-shift. */
#define SESSION_TTL_US ((int64_t)12 * 60 * 60 * 1000000)

typedef struct {
    char token[TOKEN_CHARS + 1];
    int64_t expires_us;
    /** The password in use is the default one; nothing else may proceed. */
    bool must_change;
} session_t;

static session_t s_sessions[MAX_SESSIONS];
static SemaphoreHandle_t s_mu;

static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static uint32_t s_iterations;
/** False when no password has ever been set. */
static bool s_have_password;

/* Failed attempts slow every further attempt down. One operator, one counter:
 * a KVM has no legitimate reason to see a burst of logins. */
static uint32_t s_failures;

static void lock(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

/*
 * WebSockets are authenticated once, at the upgrade, and then by socket.
 *
 * Only the upgrade request carries headers; the frames that follow arrive with
 * none, so asking for the cookie again finds nothing and would close a
 * perfectly good connection - which is exactly what it did. The set is small
 * because the device serves one console and a handful of viewers.
 */
#define MAX_WS_SOCKETS 8
static int s_ws_authed[MAX_WS_SOCKETS] = {-1, -1, -1, -1, -1, -1, -1, -1};

void kvm_auth_mark_socket(int fd)
{
    if (fd < 0) {
        return;
    }
    lock();
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (s_ws_authed[i] == fd) {
            unlock();
            return;
        }
    }
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (s_ws_authed[i] < 0) {
            s_ws_authed[i] = fd;
            break;
        }
    }
    unlock();
}

void kvm_auth_forget_socket(int fd)
{
    if (fd < 0) {
        return;
    }
    lock();
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (s_ws_authed[i] == fd) {
            s_ws_authed[i] = -1;
        }
    }
    unlock();
}

bool kvm_auth_socket_ok(int fd)
{
    if (!kvm_auth_required()) {
        return true;
    }
    /* Empty slots hold -1, so a -1 fd (an httpd_req_to_sockfd failure) would
     * otherwise match one and read as authenticated. Never treat it as valid. */
    if (fd < 0) {
        return false;
    }
    bool ok = false;
    lock();
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        if (s_ws_authed[i] == fd) {
            ok = true;
            break;
        }
    }
    unlock();
    return ok;
}

/*
 * PBKDF2-HMAC-SHA256, RFC 8018 section 5.2, written here because mbedTLS 4
 * offers no way to reach one: its own PBKDF2 sits behind a private header, the
 * PSA key derivation for it is not compiled into this build, and the public MD
 * API no longer exposes HMAC. What is left is the hash, so HMAC is assembled
 * on top of it with the padded key blocks computed once.
 *
 * The result is checked against a published vector at start-up; see
 * self_test(). A key derivation that is subtly wrong still returns bytes, and
 * "wrong" here would mean every password is accepted.
 */
#define SHA256_BLOCK 64

typedef struct {
    uint8_t ipad[SHA256_BLOCK];
    uint8_t opad[SHA256_BLOCK];
} hmac_key_t;

static esp_err_t sha256(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len,
                        uint8_t out[HASH_LEN])
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t len = 0;
    psa_status_t ps = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (ps == PSA_SUCCESS && a_len) {
        ps = psa_hash_update(&op, a, a_len);
    }
    if (ps == PSA_SUCCESS && b_len) {
        ps = psa_hash_update(&op, b, b_len);
    }
    if (ps == PSA_SUCCESS) {
        ps = psa_hash_finish(&op, out, HASH_LEN, &len);
    }
    if (ps != PSA_SUCCESS) {
        psa_hash_abort(&op);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** Pre-compute the two padded key blocks HMAC uses for every message. */
static esp_err_t hmac_key_init(hmac_key_t *k, const uint8_t *key, size_t key_len)
{
    uint8_t block[SHA256_BLOCK] = {0};
    if (key_len > SHA256_BLOCK) {
        if (sha256(key, key_len, NULL, 0, block) != ESP_OK) {
            return ESP_FAIL;
        }
    } else {
        memcpy(block, key, key_len);
    }
    for (size_t i = 0; i < SHA256_BLOCK; i++) {
        k->ipad[i] = (uint8_t)(block[i] ^ 0x36);
        k->opad[i] = (uint8_t)(block[i] ^ 0x5c);
    }
    return ESP_OK;
}

static esp_err_t hmac(const hmac_key_t *k, const uint8_t *msg, size_t msg_len,
                      uint8_t out[HASH_LEN])
{
    uint8_t inner[HASH_LEN];
    if (sha256(k->ipad, SHA256_BLOCK, msg, msg_len, inner) != ESP_OK) {
        return ESP_FAIL;
    }
    return sha256(k->opad, SHA256_BLOCK, inner, HASH_LEN, out);
}

esp_err_t kvm_auth_pbkdf2(const char *password, const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t *out, size_t out_len)
{
    if (!password || !salt || !out || out_len == 0 || iterations == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    hmac_key_t key;
    if (hmac_key_init(&key, (const uint8_t *)password, strlen(password)) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    size_t done = 0;
    uint32_t block = 1;
    while (done < out_len && result == ESP_OK) {
        uint8_t seed[SHA256_BLOCK + 4];
        if (salt_len > sizeof(seed) - 4) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        memcpy(seed, salt, salt_len);
        seed[salt_len + 0] = (uint8_t)(block >> 24);
        seed[salt_len + 1] = (uint8_t)(block >> 16);
        seed[salt_len + 2] = (uint8_t)(block >> 8);
        seed[salt_len + 3] = (uint8_t)block;

        uint8_t u[HASH_LEN];
        uint8_t acc[HASH_LEN];
        /* U1 = HMAC(password, salt || INT_32_BE(block)) */
        if (hmac(&key, seed, salt_len + 4, u) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        memcpy(acc, u, HASH_LEN);

        /* Ui = HMAC(password, Ui-1), accumulated by exclusive-or. */
        for (uint32_t i = 1; i < iterations; i++) {
            if (hmac(&key, u, HASH_LEN, u) != ESP_OK) {
                result = ESP_FAIL;
                break;
            }
            for (size_t b = 0; b < HASH_LEN; b++) {
                acc[b] ^= u[b];
            }
        }
        if (result != ESP_OK) {
            break;
        }

        const size_t take = (out_len - done) < HASH_LEN ? (out_len - done) : HASH_LEN;
        memcpy(out + done, acc, take);
        done += take;
        block++;
    }

    memset(&key, 0, sizeof(key));
    return result;
}

/** Constant time, so a wrong password cannot be found one byte at a time. */
static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static esp_err_t store_password(const char *password)
{
    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];
    esp_fill_random(salt, sizeof(salt));

    const int64_t started = esp_timer_get_time();
    esp_err_t err = kvm_auth_pbkdf2(password, salt, sizeof(salt), PBKDF2_ITERATIONS, hash,
                                    sizeof(hash));
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "password hashed in %lld ms", (long long)((esp_timer_get_time() - started) / 1000));

    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY_SALT, salt, sizeof(salt));
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_HASH, hash, sizeof(hash));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, NVS_KEY_ITER, PBKDF2_ITERATIONS);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    lock();
    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_hash, hash, sizeof(hash));
    s_iterations = PBKDF2_ITERATIONS;
    s_have_password = true;
    unlock();
    return ESP_OK;
}

static bool password_matches(const char *password)
{
    if (!s_have_password) {
        /* Nothing stored yet: the device answers to the well-known default once,
         * and the session it hands back is good for changing the password and
         * nothing else. Plain strcmp is fine here - "admin" is public, not a
         * secret, so there is no timing side channel worth defending; the real
         * stored password below is compared in constant time (equal_ct). */
        return strcmp(password, DEFAULT_PASSWORD) == 0;
    }
    uint8_t candidate[HASH_LEN];
    if (kvm_auth_pbkdf2(password, s_salt, SALT_LEN, s_iterations, candidate, sizeof(candidate)) !=
        ESP_OK) {
        return false;
    }
    return equal_ct(candidate, s_hash, HASH_LEN);
}

static void session_clear_all(void)
{
    lock();
    memset(s_sessions, 0, sizeof(s_sessions));
    /* Sockets authenticated under the old password go with them. */
    for (int i = 0; i < MAX_WS_SOCKETS; i++) {
        s_ws_authed[i] = -1;
    }
    unlock();
}

static const char *session_create(bool must_change)
{
    uint8_t raw[TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));

    lock();
    int slot = -1;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0' || s_sessions[i].expires_us < now) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* All slots live: the oldest gives way, so a browser that never logs
         * out cannot lock the operator out of their own device. */
        int64_t oldest = s_sessions[0].expires_us;
        slot = 0;
        for (int i = 1; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].expires_us < oldest) {
                oldest = s_sessions[i].expires_us;
                slot = i;
            }
        }
    }
    for (int i = 0; i < TOKEN_BYTES; i++) {
        snprintf(&s_sessions[slot].token[i * 2], 3, "%02x", raw[i]);
    }
    s_sessions[slot].expires_us = now + SESSION_TTL_US;
    s_sessions[slot].must_change = must_change;
    const char *token = s_sessions[slot].token;
    unlock();
    return token;
}

static session_t *session_find(const char *token)
{
    if (!token || !token[0]) {
        return NULL;
    }
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].token[0] == '\0') {
            continue;
        }
        if (s_sessions[i].expires_us < now) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            continue;
        }
        if (strcmp(s_sessions[i].token, token) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

/** Pull our cookie out of the request's Cookie header. */
static bool cookie_token(httpd_req_t *req, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 512) {
        return false;
    }
    char *header = malloc(len + 1);
    if (!header) {
        return false;
    }
    bool found = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", header, len + 1) == ESP_OK) {
        for (char *p = header; p && *p;) {
            while (*p == ' ' || *p == ';') {
                p++;
            }
            if (strncmp(p, COOKIE_NAME "=", sizeof(COOKIE_NAME)) == 0) {
                p += sizeof(COOKIE_NAME);
                size_t n = strcspn(p, ";");
                if (n < out_len) {
                    memcpy(out, p, n);
                    out[n] = '\0';
                    found = true;
                }
                break;
            }
            p = strchr(p, ';');
        }
    }
    free(header);
    return found;
}

bool kvm_auth_required(void)
{
    return kvm_setting_bool("sec_auth");
}

/* ---- the viewing token ---------------------------------------------------
 *
 * One long random string that opens the picture and nothing else: the MJPEG
 * stream, a single frame, and the capture's figures. It exists because Home
 * Assistant's camera integrations speak a URL and basic auth, and this device
 * speaks a session cookie - so without it the only way to put the target's
 * screen on a dashboard is to turn the login off, which is not a trade anybody
 * should make.
 *
 * What it deliberately cannot do: press a key, move the pointer, touch the
 * power, change a setting, install firmware, or read the screen as text. It is
 * a viewing credential, and a viewing credential is all it is.
 *
 * Kept as a SHA-256 of itself, so a dump of the flash gives an attacker
 * something to compare against rather than something to use. It is shown once,
 * when it is made.
 */
#define TOKEN_VIEW_BYTES 16 /* 128 bits, hex-encoded to 32 characters */

esp_err_t kvm_auth_token_create(char *out, size_t out_len)
{
    if (!out || out_len < TOKEN_VIEW_BYTES * 2 + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[TOKEN_VIEW_BYTES];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(&out[i * 2], 3, "%02x", raw[i]);
    }

    uint8_t digest[32];
    esp_err_t err = sha256((const uint8_t *)out, TOKEN_VIEW_BYTES * 2, NULL, 0, digest);
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY_TOKEN, digest, sizeof(digest));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t kvm_auth_token_revoke(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, NVS_KEY_TOKEN);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* there was nothing to revoke, which is the wanted state */
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool token_digest(uint8_t out[32])
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = 32;
    const esp_err_t err = nvs_get_blob(nvs, NVS_KEY_TOKEN, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && len == 32;
}

bool kvm_auth_token_exists(void)
{
    uint8_t digest[32];
    return token_digest(digest);
}

/** The token from the request: a bearer header, or ?token= for clients that
    can only be given a URL - which is what a camera integration is. */
static bool token_from_request(httpd_req_t *req, char *out, size_t out_len)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncasecmp(hdr, "Bearer ", 7) == 0) {
        strlcpy(out, hdr + 7, out_len);
        return out[0] != '\0';
    }
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "token", out, out_len) == ESP_OK) {
        return out[0] != '\0';
    }
    return false;
}

bool kvm_auth_token_ok(httpd_req_t *req)
{
    uint8_t want[32];
    if (!token_digest(want)) {
        return false; /* no token has been made: nothing to let in */
    }
    char given[80];
    if (!token_from_request(req, given, sizeof(given))) {
        return false;
    }
    uint8_t got[32];
    if (sha256((const uint8_t *)given, strlen(given), NULL, 0, got) != ESP_OK) {
        return false;
    }
    return equal_ct(got, want, sizeof(want));
}

bool kvm_auth_origin_ok(httpd_req_t *req)
{
    char origin[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        /* No Origin at all: a same-origin GET, curl, or Home Assistant. Nothing
           a browser sends across sites is missing this header. */
        return true;
    }
    char host[96];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    const char *slashes = strstr(origin, "://");
    const char *o = slashes ? slashes + 3 : origin;
    /* Host carries the port when it is not the default, and so does Origin, so
       the two compare directly - including the captive portal, where both name
       whatever host the phone was probing. */
    return strcmp(o, host) == 0;
}

/*
 * Is this a state-changing request that our own console made?
 *
 * The session lives in a cookie, so the question a cookie cannot answer by
 * itself is "did the operator mean this" - another site can make a browser send
 * a request with that cookie attached. SameSite=Strict is the first answer and a
 * good one; this is the second, for the browsers and the corner cases where it
 * is not enforced.
 *
 * A form on another site can only send a few content types and cannot add a
 * header of its own, so requiring either JSON or our own header shuts that door
 * without a token to keep in sync. Uploads, which are octet-stream by nature,
 * pass on the header.
 */
static bool intentional_write(httpd_req_t *req)
{
    if (req->method != HTTP_POST && req->method != HTTP_PUT && req->method != HTTP_DELETE) {
        return true;
    }
    if (!kvm_auth_origin_ok(req)) {
        return false;
    }
    char v[64];
    if (httpd_req_get_hdr_value_str(req, KVM_CONSOLE_HEADER, v, sizeof(v)) == ESP_OK) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", v, sizeof(v)) == ESP_OK &&
        strncasecmp(v, "application/json", strlen("application/json")) == 0) {
        return true;
    }
    return false;
}

bool kvm_auth_check(httpd_req_t *req)
{
    if (!intentional_write(req)) {
        ESP_LOGW(TAG, "%s refused: not from this console", req->uri);
        return false;
    }
    if (!kvm_auth_required()) {
        return true;
    }
    char token[TOKEN_CHARS + 1] = {0};
    if (!cookie_token(req, token, sizeof(token))) {
        return false;
    }
    lock();
    const session_t *s = session_find(token);
    const bool ok = s != NULL;
    const bool must_change = ok && s->must_change;
    unlock();
    if (!ok) {
        return false;
    }
    /*
     * While the default password is still in force the session may reach only the
     * auth endpoints - enough to check state and set a real password, nothing
     * that would let the device (or the machine behind it) be driven without one.
     * The console forces the change in its UI; this is the same rule on the wire,
     * so a direct API client - or the video/input WebSocket, whose upgrade also
     * runs through here - cannot skip it.
     */
    if (must_change && strncmp(req->uri, "/api/v1/auth/", 13) != 0) {
        return false;
    }
    return true;
}

esp_err_t kvm_auth_reject_ws(httpd_req_t *req)
{
    /*
     * By the time a frame reaches a handler the upgrade has long been
     * answered, so there is no status line left to send: writing "401" here
     * would push an HTTP body into an open socket and the client would report
     * a malformed frame rather than a refusal. Closing is the honest answer.
     * The upgrade itself is refused earlier, in the pre-handshake callback.
     */
    const int fd = httpd_req_to_sockfd(req);
    ESP_LOGW(TAG, "websocket frame without a session on fd %d: closing", fd);
    if (fd >= 0) {
        (void)httpd_sess_trigger_close(req->handle, fd);
    }
    return ESP_FAIL;
}

esp_err_t kvm_auth_challenge(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "{\"error\":\"authentication required\"}", HTTPD_RESP_USE_STRLEN);
}

/* ---- handlers ---------------------------------------------------------- */

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, (size_t)(req->content_len - got));
        if (kvm_recv_stalled(n) && ++stalls <= KVM_RECV_MAX_STALLS) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        stalls = 0;
        got += n;
    }
    buf[got] = '\0';
    return ESP_OK;
}

/* Copy a required string field into a fixed buffer; reject missing, non-string
 * or over-long (truncation is an error here, not a silent cut). */
static bool json_str_field(const cJSON *root, const char *name, char *out, size_t out_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(it) || it->valuestring == NULL) {
        return false;
    }
    if (strlen(it->valuestring) >= out_len) {
        return false;
    }
    strlcpy(out, it->valuestring, out_len);
    return true;
}

/**
 * Build the Set-Cookie header.
 *
 * The buffer belongs to the caller and must outlive the response, because
 * esp_http_server keeps the pointer it is given and reads it when the headers
 * are finally written. Handing it a local that has gone out of scope reads
 * whatever the stack holds by then - which cost an afternoon to find, so it is
 * written down here.
 */
static void set_session_cookie(httpd_req_t *req, char *buf, size_t len, const char *token,
                               bool clear)
{
    /* Secure only under TLS: a cookie marked Secure is simply not sent back
     * over plain HTTP, which would lock out a device deliberately run without
     * it. HttpOnly and SameSite apply either way. AP (setup hotspot) mode serves
     * the console plain even when sec_https is on (the captive browser can't clear
     * the self-signed cert), so it must not mark the cookie Secure either. */
    const bool ap_mode = (kvm_setting_int("net_mode") == KVM_NET_WIFI_AP);
    const bool tls = kvm_setting_bool("sec_https") && !ap_mode;
    snprintf(buf, len, COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict%s; %s",
             clear ? "" : token, tls ? "; Secure" : "",
             clear ? "Max-Age=0" : "Max-Age=43200");
    httpd_resp_set_hdr(req, "Set-Cookie", buf);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    kvm_web_security_headers(req);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t auth_session_get(httpd_req_t *req)
{
    char token[TOKEN_CHARS + 1] = {0};
    bool authenticated = false;
    bool must_change = false;

    if (cookie_token(req, token, sizeof(token))) {
        lock();
        const session_t *s = session_find(token);
        if (s) {
            authenticated = true;
            must_change = s->must_change;
        }
        unlock();
    }

    char body[224];
    snprintf(body, sizeof(body),
             "{\"required\":%s,\"authenticated\":%s,\"mustChange\":%s,\"user\":\"%s\","
             /* Whether a viewing token exists, never what it is. */
             "\"viewToken\":%s}",
             kvm_auth_required() ? "true" : "false", authenticated ? "true" : "false",
             must_change ? "true" : "false", kvm_setting_str("sec_user"),
             kvm_auth_token_exists() ? "true" : "false");
    return send_json(req, "200 OK", body);
}

static esp_err_t auth_login_post(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"malformed request\"}");
    }
    char user[40] = {0};
    char password[80] = {0};
    cJSON *j = cJSON_Parse(body);
    bool fields_ok = j && json_str_field(j, "user", user, sizeof(user)) &&
                     json_str_field(j, "password", password, sizeof(password));
    cJSON_Delete(j);
    if (!fields_ok) {
        return send_json(req, "400 Bad Request", "{\"error\":\"user and password are required\"}");
    }

    /* The delay grows with the number of failures and is paid before the
     * answer, so guessing costs the guesser time whether or not they are
     * right. */
    lock();
    const uint32_t failures = s_failures;
    unlock();
    if (failures > 3) {
        uint32_t delay_ms = 500u << (failures - 4 > 5 ? 5 : failures - 4);
        if (delay_ms > 15000u) {
            delay_ms = 15000u;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    const bool user_ok = strcmp(user, kvm_setting_str("sec_user")) == 0;
    if (!user_ok || !password_matches(password)) {
        lock();
        const uint32_t count = ++s_failures;
        unlock();
        ESP_LOGW(TAG, "failed login as '%s' (%lu in a row)", user, (unsigned long)count);
        memset(password, 0, sizeof(password));
        return send_json(req, "401 Unauthorized", "{\"error\":\"wrong username or password\"}");
    }
    memset(password, 0, sizeof(password));
    lock();
    s_failures = 0;
    unlock();

    const bool must_change = !s_have_password;
    const char *token = session_create(must_change);
    char cookie[192];
    set_session_cookie(req, cookie, sizeof(cookie), token, false);
    char out[64];
    snprintf(out, sizeof(out), "{\"mustChange\":%s}", must_change ? "true" : "false");
    ESP_LOGI(TAG, "logged in as '%s'%s", user, must_change ? " with the default password" : "");
    return send_json(req, "200 OK", out);
}

static esp_err_t auth_logout_post(httpd_req_t *req)
{
    char token[TOKEN_CHARS + 1] = {0};
    if (cookie_token(req, token, sizeof(token))) {
        lock();
        session_t *s = session_find(token);
        if (s) {
            memset(s, 0, sizeof(*s));
        }
        unlock();
    }
    char cookie[192];
    set_session_cookie(req, cookie, sizeof(cookie), "", true);
    return send_json(req, "200 OK", "{\"status\":\"logged out\"}");
}

static esp_err_t auth_password_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"error\":\"malformed request\"}");
    }
    char current[80] = {0};
    char next[80] = {0};
    cJSON *j = cJSON_Parse(body);
    bool fields_ok = j && json_str_field(j, "current", current, sizeof(current)) &&
                     json_str_field(j, "next", next, sizeof(next));
    cJSON_Delete(j);
    if (!fields_ok) {
        return send_json(req, "400 Bad Request",
                         "{\"error\":\"current and next passwords are required\"}");
    }
    if (strlen(next) < 8) {
        return send_json(req, "400 Bad Request",
                         "{\"error\":\"the new password must be at least 8 characters\"}");
    }
    if (!password_matches(current)) {
        memset(current, 0, sizeof(current));
        memset(next, 0, sizeof(next));
        return send_json(req, "403 Forbidden", "{\"error\":\"the current password is wrong\"}");
    }
    esp_err_t err = store_password(next);
    memset(current, 0, sizeof(current));
    memset(next, 0, sizeof(next));
    if (err != ESP_OK) {
        return send_json(req, "500 Internal Server Error",
                         "{\"error\":\"the new password could not be stored\"}");
    }

    /* Every session was established under the old password. Ending them is
     * the point of changing it. */
    session_clear_all();
    char cookie[192];
    set_session_cookie(req, cookie, sizeof(cookie), "", true);
    ESP_LOGI(TAG, "password changed; all sessions ended");
    return send_json(req, "200 OK", "{\"status\":\"changed\"}");
}

void kvm_auth_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/v1/auth/session", .method = HTTP_GET, .handler = auth_session_get},
        {.uri = "/api/v1/auth/login", .method = HTTP_POST, .handler = auth_login_post},
        {.uri = "/api/v1/auth/logout", .method = HTTP_POST, .handler = auth_logout_post},
        {.uri = "/api/v1/auth/password", .method = HTTP_POST, .handler = auth_password_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

/*
 * The way back in.
 *
 * A password nobody remembers turns a KVM into a brick, and the usual answer -
 * a recovery account, a backdoor token - is a second way in that never sleeps.
 * Physical presence is the honest credential here: whoever can hold the button
 * down can also unplug the machine this device is attached to.
 *
 * The button is read once, in a window at start-up, and then the pin is handed
 * straight back. Two things force that shape on this board:
 *
 *  - GPIO 35 is shared with the Ethernet interface. Claiming it while the
 *    network is up costs the network: the device keeps running and answers
 *    nothing, not even ARP. Found the hard way.
 *  - the PHY drives that line whenever it is out of reset, and a software
 *    restart does not reset it - so after esp_restart() the button cannot be
 *    read at all, however hard it is held. Only a power-on or an EN reset
 *    leaves the line free long enough for this window. Also found the hard
 *    way, while trying to make the reset easier to test.
 *
 * The button is the chip's boot strapping pin too, and this is the part to get
 * right when telling anyone how to use it: HOLD IT AFTER THE RESET, NOT
 * THROUGH IT.
 *
 * The window is polled in software, seconds into start-up, so holding the
 * button through the reset itself buys nothing - and on a board where that pin
 * is the ROM's download strap it costs everything. On the ESP32-P4 Function EV
 * board, BOOT held while RST is pressed is the documented way into
 * firmware-download mode: the ROM stops there, the app never runs, this window
 * never opens, and the panel sits frozen on its last frame looking like a dead
 * device. (Verified on hardware; a plain reset gets back out.) The Waveshare
 * P4-ETH is the board that made this look harmless - there the strapping byte
 * merely goes 0x30f -> 0x20f and it still boots from flash.
 *
 * So: reset, release, then press and hold. Every operator-facing string says
 * it in that order for a reason.
 *
 * What it clears is every setting that can lock the console away: the
 * password, a static address that does not exist on this network, an operator
 * TLS certificate that no longer matches, and a WiFi mode whose network is
 * gone. Nothing else - the credentials and the way in, not the configuration.
 * Wiping the rest would take a device that is merely locked and make it blank,
 * which is the opposite of a recovery.
 */
#define RESET_WINDOW_MS 8000
#define RESET_HOLD_MS 1500
#define RESET_POLL_MS 50

static esp_err_t auth_clear(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        (void)nvs_erase_all(nvs);
        err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    lock();
    memset(s_hash, 0, sizeof(s_hash));
    memset(s_salt, 0, sizeof(s_salt));
    s_have_password = false;
    s_failures = 0;
    memset(s_sessions, 0, sizeof(s_sessions));
    unlock();
    return err;
}

/** Is a password stored at all? Read straight from NVS, because this runs
 *  before the rest of the module is initialised. */
static bool password_stored(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    const bool present = nvs_get_blob(nvs, NVS_KEY_HASH, NULL, &len) == ESP_OK && len == HASH_LEN;
    nvs_close(nvs);
    return present;
}

void kvm_auth_check_reset_button(kvm_auth_reset_ui_cb_t ui)
{
    /* A board with no reachable button (Kconfig set to -1) has no reset. */
    if (KVM_BOARD_BUTTON_GPIO < 0) {
        return;
    }
    /*
     * Nothing to reset, nothing to wait for. This is what keeps the window
     * from costing every boot four seconds: a device that has never been
     * given a password walks straight past it.
     */
    if (!password_stored()) {
        return;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << KVM_BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return;
    }
    /* Let the pull-up settle before believing the first reading. */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Rounded up, not truncated: RESET_HOLD_MS / 1000 printed "1 s" for a
     * 1.5 s hold, which is an instruction to let go too early. */
    ESP_LOGW(TAG, "hold the button now (%d s) to clear the password and restore the network",
             (RESET_HOLD_MS + 999) / 1000);

    int held_ms = 0;
    int shown_pct = -1;
    if (ui) {
        ui(-1, NULL); /* the window is open and nothing is held yet */
    }
    for (int elapsed = 0; elapsed < RESET_WINDOW_MS; elapsed += RESET_POLL_MS) {
        if (gpio_get_level(KVM_BOARD_BUTTON_GPIO) == 0) {
            held_ms += RESET_POLL_MS;
            /* In tenths: a panel redraw costs far more than this loop does, so
             * only say something when the number on it would actually change. */
            const int pct = (held_ms * 100 / RESET_HOLD_MS / 10) * 10;
            if (ui && pct != shown_pct) {
                shown_pct = pct;
                ui(pct > 100 ? 100 : pct, NULL);
            }
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "button held at start-up: clearing the password, reverting to DHCP");
                /* The format string of a log macro has to be a literal. */
                const bool cleared = auth_clear() == ESP_OK;
                bool link_reset = false;
                if (cleared) {
                    ESP_LOGW(TAG, "password cleared - the default one works again");
                } else {
                    ESP_LOGE(TAG, "password could not be cleared");
                }
                /* Also drop back to DHCP, so a wrong static address that made the
                 * device unreachable is recovered the same way a forgotten
                 * password is. This runs before ethernet_init, so it takes effect
                 * on this very boot. */
                if (kvm_setting_set_int("net_dhcp", 1) == ESP_OK) {
                    ESP_LOGW(TAG, "network reverted to DHCP");
                }
                /*
                 * And back onto the wired link. A WiFi network that moved,
                 * changed its passphrase or simply is not there any more locks
                 * the device away exactly as a forgotten password does, and it
                 * is worse than a wrong static address: a WiFi mode holds
                 * Ethernet down, so plugging a cable in does nothing and the
                 * panel just reads "no link". Nothing else can undo it - the
                 * only way to change the setting is the console this is
                 * locking you out of. Two comments and the setting's own help
                 * have promised this since the mode was added; the code did
                 * not do it.
                 */
#if CONFIG_KVM_ETH_ENABLE
                if (kvm_setting_int("net_mode") != KVM_NET_ETHERNET &&
                    kvm_setting_set_int("net_mode", KVM_NET_ETHERNET) == ESP_OK) {
                    ESP_LOGW(TAG, "connection reverted to Ethernet");
                    link_reset = true;
                }
#elif CONFIG_KVM_WIFI
                /* No wired port on this board, so recovering "to Ethernet"
                 * would recover to nothing. The hotspot is the way back in. */
                if (kvm_setting_int("net_mode") != KVM_NET_WIFI_AP &&
                    kvm_setting_set_int("net_mode", KVM_NET_WIFI_AP) == ESP_OK) {
                    ESP_LOGW(TAG, "connection reverted to the hotspot");
                    link_reset = true;
                }
#endif
                /* And drop any operator-supplied TLS certificate: a wrong one
                 * (expired, or a name that no longer matches) is another way to
                 * lock oneself out of the console, recovered here the same way.
                 * The self-signed identity takes over on the next start. */
                if (kvm_tls_byo_present()) {
                    (void)kvm_tls_byo_clear();
                    ESP_LOGW(TAG, "operator TLS certificate cleared");
                }
                /* One line, for a panel the size of a coin: the link is the
                 * surprising half - that the password went back to the default
                 * is what holding the button is for. */
                if (ui) {
                    ui(100, !cleared          ? "reset failed"
                            : link_reset      ? "password + link"
                                              : "password cleared");
                }
                break;
            }
        } else {
            /* Let go early and the gauge empties, which is the feedback that
             * teaches the gesture: it is a hold, not a press. */
            if (ui && shown_pct >= 0) {
                shown_pct = -1;
                ui(-1, NULL);
            }
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(RESET_POLL_MS));
    }

    /* Give the pin back before anything else configures it. */
    gpio_reset_pin(KVM_BOARD_BUTTON_GPIO);
}

/**
 * A published PBKDF2-HMAC-SHA256 vector: password "password", salt "salt",
 * 4096 iterations. A derivation that is subtly wrong still returns bytes, and
 * the failure would look exactly like a correct one until the day someone
 * needs the password to mean something.
 */
static bool self_test(void)
{
    static const uint8_t expected[32] = {
        0xc5, 0xe4, 0x78, 0xd5, 0x92, 0x88, 0xc8, 0x41, 0xaa, 0x53, 0x0d,
        0xb6, 0x84, 0x5c, 0x4c, 0x8d, 0x96, 0x28, 0x93, 0xa0, 0x01, 0xce,
        0x4e, 0x11, 0xa4, 0x96, 0x38, 0x73, 0xaa, 0x98, 0x13, 0x4a,
    };
    uint8_t out[32];
    if (kvm_auth_pbkdf2("password", (const uint8_t *)"salt", 4, 4096, out, sizeof(out)) != ESP_OK) {
        return false;
    }
    return memcmp(out, expected, sizeof(expected)) == 0;
}

esp_err_t kvm_auth_init(void)
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
        if (!s_mu) {
            return ESP_ERR_NO_MEM;
        }
    }

    const int64_t started = esp_timer_get_time();
    if (!self_test()) {
        /* Refusing to run beats accepting every password. */
        ESP_LOGE(TAG, "PBKDF2 self-test failed; authentication is not safe to use");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PBKDF2 self-test passed (4096 iterations in %lld ms)",
             (long long)((esp_timer_get_time() - started) / 1000));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no password set; the default one works until it is changed");
        return ESP_OK;
    }
    size_t salt_len = sizeof(s_salt);
    size_t hash_len = sizeof(s_hash);
    if (nvs_get_blob(nvs, NVS_KEY_SALT, s_salt, &salt_len) == ESP_OK &&
        nvs_get_blob(nvs, NVS_KEY_HASH, s_hash, &hash_len) == ESP_OK &&
        nvs_get_u32(nvs, NVS_KEY_ITER, &s_iterations) == ESP_OK && salt_len == SALT_LEN &&
        hash_len == HASH_LEN && s_iterations > 0) {
        s_have_password = true;
    } else {
        ESP_LOGW(TAG, "no password set; the default one works until it is changed");
    }
    nvs_close(nvs);
    return ESP_OK;
}
