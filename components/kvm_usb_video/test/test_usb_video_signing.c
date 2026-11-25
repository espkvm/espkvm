/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sodium.h>

enum { TOKEN_LEN = 16, HEADER_LEN = 8, HELLO_LEN = 52 };
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, NVS_READWRITE, PSA_SUCCESS = 0, PSA_ALG_SHA_256 = 1 };
enum { SESSION = 0, READY_MSG = 2, ACTIVE = 2 };
typedef int nvs_handle_t;
typedef int esp_err_t;
typedef struct {
    uint32_t generation;
    uint8_t token[TOKEN_LEN];
    uint8_t header[HEADER_LEN + HELLO_LEN];
} session_t;

static uint8_t public_key[crypto_sign_ed25519_PUBLICKEYBYTES];
static uint8_t secret_key[crypto_sign_ed25519_SECRETKEYBYTES];
static char fingerprint[24];
static uint8_t sent[116];

static int nvs_open(const char *name, int flags, nvs_handle_t *handle)
{
    assert(strcmp(name, "usb_video") == 0 && flags == NVS_READWRITE);
    *handle = 1;
    return ESP_OK;
}

static int nvs_get_blob(nvs_handle_t handle, const char *key, void *seed, size_t *size)
{
    assert(handle == 1 && strcmp(key, "key") == 0 && *size == 32);
    memset(seed, 1, *size);
    return ESP_OK;
}

static void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
static void esp_fill_random(void *seed, size_t size) { (void)seed; (void)size; assert(false); }
static int nvs_set_blob(nvs_handle_t handle, const char *key, const void *seed, size_t size)
{
    (void)handle; (void)key; (void)seed; (void)size;
    assert(false);
    return -1;
}
static int nvs_commit(nvs_handle_t handle) { (void)handle; assert(false); return -1; }
static int psa_crypto_init(void) { return PSA_SUCCESS; }
static int psa_hash_compute(int alg, const uint8_t *data, size_t size,
                            uint8_t *digest, size_t capacity, size_t *len)
{
    assert(alg == PSA_ALG_SHA_256 && capacity == crypto_hash_sha256_BYTES);
    *len = crypto_hash_sha256_BYTES;
    return crypto_hash_sha256(digest, data, size);
}
#define ESP_LOGI(...) ((void)0)

static bool send_record(uint16_t service, uint16_t kind, const void *payload, size_t size)
{
    assert(service == SESSION && kind == READY_MSG && size == sizeof(sent));
    memcpy(sent, payload, size);
    return true;
}

static bool set_phase(uint32_t generation, uint8_t phase)
{
    assert(generation == 1 && phase == ACTIVE);
    return true;
}

#include "usb_video_signing.inc"

int main(void)
{
    /* The unchanged OS Services sender verifies this session.ready fixture. */
    static const char expected[] =
        "0500000007070707070707070707070707070707"
        "8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c"
        "d0026ddb131d87d728505167b1da2caec267d5db0c62aab8416bcfefbd86234d"
        "6df84ce30155302f43ae2fbec53e6e2255fafd7ff834bd0056d46ba882672d08";
    session_t session = {.generation = 1};
    uint8_t *hello = session.header + HEADER_LEN;
    hello[0] = 5;
    memset(session.token, 7, sizeof(session.token));
    memcpy(hello + 4, session.token, sizeof(session.token));
    memset(hello + 20, 9, HELLO_LEN - 20);

    assert(store_identity());
    assert(strcmp(fingerprint, "34:75:0F:98:BD:59:FC:FC") == 0);
    assert(authorize(&session));
    char actual[2 * sizeof(sent) + 1];
    assert(strcmp(sodium_bin2hex(actual, sizeof(actual), sent, sizeof(sent)), expected) == 0);

    hello[HELLO_LEN - 1] ^= 1;
    assert(authorize(&session));
    assert(strncmp(sodium_bin2hex(actual, sizeof(actual), sent, sizeof(sent)), expected, 104) == 0);
    assert(strcmp(actual, expected) != 0);
    puts("USB video pairing fingerprint and signed session fixture passed");
}
