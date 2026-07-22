/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_tls.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/*
 * IDF 6 ships mbedTLS 4, where key material lives behind PSA: there is no
 * public pk.h key generation, no ctr_drbg, and the X.509 writer takes its
 * randomness from the PSA layer rather than from a DRBG passed in. The key is
 * generated with PSA and then copied into a pk context, which is the one place
 * the two halves of the API still meet.
 */
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "kvm_settings.h"

#define TAG "tls"

#define NVS_NAMESPACE "kvm_tls"
#define NVS_KEY_CERT "cert"
#define NVS_KEY_KEY "key"
/** What the stored certificate was issued for, so a renamed device is noticed. */
#define NVS_KEY_NAME "name"

/** Comfortably larger than a P-256 key and a small self-signed certificate. */
#define KEY_PEM_MAX 512
#define CERT_PEM_MAX 1536

/*
 * The device has no clock and no NTP, so it cannot know the date at generation
 * time. A fixed, very wide validity window is the honest answer: the
 * certificate is not trusted on the strength of its dates by anything, and a
 * window computed from a wrong clock would expire at an arbitrary moment.
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

static esp_err_t nvs_load(kvm_tls_identity_t *out, const char *want_name)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char stored_name[40] = {0};
    size_t name_len = sizeof(stored_name);
    err = nvs_get_str(nvs, NVS_KEY_NAME, stored_name, &name_len);
    if (err != ESP_OK || strcmp(stored_name, want_name) != 0) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }

    size_t cert_len = 0;
    size_t key_len = 0;
    if (nvs_get_str(nvs, NVS_KEY_CERT, NULL, &cert_len) != ESP_OK ||
        nvs_get_str(nvs, NVS_KEY_KEY, NULL, &key_len) != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    char *cert = calloc(1, cert_len);
    char *key = calloc(1, key_len);
    if (!cert || !key) {
        free(cert);
        free(key);
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(nvs, NVS_KEY_CERT, cert, &cert_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, NVS_KEY_KEY, key, &key_len);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        free(cert);
        free(key);
        return err;
    }
    out->cert_pem = cert;
    out->cert_len = cert_len;
    out->key_pem = key;
    out->key_len = key_len;
    return ESP_OK;
}

static esp_err_t nvs_store(const kvm_tls_identity_t *id, const char *name)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, NVS_KEY_CERT, id->cert_pem);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_KEY, id->key_pem);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_NAME, name);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/**
 * Generate an ECDSA P-256 self-signed certificate.
 *
 * P-256 rather than RSA because this happens on the device: an RSA-2048 key
 * takes tens of seconds to generate here and would hold up the first boot,
 * while P-256 takes a fraction of a second on hardware that accelerates it.
 */
static esp_err_t generate(const char *name, kvm_tls_identity_t *out)
{
    esp_err_t result = ESP_FAIL;
    char subject[80];
    char san_local[48];
    unsigned char *key_pem = NULL;
    unsigned char *cert_pem = NULL;
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;

    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA crypto init failed (%d)", (int)ps);
        goto done;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    /* Exported once, to write the PEM the TLS server is configured with. */
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
                                       PSA_KEY_USAGE_EXPORT);
    ps = psa_generate_key(&attr, &key_id);
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "key generation failed (%d)", (int)ps);
        goto done;
    }
    if (mbedtls_pk_copy_from_psa(key_id, &key) != 0) {
        ESP_LOGE(TAG, "key could not be read back");
        goto done;
    }

    key_pem = calloc(1, KEY_PEM_MAX);
    cert_pem = calloc(1, CERT_PEM_MAX);
    if (!key_pem || !cert_pem) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    if (mbedtls_pk_write_key_pem(&key, key_pem, KEY_PEM_MAX) != 0) {
        ESP_LOGE(TAG, "key PEM write failed");
        goto done;
    }

    snprintf(subject, sizeof(subject), "CN=%s.local,O=ESP-KVM", name);
    snprintf(san_local, sizeof(san_local), "%s.local", name);

    /*
     * Browsers stopped reading the common name years ago: a certificate without
     * subjectAltName is rejected outright rather than merely mistrusted. Both
     * the bare hostname and the mDNS name are listed, because the device
     * answers to both.
     */
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

    /* Random serial: two certificates from the same device should not collide
     * in a browser's store after a regeneration. */
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7f; /* keep it a positive integer */

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_validity(&crt, VALID_FROM, VALID_TO) != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_mdns) != 0 ||
        mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                      MBEDTLS_X509_KU_KEY_AGREEMENT) != 0) {
        ESP_LOGE(TAG, "certificate fields rejected");
        goto done;
    }

    int written = mbedtls_x509write_crt_pem(&crt, cert_pem, CERT_PEM_MAX);
    if (written != 0) {
        ESP_LOGE(TAG, "certificate PEM write failed (-0x%04x)", -written);
        goto done;
    }

    out->key_pem = (char *)key_pem;
    out->key_len = strlen((char *)key_pem) + 1;
    out->cert_pem = (char *)cert_pem;
    out->cert_len = strlen((char *)cert_pem) + 1;
    key_pem = NULL;
    cert_pem = NULL;
    result = ESP_OK;

done:
    free(key_pem);
    free(cert_pem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    /* The PEM is what gets kept; the PSA copy has done its job. */
    (void)psa_destroy_key(key_id);
    return result;
}

/*
 * Key generation and certificate writing need something like ten kilobytes of
 * stack - far more than app_main is given, and more than it is worth giving to
 * every task in the system for something that happens once. So the work runs
 * on a task created for it and thrown away afterwards.
 */
typedef struct {
    const char *name;
    kvm_tls_identity_t *out;
    esp_err_t result;
    SemaphoreHandle_t done;
} generate_job_t;

static void generate_task(void *arg)
{
    generate_job_t *job = (generate_job_t *)arg;
    job->result = generate(job->name, job->out);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t generate_off_stack(const char *name, kvm_tls_identity_t *out)
{
    generate_job_t job = {.name = name, .out = out, .result = ESP_FAIL};
    job.done = xSemaphoreCreateBinary();
    if (!job.done) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(generate_task, "tls_gen", 12 * 1024, &job, 5, NULL) != pdPASS) {
        vSemaphoreDelete(job.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.result;
}

esp_err_t kvm_tls_identity_get(kvm_tls_identity_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char name[40];
    hostname_now(name, sizeof(name));

    if (nvs_load(out, name) == ESP_OK) {
        ESP_LOGI(TAG, "certificate for %s.local loaded", name);
        return ESP_OK;
    }

    const int64_t started = esp_log_timestamp();
    esp_err_t err = generate_off_stack(name, out);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "generated a certificate for %s.local in %lld ms", name,
             (long long)(esp_log_timestamp() - started));

    err = nvs_store(out, name);
    if (err != ESP_OK) {
        /* Usable now, regenerated next boot: worth a warning, not a failure. */
        ESP_LOGW(TAG, "certificate not stored (%s); it will be regenerated on the next boot",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

void kvm_tls_identity_free(kvm_tls_identity_t *id)
{
    if (!id) {
        return;
    }
    free(id->cert_pem);
    free(id->key_pem);
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
