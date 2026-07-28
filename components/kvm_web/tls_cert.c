/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device is its own certificate authority. A browser will not accept a
 * self-signed *leaf* certificate as trusted no matter how it is imported - a
 * trust anchor has to be a CA - so instead the device generates a small root CA
 * once, keeps it in NVS, and signs its server (leaf) certificate with it. The
 * operator imports the CA (offered at /cert.pem) one time; from then on every
 * certificate the device serves is trusted, including after the hostname or IP
 * changes, and the browser turns on the WebSocket channel and the H.264 decoder
 * that a secure context requires.
 *
 * The CA is stable; the leaf is re-issued whenever the hostname or a static IP
 * changes (it names them in its SANs) but always under the same CA, so the
 * import survives.
 */
#include "kvm_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/*
 * IDF 6 ships mbedTLS 4, where key material lives behind PSA: keys are generated
 * with PSA and copied into a pk context (which, per the API, becomes fully
 * independent of the PSA key afterwards). The CA key is stored as PEM and parsed
 * back to sign each new leaf.
 */
#include "mbedtls/asn1.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "kvm_settings.h"

#define TAG "tls"

#define NVS_NAMESPACE "kvm_tls"
#define NVS_KEY_CA_CERT "ca_cert"
#define NVS_KEY_CA_KEY "ca_key"
#define NVS_KEY_CERT "cert"
#define NVS_KEY_KEY "key"
/** What the stored leaf was issued for (hostname, or "hostname|ip" with a static
 *  address), so a renamed or re-addressed device re-issues it. */
#define NVS_KEY_NAME "name"

#define CA_SUBJECT "CN=ESP-KVM Device CA,O=ESP-KVM"

/** Comfortably larger than a P-256 key and a small certificate. */
#define KEY_PEM_MAX 512
#define CERT_PEM_MAX 1536

/*
 * The device has no clock and no NTP, so it cannot know the date at generation
 * time. A fixed, very wide validity window is the honest answer.
 */
#define VALID_FROM "20200101000000"
#define VALID_TO "20500101000000"

static void hostname_now(char *out, size_t len)
{
    const char *h = kvm_setting_str("net_hostname");
    if (!h || !h[0]) {
        h = "espkvm";
    }
    snprintf(out, len, "%s", h);
}

/** Parse "a.b.c.d" into four bytes. False (and out untouched) if it is not one. */
static bool parse_ip4(const char *s, uint8_t out[4])
{
    if (!s || !s[0]) {
        return false;
    }
    int a, b, c, d, n = 0;
    if (sscanf(s, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) != 4 || s[n] != '\0') {
        return false;
    }
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
        return false;
    }
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return true;
}

/*
 * The address the leaf should also name, or "" for none. Only a static address
 * is used: it is known here and now (from the setting, before the network is
 * even up) and it does not move. A DHCP lease can change between boots, so
 * naming it would churn the certificate; DHCP stays hostname-only.
 */
static void cert_ip_now(char *out, size_t len)
{
    out[0] = '\0';
    if (kvm_setting_bool("net_dhcp")) {
        return;
    }
    uint8_t tmp[4];
    const char *ip = kvm_setting_str("net_ip");
    if (parse_ip4(ip, tmp)) {
        snprintf(out, len, "%s", ip);
    }
}

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_get_alloc(nvs_handle_t h, const char *key, char **out)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &len);
    if (err != ESP_OK) {
        return err;
    }
    char *buf = calloc(1, len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(h, key, buf, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *out = buf;
    return ESP_OK;
}

static esp_err_t nvs_load_ca(char **cert, char **key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_alloc(h, NVS_KEY_CA_CERT, cert);
    if (err == ESP_OK) {
        err = nvs_get_alloc(h, NVS_KEY_CA_KEY, key);
        if (err != ESP_OK) {
            free(*cert);
            *cert = NULL;
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_load_leaf(const char *idkey, char **cert, char **key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    char stored[64] = {0};
    size_t len = sizeof(stored);
    err = nvs_get_str(h, NVS_KEY_NAME, stored, &len);
    if (err != ESP_OK || strcmp(stored, idkey) != 0) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }
    err = nvs_get_alloc(h, NVS_KEY_CERT, cert);
    if (err == ESP_OK) {
        err = nvs_get_alloc(h, NVS_KEY_KEY, key);
        if (err != ESP_OK) {
            free(*cert);
            *cert = NULL;
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_store_ca(const char *cert, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_CA_CERT, cert);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_CA_KEY, key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_store_leaf(const char *idkey, const char *cert, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_CERT, cert);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_KEY, key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_NAME, idkey);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- key and certificate generation ------------------------------------ */

/**
 * Generate an ECDSA P-256 key: populate @p key (caller frees) and write its PEM
 * to @p pem. P-256 rather than RSA because this happens on the device, where an
 * RSA-2048 key would take tens of seconds.
 */
static esp_err_t gen_key(mbedtls_pk_context *key, char *pem, size_t pem_len)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
                                       PSA_KEY_USAGE_EXPORT);
    if (psa_generate_key(&attr, &key_id) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    esp_err_t res = ESP_FAIL;
    if (mbedtls_pk_copy_from_psa(key_id, key) == 0 &&
        mbedtls_pk_write_key_pem(key, (unsigned char *)pem, pem_len) == 0) {
        res = ESP_OK;
    }
    /* The pk is independent of the PSA key now, so it is safe to drop it here
     * and still sign with the pk afterwards. */
    (void)psa_destroy_key(key_id);
    return res;
}

static void random_serial(unsigned char serial[16])
{
    esp_fill_random(serial, 16);
    serial[0] &= 0x7f; /* keep it a positive integer */
}

/** Generate the root CA (self-signed, marked as a CA so it can be a trust anchor). */
static esp_err_t gen_ca(char **cert_out, char **key_out)
{
    esp_err_t res = ESP_FAIL;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    char *key_pem = calloc(1, KEY_PEM_MAX);
    char *cert_pem = calloc(1, CERT_PEM_MAX);
    if (!key_pem || !cert_pem) {
        res = ESP_ERR_NO_MEM;
        goto done;
    }
    if (gen_key(&key, key_pem, KEY_PEM_MAX) != ESP_OK) {
        goto done;
    }

    unsigned char serial[16];
    random_serial(serial);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, CA_SUBJECT) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, CA_SUBJECT) != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_validity(&crt, VALID_FROM, VALID_TO) != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0) != 0 ||
        mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_KEY_CERT_SIGN |
                                                      MBEDTLS_X509_KU_CRL_SIGN) != 0 ||
        mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0 ||
        mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0) {
        ESP_LOGE(TAG, "CA fields rejected");
        goto done;
    }
    int written = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert_pem, CERT_PEM_MAX);
    if (written != 0) {
        ESP_LOGE(TAG, "CA PEM write failed (-0x%04x)", -written);
        goto done;
    }
    *cert_out = cert_pem;
    *key_out = key_pem;
    cert_pem = NULL;
    key_pem = NULL;
    res = ESP_OK;

done:
    free(cert_pem);
    free(key_pem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return res;
}

/** Generate a server (leaf) certificate signed by the CA, naming @p name and,
 *  if given, @p ip in its SANs. */
static esp_err_t gen_leaf(const char *ca_key_pem, const char *name, const char *ip,
                          char **cert_out, char **key_out)
{
    esp_err_t res = ESP_FAIL;
    mbedtls_pk_context ca_key;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&ca_key);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    char *key_pem = calloc(1, KEY_PEM_MAX);
    char *cert_pem = calloc(1, CERT_PEM_MAX);
    if (!key_pem || !cert_pem) {
        res = ESP_ERR_NO_MEM;
        goto done;
    }
    if (mbedtls_pk_parse_key(&ca_key, (const unsigned char *)ca_key_pem, strlen(ca_key_pem) + 1,
                             NULL, 0) != 0) {
        ESP_LOGE(TAG, "CA key could not be parsed");
        goto done;
    }
    if (gen_key(&key, key_pem, KEY_PEM_MAX) != ESP_OK) {
        goto done;
    }

    char subject[80];
    char san_local[48];
    snprintf(subject, sizeof(subject), "CN=%s.local,O=ESP-KVM", name);
    snprintf(san_local, sizeof(san_local), "%s.local", name);

    uint8_t ip_bytes[4];
    const bool have_ip = parse_ip4(ip, ip_bytes);

    /* Browsers reject a certificate with no subjectAltName. The bare hostname,
     * the mDNS name and (for a static address) the IP are all listed. IP SANs
     * carry the raw four bytes, not text. */
    mbedtls_x509_san_list san_host = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san = {.unstructured_name = {.p = (unsigned char *)name, .len = strlen(name)}}},
        .next = NULL,
    };
    mbedtls_x509_san_list san_mdns = {
        .node = {.type = MBEDTLS_X509_SAN_DNS_NAME,
                 .san = {.unstructured_name = {.p = (unsigned char *)san_local,
                                               .len = strlen(san_local)}}},
        .next = &san_host,
    };
    mbedtls_x509_san_list san_ip = {
        .node = {.type = MBEDTLS_X509_SAN_IP_ADDRESS,
                 .san = {.unstructured_name = {.p = ip_bytes, .len = sizeof(ip_bytes)}}},
        .next = &san_mdns,
    };
    mbedtls_x509_san_list *san_head = have_ip ? &san_ip : &san_mdns;

    /* extendedKeyUsage = serverAuth; Chrome requires it on the leaf when the
     * chain is validated against a privately-imported CA. */
    mbedtls_asn1_sequence eku = {
        .buf = {.tag = MBEDTLS_ASN1_OID,
                .p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH,
                .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)},
        .next = NULL,
    };

    unsigned char serial[16];
    random_serial(serial);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &ca_key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, CA_SUBJECT) != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_validity(&crt, VALID_FROM, VALID_TO) != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                      MBEDTLS_X509_KU_KEY_AGREEMENT) != 0 ||
        mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku) != 0 ||
        mbedtls_x509write_crt_set_subject_alternative_name(&crt, san_head) != 0 ||
        mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0 ||
        mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0) {
        ESP_LOGE(TAG, "leaf fields rejected");
        goto done;
    }
    int written = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert_pem, CERT_PEM_MAX);
    if (written != 0) {
        ESP_LOGE(TAG, "leaf PEM write failed (-0x%04x)", -written);
        goto done;
    }
    *cert_out = cert_pem;
    *key_out = key_pem;
    cert_pem = NULL;
    key_pem = NULL;
    res = ESP_OK;

done:
    free(cert_pem);
    free(key_pem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_pk_free(&ca_key);
    return res;
}

/*
 * Key generation and certificate writing need something like ten kilobytes of
 * stack - far more than app_main has - so the work runs on a task made for it
 * and thrown away afterwards.
 */
enum { JOB_CA, JOB_LEAF };
typedef struct {
    int kind;
    const char *ca_key_pem; /* JOB_LEAF */
    const char *name;       /* JOB_LEAF */
    const char *ip;         /* JOB_LEAF */
    char *cert;             /* out */
    char *key;              /* out */
    esp_err_t result;
    SemaphoreHandle_t done;
} gen_job_t;

static void gen_task(void *arg)
{
    gen_job_t *job = (gen_job_t *)arg;
    job->result = (job->kind == JOB_CA)
                      ? gen_ca(&job->cert, &job->key)
                      : gen_leaf(job->ca_key_pem, job->name, job->ip, &job->cert, &job->key);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t run_gen(gen_job_t *job)
{
    job->done = xSemaphoreCreateBinary();
    if (!job->done) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(gen_task, "tls_gen", 12 * 1024, job, 5, NULL) != pdPASS) {
        vSemaphoreDelete(job->done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(job->done, portMAX_DELAY);
    vSemaphoreDelete(job->done);
    return job->result;
}

esp_err_t kvm_tls_identity_get(kvm_tls_identity_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char name[40];
    hostname_now(name, sizeof(name));
    char ip[16];
    cert_ip_now(ip, sizeof(ip));
    char idkey[64];
    if (ip[0]) {
        snprintf(idkey, sizeof(idkey), "%s|%s", name, ip);
    } else {
        snprintf(idkey, sizeof(idkey), "%s", name);
    }

    /* The CA: generated once and kept, so importing it is a one-time act. */
    char *ca_cert = NULL;
    char *ca_key = NULL;
    bool fresh_ca = false;
    if (nvs_load_ca(&ca_cert, &ca_key) != ESP_OK) {
        const int64_t started = esp_log_timestamp();
        gen_job_t job = {.kind = JOB_CA};
        esp_err_t err = run_gen(&job);
        if (err != ESP_OK) {
            return err;
        }
        ca_cert = job.cert;
        ca_key = job.key;
        fresh_ca = true;
        if (nvs_store_ca(ca_cert, ca_key) != ESP_OK) {
            ESP_LOGW(TAG, "CA not stored; it will be regenerated next boot");
        }
        ESP_LOGI(TAG, "generated a device CA in %lld ms",
                 (long long)(esp_log_timestamp() - started));
    }

    /*
     * The leaf: re-issued when the hostname or static IP changes - and always
     * when the CA is new, since any stored leaf was signed by a different (or no)
     * CA and would not chain to this one. That is also the upgrade path from the
     * old self-signed-leaf builds.
     */
    char *leaf_cert = NULL;
    char *leaf_key = NULL;
    if (fresh_ca || nvs_load_leaf(idkey, &leaf_cert, &leaf_key) != ESP_OK) {
        gen_job_t job = {.kind = JOB_LEAF, .ca_key_pem = ca_key, .name = name, .ip = ip};
        esp_err_t err = run_gen(&job);
        if (err != ESP_OK) {
            free(ca_cert);
            free(ca_key);
            return err;
        }
        leaf_cert = job.cert;
        leaf_key = job.key;
        if (nvs_store_leaf(idkey, leaf_cert, leaf_key) != ESP_OK) {
            ESP_LOGW(TAG, "certificate not stored; it will be regenerated next boot");
        }
        ESP_LOGI(TAG, "issued a certificate for %s.local%s%s", name, ip[0] ? " / " : "", ip);
    } else {
        ESP_LOGI(TAG, "certificate for %s.local loaded", name);
    }

    /* Serve the leaf followed by the CA, so the chain is complete on the wire. */
    const size_t chain_len = strlen(leaf_cert) + strlen(ca_cert) + 1;
    char *chain = malloc(chain_len);
    if (!chain) {
        free(ca_cert);
        free(ca_key);
        free(leaf_cert);
        free(leaf_key);
        return ESP_ERR_NO_MEM;
    }
    snprintf(chain, chain_len, "%s%s", leaf_cert, ca_cert);

    out->cert_pem = chain;
    out->cert_len = strlen(chain) + 1;
    out->key_pem = leaf_key; /* ownership transferred */
    out->key_len = strlen(leaf_key) + 1;
    out->ca_pem = ca_cert;   /* ownership transferred; this is what to import */
    out->ca_len = strlen(ca_cert) + 1;

    free(leaf_cert);
    free(ca_key);
    return ESP_OK;
}

void kvm_tls_identity_free(kvm_tls_identity_t *id)
{
    if (!id) {
        return;
    }
    free(id->cert_pem);
    free(id->key_pem);
    free(id->ca_pem);
    memset(id, 0, sizeof(*id));
}

esp_err_t kvm_tls_identity_reset(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    (void)nvs_erase_all(nvs);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
