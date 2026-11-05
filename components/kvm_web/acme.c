/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ACME (RFC 8555) client for one name: the device's Tailscale name, answered
 * with DNS-01 through the control plane's set-dns. See kvm_acme.h.
 */
#include "kvm_acme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "mbedtls/psa_util.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "kvm_settings.h"
#include "kvm_ts.h"

static const char *TAG = "acme";

#define DIR_PROD "https://acme-v02.api.letsencrypt.org/directory"
#define DIR_STAGING "https://acme-staging-v02.api.letsencrypt.org/directory"

#define NVS_NS "acme"
#define RESP_MAX (12 * 1024)
#define URL_MAX 256

/* ---- the certificate being served -------------------------------------- */

typedef struct {
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    char name[128];
    time_t not_before, not_after;
    bool staging;
} acme_pair_t;

/* Read by the TLS handshake without a lock: a new pair is swapped in whole, and
 * the old one is only freed at the swap after that, weeks later. */
static acme_pair_t *volatile s_cur;
static acme_pair_t *s_retired;

/* ---- state shown in the console ---------------------------------------- */

static volatile bool s_busy;
static char s_step[64] = "";
static char s_error[160] = "";
static time_t s_last_try;
static time_t s_next_try;
static int s_fails;

static void set_step(const char *step)
{
    snprintf(s_step, sizeof(s_step), "%s", step);
    ESP_LOGI(TAG, "%s", step);
}

/* ---- small helpers ------------------------------------------------------ */

static time_t x509_time(const mbedtls_x509_time *t)
{
    /* Days from 1970-01-01 to y-m-d (proleptic Gregorian), no time zone. */
    int y = t->year, m = t->mon;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)t->day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (time_t)(days * 86400 + t->hour * 3600 + t->min * 60 + t->sec);
}

static void b64url(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 4 < cap; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        if (i + 1 < len) {
            out[o++] = tbl[(v >> 6) & 63];
        }
        if (i + 2 < len) {
            out[o++] = tbl[v & 63];
        }
    }
    out[o] = '\0';
}

static char *b64url_alloc(const uint8_t *in, size_t len)
{
    const size_t cap = (len + 2) / 3 * 4 + 1;
    char *out = malloc(cap);
    if (out) {
        b64url(in, len, out, cap);
    }
    return out;
}

static bool sha256(const void *in, size_t len, uint8_t out[32])
{
    size_t n = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, in, len, out, 32, &n) == PSA_SUCCESS && n == 32;
}

static esp_err_t new_ec_key(mbedtls_pk_context *pk)
{
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
                                       PSA_KEY_USAGE_EXPORT);
    if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    const int r = mbedtls_pk_copy_from_psa(id, pk);
    (void)psa_destroy_key(id);
    return r == 0 ? ESP_OK : ESP_FAIL;
}

static char *nvs_get_str_alloc(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len < 2) {
        return NULL;
    }
    char *s = malloc(len);
    if (s && nvs_get_str(h, key, s, &len) != ESP_OK) {
        free(s);
        s = NULL;
    }
    return s;
}

/* The chain is over the 4000-byte string limit, so it goes in as a blob. */
static char *nvs_get_blob_str(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) != ESP_OK || len < 2) {
        return NULL;
    }
    char *s = malloc(len + 1);
    if (s && nvs_get_blob(h, key, s, &len) != ESP_OK) {
        free(s);
        return NULL;
    }
    if (s) {
        s[len] = '\0';
    }
    return s;
}

/* ---- serving ------------------------------------------------------------ */

static acme_pair_t *pair_load(const char *cert, const char *key, const char *name, bool staging)
{
    acme_pair_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    mbedtls_x509_crt_init(&p->crt);
    mbedtls_pk_init(&p->pk);
    if (mbedtls_x509_crt_parse(&p->crt, (const unsigned char *)cert, strlen(cert) + 1) != 0 ||
        mbedtls_pk_parse_key(&p->pk, (const unsigned char *)key, strlen(key) + 1, NULL, 0) != 0) {
        mbedtls_x509_crt_free(&p->crt);
        mbedtls_pk_free(&p->pk);
        free(p);
        return NULL;
    }
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->not_before = x509_time(&p->crt.valid_from);
    p->not_after = x509_time(&p->crt.valid_to);
    p->staging = staging;
    return p;
}

static void pair_free(acme_pair_t *p)
{
    if (p) {
        mbedtls_x509_crt_free(&p->crt);
        mbedtls_pk_free(&p->pk);
        free(p);
    }
}

static void pair_install(acme_pair_t *p)
{
    pair_free(s_retired);
    s_retired = s_cur;
    s_cur = p;
}

int kvm_acme_sni_cb(mbedtls_ssl_context *ssl)
{
    acme_pair_t *p = s_cur;
    if (!p) {
        return 0;
    }
    size_t n = 0;
    const unsigned char *sni = mbedtls_ssl_get_hs_sni(ssl, &n);
    if (sni && n == strlen(p->name) && strncasecmp((const char *)sni, p->name, n) == 0) {
        return mbedtls_ssl_set_hs_own_cert(ssl, &p->crt, &p->pk);
    }
    return 0; /* any other name: the configured certificate */
}

/* ---- HTTP --------------------------------------------------------------- */

typedef struct {
    char *body;
    size_t len;
    int status;
    char nonce[128];
    char location[URL_MAX];
} resp_t;

static esp_err_t http_ev(esp_http_client_event_t *e)
{
    resp_t *r = (resp_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(e->header_key, "Replay-Nonce") == 0) {
            snprintf(r->nonce, sizeof(r->nonce), "%s", e->header_value);
        } else if (strcasecmp(e->header_key, "Location") == 0) {
            snprintf(r->location, sizeof(r->location), "%s", e->header_value);
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        if (r->len + (size_t)e->data_len < RESP_MAX) {
            memcpy(r->body + r->len, e->data, (size_t)e->data_len);
            r->len += (size_t)e->data_len;
            r->body[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_do(const char *url, esp_http_client_method_t method, const char *body,
                         const char *accept, resp_t *r)
{
    r->len = 0;
    r->body[0] = '\0';
    r->status = 0;
    r->nonce[0] = '\0';
    r->location[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = http_ev,
        .user_data = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .disable_auto_redirect = true,
        /* Let's Encrypt's Link headers do not fit the default 512 bytes. */
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "User-Agent", "ESP-KVM");
    if (accept) {
        esp_http_client_set_header(c, "Accept", accept);
    }
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/jose+json");
        esp_http_client_set_post_field(c, body, (int)strlen(body));
    }
    esp_err_t err = esp_http_client_perform(c);
    r->status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return err;
}

/* ---- ACME --------------------------------------------------------------- */

typedef struct {
    resp_t r;
    mbedtls_pk_context acct;
    char jwk[192];
    char thumb[48];
    char nonce[128];
    char kid[URL_MAX];
    char new_nonce[URL_MAX], new_account[URL_MAX], new_order[URL_MAX];
    char err[160];
} acme_t;

/* The account key as a JWK, members in the order RFC 7638 hashes them. */
static esp_err_t make_jwk(acme_t *a)
{
    unsigned char der[160];
    const int n = mbedtls_pk_write_pubkey_der(&a->acct, der, sizeof(der));
    if (n < 65) {
        return ESP_FAIL;
    }
    const unsigned char *pt = der + sizeof(der) - 65; /* 04 || X || Y, written at the end */
    if (pt[0] != 0x04) {
        return ESP_FAIL;
    }
    char x[48], y[48];
    b64url(pt + 1, 32, x, sizeof(x));
    b64url(pt + 33, 32, y, sizeof(y));
    snprintf(a->jwk, sizeof(a->jwk), "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x, y);
    uint8_t h[32];
    if (!sha256(a->jwk, strlen(a->jwk), h)) {
        return ESP_FAIL;
    }
    b64url(h, 32, a->thumb, sizeof(a->thumb));
    return ESP_OK;
}

static esp_err_t load_account_key(acme_t *a)
{
    nvs_handle_t h;
    char *pem = NULL;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        pem = nvs_get_str_alloc(h, "acct");
        nvs_close(h);
    }
    if (pem) {
        const int r = mbedtls_pk_parse_key(&a->acct, (unsigned char *)pem, strlen(pem) + 1, NULL, 0);
        free(pem);
        if (r == 0) {
            return ESP_OK;
        }
        mbedtls_pk_free(&a->acct);
        mbedtls_pk_init(&a->acct);
    }
    if (new_ec_key(&a->acct) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[300];
    if (mbedtls_pk_write_key_pem(&a->acct, (unsigned char *)buf, sizeof(buf)) == 0 &&
        nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "acct", buf);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

static esp_err_t get_nonce(acme_t *a)
{
    if (http_do(a->new_nonce, HTTP_METHOD_HEAD, NULL, NULL, &a->r) != ESP_OK || !a->r.nonce[0]) {
        snprintf(a->err, sizeof(a->err), "Let's Encrypt did not answer");
        return ESP_FAIL;
    }
    snprintf(a->nonce, sizeof(a->nonce), "%s", a->r.nonce);
    return ESP_OK;
}

/* The "detail" of an ACME problem document, or the status. */
static void problem(acme_t *a, const char *what)
{
    cJSON *j = cJSON_Parse(a->r.body);
    const cJSON *d = j ? cJSON_GetObjectItem(j, "detail") : NULL;
    if (cJSON_IsString(d)) {
        snprintf(a->err, sizeof(a->err), "%s: %s", what, d->valuestring);
    } else {
        snprintf(a->err, sizeof(a->err), "%s: HTTP %d", what, a->r.status);
    }
    cJSON_Delete(j);
    ESP_LOGW(TAG, "%s (%s)", a->err, a->r.body);
}

/*
 * A signed POST (JWS, ES256). @p payload NULL is POST-as-GET. Signed with the
 * JWK until the account URL is known, with the kid after.
 */
static esp_err_t acme_post(acme_t *a, const char *url, const char *payload, const char *accept)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!a->nonce[0] && get_nonce(a) != ESP_OK) {
            return ESP_FAIL;
        }
        char *prot = malloc(768);
        if (!prot) {
            return ESP_ERR_NO_MEM;
        }
        if (a->kid[0]) {
            snprintf(prot, 768, "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}",
                     a->kid, a->nonce, url);
        } else {
            snprintf(prot, 768, "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
                     a->jwk, a->nonce, url);
        }
        a->nonce[0] = '\0'; /* used */
        char *p64 = b64url_alloc((const uint8_t *)prot, strlen(prot));
        free(prot);
        char *pl64 = payload ? b64url_alloc((const uint8_t *)payload, strlen(payload)) : strdup("");
        char *input = NULL, *body = NULL;
        esp_err_t res = ESP_ERR_NO_MEM;
        if (p64 && pl64 && (input = malloc(strlen(p64) + strlen(pl64) + 2))) {
            sprintf(input, "%s.%s", p64, pl64);
            uint8_t h[32], der[MBEDTLS_PK_SIGNATURE_MAX_SIZE], raw[64];
            size_t der_len = 0, raw_len = 0;
            char sig[96];
            if (sha256(input, strlen(input), h) &&
                mbedtls_pk_sign(&a->acct, MBEDTLS_MD_SHA256, h, 32, der, sizeof(der), &der_len) == 0 &&
                mbedtls_ecdsa_der_to_raw(256, der, der_len, raw, sizeof(raw), &raw_len) == 0) {
                b64url(raw, raw_len, sig, sizeof(sig));
                const size_t blen = strlen(p64) + strlen(pl64) + strlen(sig) + 64;
                body = malloc(blen);
                if (body) {
                    snprintf(body, blen, "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}",
                             p64, pl64, sig);
                    res = http_do(url, HTTP_METHOD_POST, body, accept, &a->r);
                }
            } else {
                res = ESP_FAIL;
            }
        }
        free(p64);
        free(pl64);
        free(input);
        free(body);
        if (a->r.nonce[0]) {
            snprintf(a->nonce, sizeof(a->nonce), "%s", a->r.nonce);
        }
        if (res != ESP_OK) {
            snprintf(a->err, sizeof(a->err), "Let's Encrypt did not answer (%s)", esp_err_to_name(res));
            return res;
        }
        if (a->r.status == 400 && strstr(a->r.body, "badNonce")) {
            continue; /* a stale nonce: once more with the fresh one */
        }
        return ESP_OK;
    }
    return ESP_OK;
}

static const char *json_str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* POST-as-GET @p url until its "status" leaves pending/processing. */
static cJSON *poll_status(acme_t *a, const char *url, int tries)
{
    for (int i = 0; i < tries; i++) {
        if (acme_post(a, url, NULL, NULL) != ESP_OK) {
            return NULL;
        }
        if (a->r.status != 200) {
            problem(a, "status check failed");
            return NULL;
        }
        cJSON *j = cJSON_Parse(a->r.body);
        const char *st = j ? json_str(j, "status") : NULL;
        if (st && strcmp(st, "pending") != 0 && strcmp(st, "processing") != 0) {
            return j;
        }
        cJSON_Delete(j);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    snprintf(a->err, sizeof(a->err), "Let's Encrypt took too long");
    return NULL;
}

static esp_err_t make_csr(const char *name, mbedtls_pk_context *key, char **b64_out)
{
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_key(&csr, key);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    char subj[160];
    snprintf(subj, sizeof(subj), "CN=%s", name);
    mbedtls_x509_san_list san = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san = {.unstructured_name = {.p = (unsigned char *)name, .len = strlen(name)}}},
        .next = NULL,
    };
    esp_err_t res = ESP_FAIL;
    unsigned char *der = malloc(1024);
    if (der && mbedtls_x509write_csr_set_subject_name(&csr, subj) == 0 &&
        mbedtls_x509write_csr_set_subject_alternative_name(&csr, &san) == 0) {
        const int n = mbedtls_x509write_csr_der(&csr, der, 1024);
        if (n > 0) {
            *b64_out = b64url_alloc(der + 1024 - n, (size_t)n);
            res = *b64_out ? ESP_OK : ESP_ERR_NO_MEM;
        }
    }
    free(der);
    mbedtls_x509write_csr_free(&csr);
    return res;
}

static esp_err_t store_cert(const char *cert, const char *key, const char *name, bool staging, char *err,
                            size_t errlen)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "chain", cert, strlen(cert));
        if (e == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            /* NVS keeps the old copy until the new one is written; drop it first. */
            nvs_erase_key(h, "chain");
            e = nvs_set_blob(h, "chain", cert, strlen(cert));
        }
        if (e == ESP_OK) {
            e = nvs_set_str(h, "key", key);
        }
        if (e == ESP_OK) {
            e = nvs_set_str(h, "name", name);
        }
        if (e == ESP_OK) {
            e = nvs_set_u8(h, "staging", staging ? 1 : 0);
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        snprintf(err, errlen, "could not store the certificate (%s)", esp_err_to_name(e));
    }
    return e;
}

/* The whole order, start to finish. Fills a->err on failure. */
static esp_err_t obtain(acme_t *a, const char *domain, bool staging)
{
    cJSON *j = NULL;
    char *csr = NULL;
    char order_url[URL_MAX] = "", finalize[URL_MAX] = "", authz[URL_MAX] = "", chal[URL_MAX] = "";
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    esp_err_t res = ESP_FAIL;

    set_step("Reading the Let's Encrypt directory");
    if (http_do(staging ? DIR_STAGING : DIR_PROD, HTTP_METHOD_GET, NULL, NULL, &a->r) != ESP_OK ||
        a->r.status != 200 || !(j = cJSON_Parse(a->r.body))) {
        snprintf(a->err, sizeof(a->err), "Let's Encrypt could not be reached (HTTP %d)", a->r.status);
        goto out;
    }
    const char *nn = json_str(j, "newNonce"), *na = json_str(j, "newAccount"), *no = json_str(j, "newOrder");
    if (!nn || !na || !no) {
        snprintf(a->err, sizeof(a->err), "unexpected directory");
        goto out;
    }
    snprintf(a->new_nonce, sizeof(a->new_nonce), "%s", nn);
    snprintf(a->new_account, sizeof(a->new_account), "%s", na);
    snprintf(a->new_order, sizeof(a->new_order), "%s", no);
    cJSON_Delete(j);
    j = NULL;

    set_step("Signing in to the account");
    if (load_account_key(a) != ESP_OK || make_jwk(a) != ESP_OK) {
        snprintf(a->err, sizeof(a->err), "could not make the account key");
        goto out;
    }
    if (acme_post(a, a->new_account, "{\"termsOfServiceAgreed\":true}", NULL) != ESP_OK) {
        goto out;
    }
    if ((a->r.status != 200 && a->r.status != 201) || !a->r.location[0]) {
        problem(a, "account refused");
        goto out;
    }
    snprintf(a->kid, sizeof(a->kid), "%s", a->r.location);

    set_step("Ordering the certificate");
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"%s\"}]}", domain);
    if (acme_post(a, a->new_order, payload, NULL) != ESP_OK) {
        goto out;
    }
    if (a->r.status != 201 || !(j = cJSON_Parse(a->r.body))) {
        problem(a, "order refused");
        goto out;
    }
    snprintf(order_url, sizeof(order_url), "%s", a->r.location);
    const char *fin = json_str(j, "finalize");
    const cJSON *az = cJSON_GetObjectItem(j, "authorizations");
    if (!fin || !cJSON_IsArray(az) || cJSON_GetArraySize(az) < 1 ||
        !cJSON_IsString(cJSON_GetArrayItem(az, 0))) {
        snprintf(a->err, sizeof(a->err), "unexpected order");
        goto out;
    }
    snprintf(finalize, sizeof(finalize), "%s", fin);
    snprintf(authz, sizeof(authz), "%s", cJSON_GetArrayItem(az, 0)->valuestring);
    cJSON_Delete(j);
    j = NULL;

    if (acme_post(a, authz, NULL, NULL) != ESP_OK) {
        goto out;
    }
    if (a->r.status != 200 || !(j = cJSON_Parse(a->r.body))) {
        problem(a, "authorization not readable");
        goto out;
    }
    const char *ast = json_str(j, "status");
    if (!ast || strcmp(ast, "valid") != 0) {
        const char *token = NULL;
        const cJSON *ch;
        cJSON_ArrayForEach(ch, cJSON_GetObjectItem(j, "challenges"))
        {
            const char *type = json_str(ch, "type");
            if (type && strcmp(type, "dns-01") == 0) {
                token = json_str(ch, "token");
                const char *u = json_str(ch, "url");
                snprintf(chal, sizeof(chal), "%s", u ? u : "");
            }
        }
        if (!token || !chal[0]) {
            snprintf(a->err, sizeof(a->err), "no DNS challenge offered");
            goto out;
        }
        char ka[160];
        snprintf(ka, sizeof(ka), "%s.%s", token, a->thumb);
        uint8_t h[32];
        char txt[48];
        sha256(ka, strlen(ka), h);
        b64url(h, 32, txt, sizeof(txt));
        cJSON_Delete(j);
        j = NULL;

        set_step("Publishing the DNS record through Tailscale");
        char rec[160], why[96] = "";
        snprintf(rec, sizeof(rec), "_acme-challenge.%s", domain);
        if (kvm_ts_set_txt(rec, txt, why, sizeof(why)) != ESP_OK) {
            snprintf(a->err, sizeof(a->err), "Tailscale did not publish the DNS record: %s", why);
            goto out;
        }

        set_step("Waiting for Let's Encrypt to check the record");
        if (acme_post(a, chal, "{}", NULL) != ESP_OK) {
            goto out;
        }
        if (a->r.status != 200) {
            problem(a, "challenge refused");
            goto out;
        }
        j = poll_status(a, authz, 40);
        if (!j) {
            goto out;
        }
        ast = json_str(j, "status");
        if (!ast || strcmp(ast, "valid") != 0) {
            const cJSON *c0;
            const char *detail = NULL;
            cJSON_ArrayForEach(c0, cJSON_GetObjectItem(j, "challenges"))
            {
                const cJSON *e = cJSON_GetObjectItem(c0, "error");
                if (e && json_str(e, "detail")) {
                    detail = json_str(e, "detail");
                }
            }
            snprintf(a->err, sizeof(a->err), "the DNS check failed: %s", detail ? detail : ast ? ast : "?");
            goto out;
        }
    }
    cJSON_Delete(j);
    j = NULL;

    set_step("Making the key and the request");
    if (new_ec_key(&key) != ESP_OK || make_csr(domain, &key, &csr) != ESP_OK) {
        snprintf(a->err, sizeof(a->err), "could not make the key");
        goto out;
    }
    char *fp = malloc(strlen(csr) + 16);
    if (!fp) {
        goto out;
    }
    sprintf(fp, "{\"csr\":\"%s\"}", csr);
    esp_err_t pr = acme_post(a, finalize, fp, NULL);
    free(fp);
    if (pr != ESP_OK) {
        goto out;
    }
    if (a->r.status != 200) {
        problem(a, "finalize refused");
        goto out;
    }

    set_step("Waiting for the certificate");
    j = poll_status(a, order_url, 20);
    if (!j) {
        goto out;
    }
    const char *ost = json_str(j, "status"), *cu = json_str(j, "certificate");
    if (!ost || strcmp(ost, "valid") != 0 || !cu) {
        snprintf(a->err, sizeof(a->err), "the order ended as %s", ost ? ost : "?");
        goto out;
    }
    char cert_url[URL_MAX];
    snprintf(cert_url, sizeof(cert_url), "%s", cu);
    cJSON_Delete(j);
    j = NULL;

    set_step("Downloading the certificate");
    if (acme_post(a, cert_url, NULL, "application/pem-certificate-chain") != ESP_OK) {
        goto out;
    }
    if (a->r.status != 200 || !strstr(a->r.body, "-----BEGIN CERTIFICATE-----")) {
        problem(a, "certificate download failed");
        goto out;
    }

    char kpem[300];
    if (mbedtls_pk_write_key_pem(&key, (unsigned char *)kpem, sizeof(kpem)) != 0) {
        snprintf(a->err, sizeof(a->err), "could not write the key");
        goto out;
    }
    acme_pair_t *p = pair_load(a->r.body, kpem, domain, staging);
    if (!p) {
        snprintf(a->err, sizeof(a->err), "the certificate does not parse");
        goto out;
    }
    if (store_cert(a->r.body, kpem, domain, staging, a->err, sizeof(a->err)) != ESP_OK) {
        pair_free(p);
        goto out;
    }
    pair_install(p);
    res = ESP_OK;

out:
    cJSON_Delete(j);
    free(csr);
    mbedtls_pk_free(&key);
    return res;
}

/* ---- the worker and the schedule --------------------------------------- */

static void worker(void *arg)
{
    const bool staging = (bool)(uintptr_t)arg;
    kvm_ts_status_t ts;
    kvm_ts_status(&ts);
    s_last_try = time(NULL);
    s_error[0] = '\0';

    acme_t *a = calloc(1, sizeof(*a));
    char *body = malloc(RESP_MAX);
    esp_err_t res = ESP_FAIL;
    if (!a || !body) {
        snprintf(s_error, sizeof(s_error), "out of memory");
    } else if (!ts.up) {
        snprintf(s_error, sizeof(s_error), "Tailscale is not connected");
    } else if (!ts.cert_domain[0]) {
        snprintf(s_error, sizeof(s_error),
                 "HTTPS certificates are off for this tailnet: turn them on in the Tailscale "
                 "admin console, DNS page");
    } else {
        a->r.body = body;
        mbedtls_pk_init(&a->acct);
        res = obtain(a, ts.cert_domain, staging);
        if (res != ESP_OK) {
            snprintf(s_error, sizeof(s_error), "%s", a->err[0] ? a->err : "failed");
        }
        mbedtls_pk_free(&a->acct);
    }
    free(body);
    free(a);

    if (res == ESP_OK) {
        s_fails = 0;
        s_next_try = 0;
        set_step("Done");
        ESP_LOGI(TAG, "certificate for %s until %lld", s_cur->name, (long long)s_cur->not_after);
    } else {
        s_fails++;
        const int hours = s_fails >= 5 ? 24 : 1 << (s_fails - 1);
        s_next_try = time(NULL) + hours * 3600;
        set_step("Failed");
        ESP_LOGW(TAG, "%s", s_error);
    }
    s_busy = false;
    vTaskDelete(NULL);
}

esp_err_t kvm_acme_run(bool staging)
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    /* TLS to Let's Encrypt and NVS writes: an internal-RAM stack, only while it runs. */
    if (xTaskCreate(worker, "acme", 10 * 1024, (void *)(uintptr_t)staging, 4, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Every ten minutes: is a certificate wanted, and is ours missing or two-thirds used? */
static void tick(void *arg)
{
    (void)arg;
    if (s_busy || !kvm_setting_bool("ts_cert")) {
        return;
    }
    const time_t now = time(NULL);
    if (now < 1735689600 || (s_next_try && now < s_next_try)) {
        return; /* clock not set yet, or backing off */
    }
    kvm_ts_status_t ts;
    kvm_ts_status(&ts);
    if (!ts.up || !ts.cert_domain[0]) {
        return;
    }
    acme_pair_t *p = s_cur;
    if (p && !p->staging && strcasecmp(p->name, ts.cert_domain) == 0 &&
        now < p->not_after - (p->not_after - p->not_before) / 3) {
        return;
    }
    kvm_acme_run(false);
}

/* RFC 3339 in UTC, as Go prints it. */
static void rfc3339(time_t t, char *out, size_t cap)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* The Tailscale admin console's "TLS certificate" box: GET /tls-cert-status
 * ?domain=<name>, answered like tailscaled does (tailcfg.C2NTLSCertInfo). */
static int c2n(const char *method, const char *path, char *body, size_t cap)
{
    const char *q = strncmp(path, "/tls-cert-status", 16) == 0 ? path + 16 : NULL;
    if (!q || (*q != '?' && *q != '\0') || strcmp(method, "GET") != 0) {
        return 404;
    }
    const char *d = strstr(q, "domain=");
    char domain[128] = "";
    if (d) {
        d += 7;
        size_t n = strcspn(d, "&");
        if (n < sizeof(domain)) {
            memcpy(domain, d, n);
            domain[n] = '\0';
        }
    }
    if (!domain[0]) {
        snprintf(body, cap, "no 'domain'\n");
        return 400;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return 500;
    }
    acme_pair_t *p = s_cur;
    if (!p || strcasecmp(p->name, domain) != 0) {
        cJSON_AddBoolToObject(o, "Missing", true);
        cJSON_AddStringToObject(o, "Error", s_error[0] ? s_error : "no certificate");
    } else {
        char nb[32], na[32];
        rfc3339(p->not_before, nb, sizeof(nb));
        rfc3339(p->not_after, na, sizeof(na));
        if (time(NULL) > p->not_after) {
            cJSON_AddBoolToObject(o, "Expired", true);
            cJSON_AddStringToObject(o, "Error", "cert expired");
        } else if (p->staging) {
            cJSON_AddStringToObject(o, "Error", "a Let's Encrypt test certificate");
        } else {
            cJSON_AddBoolToObject(o, "Valid", true);
        }
        cJSON_AddStringToObject(o, "NotBefore", nb);
        cJSON_AddStringToObject(o, "NotAfter", na);
    }
    if (!cJSON_PrintPreallocated(o, body, (int)cap, false)) {
        snprintf(body, cap, "{}");
    }
    cJSON_Delete(o);
    return 200;
}

void kvm_acme_init(void)
{
    kvm_ts_set_c2n_cb(c2n);
    (void)psa_crypto_init();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char *cert = nvs_get_blob_str(h, "chain");
        char *key = nvs_get_str_alloc(h, "key");
        char *name = nvs_get_str_alloc(h, "name");
        uint8_t stg = 0;
        nvs_get_u8(h, "staging", &stg);
        nvs_close(h);
        if (cert && key && name) {
            acme_pair_t *p = pair_load(cert, key, name, stg != 0);
            if (p) {
                s_cur = p;
                ESP_LOGI(TAG, "serving the Let's Encrypt certificate for %s", name);
            }
        }
        free(cert);
        free(key);
        free(name);
    }
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = tick, .name = "acme"};
    if (!t && esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 10ull * 60 * 1000000);
    }
}

void kvm_acme_status_json(char *buf, size_t len)
{
    kvm_ts_status_t ts;
    kvm_ts_status(&ts);
    acme_pair_t *p = s_cur;
    buf[0] = '\0';
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddBoolToObject(o, "enabled", kvm_setting_bool("ts_cert"));
    cJSON_AddBoolToObject(o, "tailscale", ts.up);
    cJSON_AddStringToObject(o, "domain", ts.cert_domain);
    cJSON_AddStringToObject(o, "name", ts.name);
    cJSON_AddBoolToObject(o, "busy", s_busy);
    cJSON_AddStringToObject(o, "step", s_step);
    cJSON_AddStringToObject(o, "error", s_error);
    cJSON_AddStringToObject(o, "cert", p ? p->name : "");
    cJSON_AddNumberToObject(o, "expires", p ? (double)p->not_after : 0);
    cJSON_AddBoolToObject(o, "staging", p && p->staging);
    cJSON_AddNumberToObject(o, "lastTry", (double)s_last_try);
    cJSON_AddNumberToObject(o, "nextTry", (double)s_next_try);
    if (!cJSON_PrintPreallocated(o, buf, (int)len, false)) {
        snprintf(buf, len, "{}");
    }
    cJSON_Delete(o);
}
