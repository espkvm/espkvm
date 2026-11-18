/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>

#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

typedef struct {
    mbedtls_pk_context key;
    mbedtls_x509_crt crt;
    /* SHA-256 of the DER, for a=fingerprint. */
    uint8_t fingerprint[32];
} wr_cert_t;

/** Make a key and a self-signed certificate. 0 on success. */
int wr_cert_make(wr_cert_t *c);
void wr_cert_free(wr_cert_t *c);
