/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The DTLS certificate: a fresh self-signed ECDSA P-256 one each boot. WebRTC
 * trusts it by its fingerprint, which travels in the answer over the
 * signed-in HTTPS connection, so no CA and no name are involved and nothing
 * needs to be stored.
 */
#include "wr_cert.h"

#include <string.h>

#include "esp_random.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define DER_MAX 1024

int wr_cert_make(wr_cert_t *c)
{
    memset(c, 0, sizeof(*c));
    mbedtls_pk_init(&c->key);
    mbedtls_x509_crt_init(&c->crt);
    if (psa_crypto_init() != PSA_SUCCESS) {
        return -1;
    }

    psa_key_id_t id = 0;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE |
                                       PSA_KEY_USAGE_EXPORT);
    if (psa_generate_key(&attr, &id) != PSA_SUCCESS) {
        return -1;
    }
    const int copied = mbedtls_pk_copy_from_psa(id, &c->key);
    psa_destroy_key(id);
    if (copied != 0) {
        return -1;
    }

    int res = -1;
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    unsigned char *der = calloc(1, DER_MAX);
    if (!der) {
        goto done;
    }
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7f) | 0x01; /* positive, no leading zero */
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &c->key);
    mbedtls_x509write_crt_set_issuer_key(&w, &c->key);
    if (mbedtls_x509write_crt_set_subject_name(&w, "CN=ESP-KVM WebRTC") != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&w, "CN=ESP-KVM WebRTC") != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_validity(&w, "20250101000000", "20491231235959") != 0) {
        goto done;
    }
    /* The DER is written at the END of the buffer. */
    const int n = mbedtls_x509write_crt_der(&w, der, DER_MAX);
    if (n <= 0) {
        goto done;
    }
    const unsigned char *start = der + DER_MAX - n;
    size_t hl = 0;
    if (mbedtls_x509_crt_parse_der(&c->crt, start, (size_t)n) != 0 ||
        psa_hash_compute(PSA_ALG_SHA_256, start, (size_t)n, c->fingerprint, 32, &hl) !=
            PSA_SUCCESS ||
        hl != 32) {
        goto done;
    }
    res = 0;

done:
    free(der);
    mbedtls_x509write_crt_free(&w);
    if (res != 0) {
        wr_cert_free(c);
    }
    return res;
}

void wr_cert_free(wr_cert_t *c)
{
    mbedtls_pk_free(&c->key);
    mbedtls_x509_crt_free(&c->crt);
}
