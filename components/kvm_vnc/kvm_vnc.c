/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A small RFB 3.8 server. A viewer that knows Open H.264 gets the browser's
 * H.264 stream as it is. Others get Tight/JPEG: the capture pipeline's JPEG
 * as-is, or a snapshot when the pipeline makes H.264.
 */
#include "kvm_vnc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "capture.h"
#include "http_server.h"
#include "kvm_auth.h"
#include "kvm_settings.h"
#include "kvm_tls.h"
#include "vencrypt_protocol.h"
#include "usb_hid.h"
#include "video_frame.h"
#include "wifi.h"

static const char *TAG = "vnc";

#define VNC_TASK_STACK 8192
#define VNC_TASK_PRIO 5
#define VNC_CLIENT_TEXT_MAX 4096
#define VNC_READ_TIMEOUT_MS 1000
#define VNC_SEND_TIMEOUT_MS 3000
#define VNC_HANDSHAKE_TIMEOUT_MS 10000

static volatile uint32_t s_generation;

/* The connected client, for the console's list of viewers. */
static portMUX_TYPE s_viewer_mu = portMUX_INITIALIZER_UNLOCKED;
static kvm_web_viewer_t s_viewer;
static bool s_viewer_on;

static void viewer_set(const char *ip, bool encrypted)
{
    portENTER_CRITICAL(&s_viewer_mu);
    strlcpy(s_viewer.ip, ip, sizeof(s_viewer.ip));
    s_viewer.kind = "vnc";
    s_viewer.encrypted = encrypted;
    s_viewer.since_us = esp_timer_get_time();
    s_viewer_on = true;
    portEXIT_CRITICAL(&s_viewer_mu);
}

static void viewer_clear(void)
{
    portENTER_CRITICAL(&s_viewer_mu);
    s_viewer_on = false;
    portEXIT_CRITICAL(&s_viewer_mu);
}

static int viewers_list(kvm_web_viewer_t *out, int max)
{
    int n = 0;
    portENTER_CRITICAL(&s_viewer_mu);
    if (s_viewer_on && max > 0) {
        out[n++] = s_viewer;
    }
    portEXIT_CRITICAL(&s_viewer_mu);
    return n;
}
static TaskHandle_t s_task;

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
} vnc_tls_t;

typedef struct {
    int fd;
    vnc_tls_t *tls;
} vnc_conn_t;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    const int fd = *(const int *)ctx;
    const int n = send(fd, buf, len, 0);
    if (n > 0) return n;
    if (n == 0 || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    const int fd = *(const int *)ctx;
    const int n = recv(fd, buf, len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool recv_exact_until(vnc_conn_t *conn, void *data, size_t len, uint32_t generation,
                             int64_t deadline_us)
{
    uint8_t *p = data;
    while (len) {
        if (deadline_us && esp_timer_get_time() >= deadline_us) {
            return false;
        }
        const int n = conn->tls ? mbedtls_ssl_read(&conn->tls->ssl, p, len)
                                : recv(conn->fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == 0 || (conn->tls
                           ? (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE)
                           : (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))) {
            return false;
        }
        if (generation != s_generation) {
            return false;
        }
        vTaskDelay(1);
    }
    return generation == s_generation;
}

static bool recv_exact(vnc_conn_t *conn, void *data, size_t len, uint32_t generation)
{
    return recv_exact_until(conn, data, len, generation, 0);
}

static bool send_all(vnc_conn_t *conn, const void *data, size_t len, uint32_t generation)
{
    const uint8_t *p = data;
    int64_t last_progress_us = esp_timer_get_time();
    while (len) {
        const size_t chunk = conn->tls && len > 4096 ? 4096 : len;
        const int n = conn->tls ? mbedtls_ssl_write(&conn->tls->ssl, p, chunk)
                                : send(conn->fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            last_progress_us = esp_timer_get_time();
            continue;
        }
        if (conn->tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            if (generation != s_generation ||
                esp_timer_get_time() - last_progress_us >=
                    (int64_t)VNC_SEND_TIMEOUT_MS * 1000) {
                return false;
            }
            vTaskDelay(1);
            continue;
        }
        if (!conn->tls && n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return generation == s_generation;
}

static bool send_u32(vnc_conn_t *conn, uint32_t value, uint32_t generation)
{
    uint8_t b[4];
    put_be32(b, value);
    return send_all(conn, b, sizeof(b), generation);
}

/* DES tables are the bit positions from FIPS 46-3, numbered from the MSB. */
static const uint8_t s_ip[64] = {
    58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,
    64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7
};
static const uint8_t s_fp[64] = {
    40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,
    37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,
    34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25
};
static const uint8_t s_e[48] = {
    32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1
};
static const uint8_t s_p[32] = {
    16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,
    2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25
};
static const uint8_t s_pc1[56] = {
    57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,
    60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,
    29,21,13,5,28,20,12,4
};
static const uint8_t s_pc2[48] = {
    14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32
};
static const uint8_t s_shifts[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t s_sbox[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
     4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
     0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
     13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
     10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
     4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
     9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
     1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
     7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

static uint64_t permute(uint64_t in, const uint8_t *table, unsigned n, unsigned width)
{
    uint64_t out = 0;
    for (unsigned i = 0; i < n; i++) {
        out = (out << 1) | ((in >> (width - table[i])) & 1u);
    }
    return out;
}

static uint32_t rol28(uint32_t v, unsigned n)
{
    return ((v << n) | (v >> (28u - n))) & 0x0fffffffu;
}

static uint64_t des_encrypt(uint64_t block, const uint8_t key[8])
{
    uint64_t raw_key = 0;
    for (unsigned i = 0; i < 8; i++) {
        raw_key = (raw_key << 8) | key[i];
    }
    const uint64_t selected = permute(raw_key, s_pc1, 56, 64);
    uint32_t c = (uint32_t)(selected >> 28);
    uint32_t d = (uint32_t)selected & 0x0fffffffu;
    uint64_t round_key[16];
    for (unsigned i = 0; i < 16; i++) {
        c = rol28(c, s_shifts[i]);
        d = rol28(d, s_shifts[i]);
        round_key[i] = permute(((uint64_t)c << 28) | d, s_pc2, 48, 56);
    }

    const uint64_t ip = permute(block, s_ip, 64, 64);
    uint32_t left = (uint32_t)(ip >> 32);
    uint32_t right = (uint32_t)ip;
    for (unsigned round = 0; round < 16; round++) {
        const uint64_t expanded = permute(right, s_e, 48, 32) ^ round_key[round];
        uint32_t s = 0;
        for (unsigned box = 0; box < 8; box++) {
            const uint8_t six = (uint8_t)((expanded >> (42u - 6u * box)) & 0x3fu);
            const uint8_t row = (uint8_t)(((six & 0x20u) >> 4) | (six & 1u));
            const uint8_t col = (uint8_t)((six >> 1) & 0x0fu);
            s = (s << 4) | s_sbox[box][row * 16u + col];
        }
        const uint32_t f = (uint32_t)permute(s, s_p, 32, 32);
        const uint32_t next = left ^ f;
        left = right;
        right = next;
    }
    return permute(((uint64_t)right << 32) | left, s_fp, 64, 64);
}

static uint8_t reverse_bits(uint8_t v)
{
    v = (uint8_t)(((v & 0x55u) << 1) | ((v >> 1) & 0x55u));
    v = (uint8_t)(((v & 0x33u) << 2) | ((v >> 2) & 0x33u));
    return (uint8_t)((v << 4) | (v >> 4));
}

static void vnc_tls_free(vnc_conn_t *conn)
{
    if (!conn->tls) return;
    mbedtls_ssl_free(&conn->tls->ssl);
    mbedtls_ssl_config_free(&conn->tls->config);
    mbedtls_x509_crt_free(&conn->tls->cert);
    mbedtls_pk_free(&conn->tls->key);
    free(conn->tls);
    conn->tls = NULL;
}

typedef struct {
    vnc_conn_t *conn;
    uint32_t generation;
    int64_t deadline_us;
} vencrypt_socket_t;

static int vencrypt_socket_send(void *ctx, const uint8_t *data, size_t len)
{
    vencrypt_socket_t *s = ctx;
    const int n = send(s->conn->fd, data, len, 0);
    if (n > 0) return n;
    if (n == 0) return VENC_IO_WANT_WRITE;
    if (errno == EINTR) return VENC_IO_INTERRUPTED;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return VENC_IO_WANT_WRITE;
    return VENC_IO_ERROR;
}

static int vencrypt_socket_recv(void *ctx, uint8_t *data, size_t len)
{
    vencrypt_socket_t *s = ctx;
    const int n = recv(s->conn->fd, data, len, 0);
    if (n > 0) return n;
    if (n == 0) return VENC_IO_CLOSED;
    if (errno == EINTR) return VENC_IO_INTERRUPTED;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return VENC_IO_WANT_READ;
    return VENC_IO_ERROR;
}

static bool vencrypt_socket_wait(void *ctx, bool writing)
{
    vencrypt_socket_t *s = ctx;
    for (;;) {
        if (s->generation != s_generation || esp_timer_get_time() >= s->deadline_us) {
            return false;
        }
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(s->conn->fd, &ready);
        const int64_t remaining_us = s->deadline_us - esp_timer_get_time();
        const int64_t wait_us = remaining_us < 100000 ? remaining_us : 100000;
        struct timeval wait = {.tv_sec = (long)(wait_us / 1000000),
                               .tv_usec = (long)(wait_us % 1000000)};
        const int selected = select(s->conn->fd + 1, writing ? NULL : &ready,
                                    writing ? &ready : NULL, NULL, &wait);
        if (selected > 0) return s->generation == s_generation;
        if (selected < 0 && errno != EINTR) return false;
    }
}

/*
 * The identity comes from NVS, a flash read, and the cache is off while the
 * flash is read: a task whose stack is in PSRAM must not do it (IDF asserts in
 * spi_flash_disable_interrupts_caches_and_other_cpu). The VNC task's stack is
 * in PSRAM, so a short task with an internal stack does the read.
 */
typedef struct {
    kvm_tls_identity_t *out;
    esp_err_t err;
    SemaphoreHandle_t done;
} identity_job_t;

static void identity_task(void *arg)
{
    identity_job_t *job = arg;
    job->err = kvm_tls_identity_get(job->out, true);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t identity_get(kvm_tls_identity_t *out)
{
    identity_job_t job = {.out = out, .err = ESP_FAIL, .done = xSemaphoreCreateBinary()};
    if (!job.done) {
        return ESP_ERR_NO_MEM;
    }
    /* As much stack as the HTTPS server task, which makes the same call: it
     * signs a new leaf when the address changed. */
    if (xTaskCreate(identity_task, "vnc_id", 12 * 1024, &job, VNC_TASK_PRIO, NULL) != pdPASS) {
        vSemaphoreDelete(job.done);
        return ESP_ERR_NO_MEM;
    }
    (void)xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.err;
}

static bool vnc_tls_start(vnc_conn_t *conn, uint32_t generation, int64_t deadline_us)
{
    kvm_tls_identity_t identity;
    if (identity_get(&identity) != ESP_OK) {
        ESP_LOGE(TAG, "could not load the device TLS identity for VeNCrypt");
        return false;
    }

    vnc_tls_t *tls = calloc(1, sizeof(*tls));
    if (!tls) {
        kvm_tls_identity_free(&identity);
        ESP_LOGE(TAG, "VeNCrypt TLS context allocation failed");
        return false;
    }
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->config);
    mbedtls_x509_crt_init(&tls->cert);
    mbedtls_pk_init(&tls->key);

    int rc = mbedtls_x509_crt_parse(&tls->cert, (const unsigned char *)identity.cert_pem,
                                    identity.cert_len);
    if (rc == 0) {
        rc = mbedtls_pk_parse_key(&tls->key, (const unsigned char *)identity.key_pem,
                                  identity.key_len, NULL, 0);
    }
    if (rc == 0) {
        rc = mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_SERVER,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (rc == 0) {
        rc = mbedtls_ssl_conf_own_cert(&tls->config, &tls->cert, &tls->key);
    }
    if (rc == 0) {
        rc = mbedtls_ssl_setup(&tls->ssl, &tls->config);
    }
    kvm_tls_identity_free(&identity);
    if (rc != 0) {
        ESP_LOGE(TAG, "VeNCrypt TLS setup failed (-0x%04x)", (unsigned)-rc);
        conn->tls = tls;
        vnc_tls_free(conn);
        return false;
    }

    conn->tls = tls;
    mbedtls_ssl_set_bio(&tls->ssl, &conn->fd, tls_send, tls_recv, NULL);
    ESP_LOGD(TAG, "VeNCrypt TLS context initialized");
    ESP_LOGD(TAG, "VeNCrypt TLS handshake starting");
    do {
        rc = mbedtls_ssl_handshake(&tls->ssl);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (generation != s_generation || esp_timer_get_time() >= deadline_us) {
                rc = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            vTaskDelay(1);
        }
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (rc != 0) {
        ESP_LOGW(TAG, "VeNCrypt TLS handshake failed (-0x%04x)", (unsigned)-rc);
        vnc_tls_free(conn);
        return false;
    }
    ESP_LOGD(TAG, "VeNCrypt TLS handshake complete");
    return generation == s_generation;
}

typedef enum {
    VNC_AUTH_IO_ERROR,
    VNC_AUTH_BAD_PASSWORD,
    VNC_AUTH_OK,
} vnc_auth_result_t;

static bool vnc_response_matches(const char *password, const uint8_t challenge[16],
                                 const uint8_t response[16])
{
    if (!password) return false;
    const size_t len = strlen(password);
    if (!len || len > 8) return false;

    uint8_t key[8] = {0};
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)password[i];
        if (ch < 0x20 || ch > 0x7e) {
            memset(key, 0, sizeof(key));
            return false;
        }
        key[i] = reverse_bits(ch);
    }

    uint8_t expected[16];
    for (unsigned i = 0; i < 2; i++) {
        uint64_t block = 0;
        for (unsigned j = 0; j < 8; j++) {
            block = (block << 8) | challenge[i * 8 + j];
        }
        const uint64_t encrypted = des_encrypt(block, key);
        for (unsigned j = 0; j < 8; j++) {
            expected[i * 8 + j] = (uint8_t)(encrypted >> (56u - 8u * j));
        }
    }
    uint8_t diff = 0;
    for (unsigned i = 0; i < sizeof(expected); i++) {
        diff |= expected[i] ^ response[i];
    }
    memset(key, 0, sizeof(key));
    memset(expected, 0, sizeof(expected));
    return diff == 0;
}

static vnc_auth_result_t vnc_auth(vnc_conn_t *conn, uint32_t generation, int64_t deadline_us)
{
    const char *password = kvm_setting_str("vnc_pass");
    if (!password || !password[0] || strlen(password) > 8) {
        return VNC_AUTH_IO_ERROR;
    }
    uint8_t challenge[16], response[16];
    esp_fill_random(challenge, sizeof(challenge));
    kvm_auth_attempt_wait();
    if (!send_all(conn, challenge, sizeof(challenge), generation) ||
        !recv_exact_until(conn, response, sizeof(response), generation, deadline_us)) {
        return VNC_AUTH_IO_ERROR;
    }

    char tickets[KVM_AUTH_VNC_TICKET_SLOTS][KVM_AUTH_VNC_TICKET_CHARS + 1] = {{0}};
    const size_t count = kvm_auth_vnc_tickets_copy(tickets, KVM_AUTH_VNC_TICKET_SLOTS);
    bool ticket_authenticated = false;
    for (size_t i = 0; i < count; i++) {
        if (vnc_response_matches(tickets[i], challenge, response)) {
            ticket_authenticated = kvm_auth_vnc_ticket_consume(tickets[i]);
            break;
        }
    }
    memset(tickets, 0, sizeof(tickets));
    const bool password_authenticated = !ticket_authenticated &&
                                        vnc_response_matches(password, challenge, response);
    memset(challenge, 0, sizeof(challenge));
    memset(response, 0, sizeof(response));
    return ticket_authenticated || password_authenticated ? VNC_AUTH_OK
                                                           : VNC_AUTH_BAD_PASSWORD;
}

static bool rfb_handshake(vnc_conn_t *conn, uint32_t generation, int64_t deadline_us,
                          uint16_t *width, uint16_t *height)
{
    static const uint8_t version[] = "RFB 003.008\n";
    uint8_t client_version[12];
    if (!send_all(conn, version, 12, generation) ||
        !recv_exact_until(conn, client_version, sizeof(client_version), generation, deadline_us)) {
        return false;
    }
    (void)client_version;
    static const uint8_t security_types[] = {2, 19, 2}; /* Prefer VeNCrypt; keep legacy clients working. */
    uint8_t selected;
    if (!send_all(conn, security_types, sizeof(security_types), generation) ||
        !recv_exact_until(conn, &selected, 1, generation, deadline_us)) {
        return false;
    }
    if (selected != 2 && selected != 19) {
        ESP_LOGW(TAG, "client selected unsupported RFB security type %u", (unsigned)selected);
        return false;
    }
    ESP_LOGD(TAG, "client selected RFB security type %u%s", (unsigned)selected,
             selected == 19 ? " (VeNCrypt)" : " (legacy VNC auth)");

    if (selected == 19) {
        /* VeNCrypt 0.2 offers certificate-authenticated X509Vnc first, then
           TLSVnc for viewers that do not validate the server certificate. */
        vencrypt_socket_t socket = {.conn = conn, .generation = generation,
                                    .deadline_us = deadline_us};
        const vencrypt_io_t io = {.ctx = &socket, .send = vencrypt_socket_send,
                                  .recv = vencrypt_socket_recv, .wait = vencrypt_socket_wait};
        uint32_t subtype_id;
        const vencrypt_result_t result = vencrypt_server_negotiate(&io, &subtype_id);
        if (result != VENC_RESULT_OK) {
            ESP_LOGW(TAG, "VeNCrypt negotiation failed (%s)",
                     result == VENC_RESULT_BAD_VERSION ? "unsupported version" :
                     result == VENC_RESULT_BAD_SUBTYPE ? "unsupported subtype" : "I/O error");
            return false;
        }
        ESP_LOGD(TAG, "client selected VeNCrypt subtype %u", (unsigned)subtype_id);
        ESP_LOGD(TAG, "VeNCrypt success status sent; starting TLS");
        if (!vnc_tls_start(conn, generation, deadline_us)) {
            return false;
        }
    }

    const vnc_auth_result_t auth = vnc_auth(conn, generation, deadline_us);
    if (auth != VNC_AUTH_OK) {
        if (auth == VNC_AUTH_BAD_PASSWORD) {
            ESP_LOGW(TAG, "VNC password authentication rejected");
            kvm_auth_attempt_failed();
        } else {
            ESP_LOGW(TAG, "VNC authentication exchange failed");
        }
        (void)send_u32(conn, 1, generation);
        static const char reason[] = "VNC password is invalid or authentication failed";
        (void)send_u32(conn, sizeof(reason) - 1, generation);
        (void)send_all(conn, reason, sizeof(reason) - 1, generation);
        return false;
    }
    kvm_auth_attempt_succeeded();
    ESP_LOGD(TAG, "VNC authentication succeeded");
    if (!send_u32(conn, 0, generation)) {
        ESP_LOGW(TAG, "could not send successful RFB SecurityResult");
        return false;
    }
    uint8_t shared;
    if (!recv_exact_until(conn, &shared, 1, generation, deadline_us)) {
        ESP_LOGW(TAG, "client closed before RFB ClientInit");
        return false;
    }
    (void)shared;

    kvm_video_status_t status;
    capture_status_get(&status);
    *width = (uint16_t)(status.hres ? status.hres : 1);
    *height = (uint16_t)(status.vres ? status.vres : 1);
    uint8_t init[24] = {0};
    put_be16(init, *width);
    put_be16(init + 2, *height);
    /* 32-bit true colour, 8 bits per channel, RGB in the normal order. */
    init[4] = 32;
    init[5] = 24;
    init[6] = 0; /* little endian pixels */
    init[7] = 1; /* true colour */
    put_be16(init + 8, 255);
    put_be16(init + 10, 255);
    put_be16(init + 12, 255);
    init[14] = 16;
    init[15] = 8;
    init[16] = 0;
    /* The device's name, so a client with several of them tells them apart.
     * RFB counts the bytes; no terminator goes on the wire. */
    const char *host = kvm_setting_str("net_hostname");
    const char *name = host && host[0] ? host : "ESP-KVM";
    const size_t name_len = strnlen(name, 63);
    put_be32(init + 20, (uint32_t)name_len);
    if (!send_all(conn, init, sizeof(init), generation) ||
        !send_all(conn, name, name_len, generation)) {
        ESP_LOGW(TAG, "failed to send RFB ServerInit");
        return false;
    }
    ESP_LOGD(TAG, "RFB ServerInit sent (%ux%u)", (unsigned)*width, (unsigned)*height);
    return true;
}

static bool send_empty_update(vnc_conn_t *conn, uint32_t generation)
{
    const uint8_t empty[] = {0, 0, 0, 0};
    return send_all(conn, empty, sizeof(empty), generation);
}

/* Tell the client the screen changed size. Without DesktopSize it cannot be
 * told, so it gets an empty update and keeps its old size. */
static bool send_resize(vnc_conn_t *conn, uint32_t generation, bool desktop_size,
                        uint16_t w, uint16_t h, uint16_t *width, uint16_t *height)
{
    if (!desktop_size) return send_empty_update(conn, generation);
    const uint8_t update[] = {0, 0, 0, 1};
    uint8_t rect[12] = {0};
    put_be16(rect + 4, w);
    put_be16(rect + 6, h);
    put_be32(rect + 8, (uint32_t)-223); /* DesktopSize pseudo-encoding */
    if (!send_all(conn, update, sizeof(update), generation) ||
        !send_all(conn, rect, sizeof(rect), generation)) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

/*
 * Open H.264 (encoding 50, from PiKVM): the browser's own H.264 stream goes to
 * the viewer as it is, so a VNC client costs no second encoder and gets the
 * browser's frame rate. A rectangle is a 32-bit length, 32-bit flags and an
 * Annex-B access unit. Flag 1 tells the viewer to start a new decoder.
 */
#define VNC_ENC_H264 50
#define VNC_H264_RESET_CONTEXT 1u
#define VNC_KEYFRAME_WAIT_MS 1800

typedef struct {
    bool started;
    bool need_key; /* a delta now would decode against nothing */
    bool reset;    /* the next rectangle starts a new decoder */
    uint32_t last_seq;
    uint8_t *buf; /* a copy, so a slow link does not hold a frame slot */
    size_t cap;
} vnc_h264_t;

static void h264_restart(vnc_h264_t *s)
{
    s->started = true;
    s->need_key = true;
    s->reset = true;
    video_frame_request_keyframe();
}

/* A non-incremental request needs no IDR: the viewer's decoder already holds
 * the whole screen, and the next delta updates all of it. Restarting on every
 * one would let a client fill the browser's stream with keyframes. */
static bool rfb_send_h264(vnc_conn_t *conn, uint32_t generation, uint16_t *width, uint16_t *height,
                          bool desktop_size, vnc_h264_t *s)
{
    if (!s->started) h264_restart(s);

    kvm_video_status_t status;
    capture_status_get(&status);
    const uint16_t w = (uint16_t)status.hres;
    const uint16_t h = (uint16_t)status.vres;
    if (w && h && (w != *width || h != *height)) {
        /* The encoder reopens at the new size and starts with an IDR. A client
         * without DesktopSize stays at its old size and gets empty updates,
         * so it must not ask for a keyframe on every request. */
        if (desktop_size) h264_restart(s);
        return send_resize(conn, generation, desktop_size, w, h, width, height);
    }

    /* Waiting for a keyframe skips the deltas before it, within a bound:
     * meanwhile nothing reads the client's keys. */
    const int64_t deadline = esp_timer_get_time() +
                             (int64_t)(s->need_key ? VNC_KEYFRAME_WAIT_MS : VNC_READ_TIMEOUT_MS) * 1000;
    video_frame_ref_t ref;
    for (;;) {
        const int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        if (left_ms <= 0 || !video_frame_wait_new(s->last_seq, (uint32_t)left_ms)) {
            return send_empty_update(conn, generation);
        }
        if (!video_frame_acquire(&ref)) {
            /* The front slot is set aside (a codec reinit). last_seq stays the
             * last frame sent, so the gap check below still sees the skip. */
            vTaskDelay(1);
            continue;
        }
        if (ref.payload != VIDEO_PAYLOAD_H264) {
            video_frame_release(&ref);
            return send_empty_update(conn, generation);
        }
        if (ref.seq == s->last_seq) { /* already sent */
            video_frame_release(&ref);
            vTaskDelay(1);
            continue;
        }
        if (s->need_key && !ref.keyframe) {
            s->last_seq = ref.seq;
            video_frame_release(&ref);
            continue;
        }
        break;
    }
    /* A skipped delta breaks the chain: keep sending, the picture smudges
     * until the repair IDR, as in the browser. */
    if (!s->need_key && !ref.keyframe && (uint32_t)(ref.seq - s->last_seq) > 1u) {
        video_frame_request_repair_keyframe();
    }
    if (s->cap < ref.len) {
        uint8_t *grown = heap_caps_realloc(s->buf, ref.len, MALLOC_CAP_SPIRAM);
        if (!grown) {
            video_frame_release(&ref);
            return send_empty_update(conn, generation);
        }
        s->buf = grown;
        s->cap = ref.len;
    }
    memcpy(s->buf, ref.data, ref.len);
    const size_t len = ref.len;
    const uint32_t seq = ref.seq;
    const bool key = ref.keyframe;
    video_frame_release(&ref);

    uint8_t head[4 + 12 + 8] = {0, 0, 0, 1};
    put_be16(head + 4 + 4, w ? w : *width);
    put_be16(head + 4 + 6, h ? h : *height);
    put_be32(head + 4 + 8, VNC_ENC_H264);
    put_be32(head + 16, (uint32_t)len);
    put_be32(head + 20, s->reset ? VNC_H264_RESET_CONTEXT : 0);
    if (!send_all(conn, head, sizeof(head), generation) || !send_all(conn, s->buf, len, generation)) {
        return false;
    }
    s->last_seq = seq;
    if (key) s->need_key = false;
    s->reset = false;
    return true;
}

static bool rfb_send_update(vnc_conn_t *conn, uint32_t generation, uint16_t *width, uint16_t *height,
                            bool desktop_size, bool incremental, bool *has_frame,
                            uint32_t *last_seq, bool *stream_open)
{
    if (incremental && *has_frame &&
        !video_frame_wait_new(*last_seq, VNC_READ_TIMEOUT_MS)) {
        return send_empty_update(conn, generation);
    }

    const uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    uint32_t seq = video_frame_seq();
    video_frame_ref_t ref = {.slot = -1};
    const bool mjpeg = video_frame_payload() == VIDEO_PAYLOAD_JPEG;
    bool held = false;
    if (mjpeg) {
        if (!video_frame_acquire(&ref)) {
            const uint32_t seen = video_frame_seq();
            if (!video_frame_wait_new(seen, 1800) || !video_frame_acquire(&ref)) {
                return send_empty_update(conn, generation);
            }
        }
        held = true;
        if (ref.payload != VIDEO_PAYLOAD_JPEG ||
            (incremental && *has_frame && ref.seq == *last_seq)) {
            video_frame_release(&ref);
            return send_empty_update(conn, generation);
        }
        jpeg = ref.data;
        jpeg_len = ref.len;
        seq = ref.seq;
        if (*stream_open) {
            capture_vnc_snapshot_end();
            *stream_open = false;
        }
    } else {
        if (!*stream_open) {
            if (capture_vnc_snapshot_begin() != ESP_OK) {
                return send_empty_update(conn, generation);
            }
            *stream_open = true;
        }
        const esp_err_t err = capture_vnc_snapshot_next(&jpeg, &jpeg_len, 1800);
        if (err != ESP_OK || !jpeg || !jpeg_len) {
            return send_empty_update(conn, generation);
        }
        seq = video_frame_seq();
    }
    if (!jpeg || !jpeg_len || jpeg_len > 0x3fffffu) {
        if (held) video_frame_release(&ref);
        return send_empty_update(conn, generation);
    }

    kvm_video_status_t status;
    capture_status_get(&status);
    const uint16_t w = (uint16_t)status.hres;
    const uint16_t h = (uint16_t)status.vres;
    const bool resized = w && h && (w != *width || h != *height);
    if (resized) {
        const bool ok = send_resize(conn, generation, desktop_size, w, h, width, height);
        if (held) video_frame_release(&ref);
        if (ok) {
            *last_seq = seq;
            *has_frame = true;
        }
        return ok;
    }

    const uint8_t update[] = {0, 0, 0, 1};
    bool ok = send_all(conn, update, sizeof(update), generation);
    if (ok) {
        uint8_t rect[12] = {0};
        put_be16(rect + 4, w ? w : *width);
        put_be16(rect + 6, h ? h : *height);
        put_be32(rect + 8, 7); /* Tight encoding */
        const uint8_t jpeg_subencoding = 0x90; /* JPEG rectangle */
        ok = send_all(conn, rect, sizeof(rect), generation) &&
             send_all(conn, &jpeg_subencoding, 1, generation);
    }
    if (ok) {
        uint8_t compact[3];
        size_t n;
        if (jpeg_len < 128) {
            compact[0] = (uint8_t)jpeg_len;
            n = 1;
        } else if (jpeg_len < 16384) {
            compact[0] = (uint8_t)((jpeg_len & 0x7fu) | 0x80u);
            compact[1] = (uint8_t)(jpeg_len >> 7);
            n = 2;
        } else {
            compact[0] = (uint8_t)((jpeg_len & 0x7fu) | 0x80u);
            compact[1] = (uint8_t)(((jpeg_len >> 7) & 0x7fu) | 0x80u);
            compact[2] = (uint8_t)(jpeg_len >> 14);
            n = 3;
        }
        ok = send_all(conn, compact, n, generation) && send_all(conn, jpeg, jpeg_len, generation);
    }
    if (held) video_frame_release(&ref);
    if (ok) {
        *last_seq = seq;
        *has_frame = true;
    }
    return ok;
}

static uint8_t keysym_usage(uint32_t key)
{
    if (key >= 'a' && key <= 'z') return (uint8_t)(0x04 + key - 'a');
    if (key >= 'A' && key <= 'Z') return (uint8_t)(0x04 + key - 'A');
    if (key >= '1' && key <= '9') return (uint8_t)(0x1e + key - '1');
    if (key == '0') return 0x27;
    switch (key) {
    case 0xff08: return 0x2a; /* Backspace */
    case 0xff09: return 0x2b; /* Tab */
    case 0xff0d: return 0x28; /* Return */
    case 0xff1b: return 0x29; /* Escape */
    case ' ': return 0x2c;
    case '-': case '_': return 0x2d;
    case '=': case '+': return 0x2e;
    case '[': case '{': return 0x2f;
    case ']': case '}': return 0x30;
    case '\\': case '|': return 0x31;
    case ';': case ':': return 0x33;
    case '\'': case '"': return 0x34;
    case '`': case '~': return 0x35;
    case ',': case '<': return 0x36;
    case '.': case '>': return 0x37;
    case '/': case '?': return 0x38;
    case 0xffe5: return 0x39; /* Caps Lock */
    case 0xffbe: return 0x3a; case 0xffbf: return 0x3b;
    case 0xffc0: return 0x3c; case 0xffc1: return 0x3d;
    case 0xffc2: return 0x3e; case 0xffc3: return 0x3f;
    case 0xffc4: return 0x40; case 0xffc5: return 0x41;
    case 0xffc6: return 0x42; case 0xffc7: return 0x43;
    case 0xffc8: return 0x44; case 0xffc9: return 0x45;
    case 0xff50: return 0x4a; /* Home */
    case 0xff51: return 0x50; /* Left */
    case 0xff52: return 0x52; /* Up */
    case 0xff53: return 0x4f; /* Right */
    case 0xff54: return 0x51; /* Down */
    case 0xff55: return 0x4b; /* Page Up */
    case 0xff56: return 0x4e; /* Page Down */
    case 0xff57: return 0x4d; /* End */
    case 0xff63: return 0x49; /* Insert */
    case 0xffff: return 0x4c; /* Delete */
    default: return 0;
    }
}

static uint8_t keysym_modifier(uint32_t key)
{
    switch (key) {
    case 0xffe1: return 1u << 1; /* left shift */
    case 0xffe2: return 1u << 5; /* right shift */
    case 0xffe3: return 1u << 0; /* left control */
    case 0xffe4: return 1u << 4; /* right control */
    case 0xffe9: return 1u << 2; /* left alt */
    case 0xffea: return 1u << 6; /* right alt */
    case 0xffeb: return 1u << 3; /* left GUI */
    case 0xffec: return 1u << 7; /* right GUI */
    default: return 0;
    }
}

typedef struct {
    uint32_t keysyms[16];
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
    bool tight;
    bool h264;
    bool desktop_size;
} client_state_t;

static void send_keyboard_state(const client_state_t *state)
{
    uint8_t modifier = 0, keys[6] = {0};
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (!state->keysyms[i]) continue;
        modifier |= keysym_modifier(state->keysyms[i]);
        const uint8_t usage = keysym_usage(state->keysyms[i]);
        if (!usage) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < n; j++) duplicate |= keys[j] == usage;
        if (!duplicate && n < sizeof(keys)) keys[n++] = usage;
    }
    usb_hid_vnc_keyboard(modifier, keys);
}

static void client_key(client_state_t *state, bool down, uint32_t keysym)
{
    int found = -1, empty = -1;
    for (unsigned i = 0; i < 16; i++) {
        if (state->keysyms[i] == keysym) found = (int)i;
        if (!state->keysyms[i] && empty < 0) empty = (int)i;
    }
    if (down && found < 0 && empty >= 0) state->keysyms[empty] = keysym;
    if (!down && found >= 0) state->keysyms[found] = 0;
    send_keyboard_state(state);
}

static void client_pointer(client_state_t *state, uint8_t mask, uint16_t x, uint16_t y)
{
    kvm_video_status_t status;
    capture_status_get(&status);
    const uint32_t w = status.hres ? status.hres : 1;
    const uint32_t h = status.vres ? status.vres : 1;
    if (x >= w) x = (uint16_t)(w - 1);
    if (y >= h) y = (uint16_t)(h - 1);
    uint8_t buttons = 0;
    if (mask & 1u) buttons |= 1u;
    if (mask & 2u) buttons |= 4u; /* RFB middle becomes HID middle */
    if (mask & 4u) buttons |= 2u; /* RFB right becomes HID right */
    const int8_t wheel = (mask & 8u) && !(state->buttons & 8u) ? 1 :
                         (mask & 16u) && !(state->buttons & 16u) ? -1 : 0;
    const int8_t pan = (mask & 32u) && !(state->buttons & 32u) ? 1 :
                       (mask & 64u) && !(state->buttons & 64u) ? -1 : 0;
    state->buttons = mask;
    state->x = x;
    state->y = y;
    usb_hid_vnc_mouse_abs(buttons, (uint16_t)((uint32_t)x * 32767u / (w > 1 ? w - 1 : 1)),
                          (uint16_t)((uint32_t)y * 32767u / (h > 1 ? h - 1 : 1)), wheel, pan);
}

static bool discard_bytes(vnc_conn_t *conn, uint32_t len, uint32_t generation)
{
    uint8_t buf[128];
    while (len) {
        const size_t n = len > sizeof(buf) ? sizeof(buf) : len;
        if (!recv_exact(conn, buf, n, generation)) return false;
        len -= (uint32_t)n;
    }
    return true;
}

static void set_client_timeouts(int fd);

/* The listening socket, so a session can see someone else knocking. */
static int s_listener = -1;

/* Accept one connection: -1 when there was none or it came over the hotspot
 * with that not allowed. */
static int accept_client(int listener, char *ip, size_t ip_cap)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
    if (client < 0) return -1;
    set_client_timeouts(client);
    struct sockaddr_in local = {0};
    socklen_t local_len = sizeof(local);
    if (!kvm_setting_bool("vnc_hotspot") &&
        getsockname(client, (struct sockaddr *)&local, &local_len) == 0 &&
        kvm_wifi_is_ap_address(local.sin_addr.s_addr)) {
        ESP_LOGI(TAG, "rejected client from hotspot (enable vnc_hotspot to allow it)");
        close(client);
        return -1;
    }
    inet_ntoa_r(peer.sin_addr, ip, ip_cap);
    return client;
}

static void conn_close(vnc_conn_t *conn)
{
    vnc_tls_free(conn);
    shutdown(conn->fd, SHUT_RDWR);
    close(conn->fd);
    conn->fd = -1;
}

/*
 * Wait up to @p timeout_ms for the next message. Meanwhile a new client may knock:
 * there is one session at a time, and a client left open somewhere would
 * otherwise keep everyone else out. 1: a message is there. 0: nothing yet.
 * 2: someone is at the listener.
 */
static int wait_message(const vnc_conn_t *conn, uint32_t timeout_ms)
{
    if (conn->tls && mbedtls_ssl_get_bytes_avail(&conn->tls->ssl) > 0) return 1;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(conn->fd, &fds);
    int top = conn->fd;
    if (s_listener >= 0) {
        FD_SET(s_listener, &fds);
        if (s_listener > top) top = s_listener;
    }
    struct timeval wait = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    if (select(top + 1, &fds, NULL, NULL, &wait) <= 0) return 0;
    if (s_listener >= 0 && FD_ISSET(s_listener, &fds)) return 2;
    return FD_ISSET(conn->fd, &fds) ? 1 : 0;
}

/*
 * An update request waits for a new frame, and on a still screen with MJPEG
 * none comes for a while. Meanwhile the keys and the mouse must still be read:
 * the move that would change the screen is one of them. Polls the socket once
 * a tick, and wakes at once for a frame. 1 or 2 as wait_message(), 0: a new
 * frame, or @p until passed.
 */
static int wait_frame_or_message(const vnc_conn_t *conn, uint32_t seen, int64_t until)
{
    for (;;) {
        const int ready = wait_message(conn, 0);
        if (ready) return ready;
        const int64_t left_ms = (until - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) return 0;
        if (video_frame_wait_new(seen, left_ms < 10 ? (uint32_t)left_ms : 10)) return 0;
    }
}

/* Closes its connection (and any it took over) before it returns. */
static bool client_loop(int fd, uint32_t generation, const char *first_ip)
{
    char ip[16];
    strlcpy(ip, first_ip, sizeof(ip));
    vnc_conn_t conn = {.fd = fd};
    uint16_t width, height;
    const int64_t handshake_deadline = esp_timer_get_time() +
                                       (int64_t)VNC_HANDSHAKE_TIMEOUT_MS * 1000;
    if (!rfb_handshake(&conn, generation, handshake_deadline, &width, &height)) {
        conn_close(&conn);
        return false;
    }
    viewer_set(ip, conn.tls != NULL);
    client_state_t state = {0};
    video_frame_viewer_enter();
    bool has_frame = false;
    bool stream_open = false;
    uint32_t last_seq = video_frame_seq();
    vnc_h264_t h264 = {0};
    bool h264_only_warned = false;
    /* An update request not answered yet: what it waits for, and until when. */
    bool pending = false;
    bool pending_incremental = false;
    bool pending_waits = false;
    uint32_t pending_seen = 0;
    int64_t pending_until = 0;
    bool alive = true;
    while (alive && generation == s_generation) {
        int ready;
        if (pending) {
            ready = pending_waits ? wait_frame_or_message(&conn, pending_seen, pending_until) : 0;
            if (ready == 0) {
                pending = false;
                const bool fresh = !pending_waits || video_frame_seq() != pending_seen;
                if (state.h264 && video_frame_payload() == VIDEO_PAYLOAD_H264) {
                    if (stream_open) {
                        capture_vnc_snapshot_end();
                        stream_open = false;
                    }
                    alive = fresh ? rfb_send_h264(&conn, generation, &width, &height, state.desktop_size, &h264)
                                  : send_empty_update(&conn, generation);
                } else if (state.tight && !capture_source_is_usb()) {
                    h264.started = false;
                    alive = fresh ? rfb_send_update(&conn, generation, &width, &height, state.desktop_size,
                                                    pending_incremental, &has_frame, &last_seq, &stream_open)
                                  : send_empty_update(&conn, generation);
                } else {
                    if (stream_open) {
                        capture_vnc_snapshot_end();
                        stream_open = false;
                    }
                    /* The client and source have no encoding in common. */
                    if (!state.tight && !h264_only_warned && !capture_source_is_usb()) {
                        ESP_LOGW(TAG, "client takes only H.264 and the video is MJPEG; switch the codec to H.264");
                        h264_only_warned = true;
                    }
                    alive = send_empty_update(&conn, generation);
                }
                continue;
            }
        } else {
            ready = wait_message(&conn, 1000);
        }
        if (ready == 0) continue;
        if (ready == 2) {
            /* The newcomer takes over only once it has signed in: an
             * unauthenticated connection must not be able to end a session. */
            char new_ip[16];
            const int nfd = accept_client(s_listener, new_ip, sizeof(new_ip));
            if (nfd < 0) continue;
            vnc_conn_t next = {.fd = nfd};
            uint16_t nw, nh;
            const int64_t deadline = esp_timer_get_time() + (int64_t)VNC_HANDSHAKE_TIMEOUT_MS * 1000;
            if (!rfb_handshake(&next, generation, deadline, &nw, &nh)) {
                ESP_LOGI(TAG, "client from %s did not sign in; %s keeps the session", new_ip, ip);
                conn_close(&next);
                continue;
            }
            ESP_LOGI(TAG, "client from %s took over from %s", new_ip, ip);
            usb_hid_vnc_release();
            conn_close(&conn);
            conn = next;
            width = nw;
            height = nh;
            strlcpy(ip, new_ip, sizeof(ip));
            memset(&state, 0, sizeof(state));
            pending = false;
            has_frame = false;
            last_seq = video_frame_seq();
            h264.started = false; /* the newcomer's decoder starts on an IDR */
            viewer_set(ip, conn.tls != NULL);
            continue;
        }
        uint8_t type;
        if (!recv_exact(&conn, &type, 1, generation)) {
            ESP_LOGD(TAG, "client closed while waiting for an RFB message");
            break;
        }
        switch (type) {
        case 0: { /* SetPixelFormat */
            uint8_t rest[19];
            alive = recv_exact(&conn, rest, sizeof(rest), generation);
            break;
        }
        case 1: { /* SetColourMapEntries */
            uint8_t rest[5];
            if (!recv_exact(&conn, rest, sizeof(rest), generation)) { alive = false; break; }
            const uint32_t bytes = (uint32_t)be16(rest + 3) * 6u;
            alive = discard_bytes(&conn, bytes, generation);
            break;
        }
        case 2: { /* SetEncodings */
            uint8_t header[3];
            if (!recv_exact(&conn, header, sizeof(header), generation)) { alive = false; break; }
            const uint16_t count = be16(header + 1);
            if (count > 64) {
                ESP_LOGW(TAG, "client sent invalid SetEncodings count %u", (unsigned)count);
                alive = false;
                break;
            }
            state.tight = false;
            state.h264 = false;
            state.desktop_size = false;
            /* The client picks the JPEG quality: a level 0-9 (-32..-23, the
             * TurboVNC table below) or an exact 0-100 (-512..-412). TigerVNC's
             * automatic mode lowers it on a slow link. */
            static const uint8_t level_quality[10] = {15, 29, 41, 42, 62, 77, 79, 86, 92, 100};
            int quality = 80, fine = -1;
            for (unsigned i = 0; i < count; i++) {
                uint8_t enc[4];
                if (!recv_exact(&conn, enc, sizeof(enc), generation)) { alive = false; break; }
                const int32_t value = (int32_t)be32(enc);
                if (value == 7) state.tight = true;
                if (value == VNC_ENC_H264) state.h264 = true;
                if (value == -223) state.desktop_size = true;
                if (value >= -32 && value <= -23) quality = level_quality[value + 32];
                if (value >= -512 && value <= -412) fine = value + 512;
            }
            capture_vnc_snapshot_quality(fine >= 0 ? fine : quality);
            ESP_LOGD(TAG, "client SetEncodings: %u entries, Tight=%s, H.264=%s, DesktopSize=%s, quality %d",
                     (unsigned)count, state.tight ? "yes" : "no", state.h264 ? "yes" : "no",
                     state.desktop_size ? "yes" : "no", fine >= 0 ? fine : quality);
            if (!state.tight && !state.h264) {
                ESP_LOGW(TAG, "client offers neither Tight nor H.264; cannot send frames");
                alive = false;
            }
            break;
        }
        case 3: { /* FramebufferUpdateRequest */
            uint8_t request[9];
            if (!recv_exact(&conn, request, sizeof(request), generation)) { alive = false; break; }
            /* Answered from the loop above once there is something to send.
             * A second request before that only matters if it wants all. */
            const bool incremental = request[0] != 0 && (!pending || pending_incremental);
            const int64_t now = esp_timer_get_time();
            pending = true;
            pending_incremental = incremental;
            if (state.h264 && video_frame_payload() == VIDEO_PAYLOAD_H264) {
                pending_waits = h264.started;
                pending_seen = h264.last_seq;
                pending_until = now + (int64_t)(h264.need_key ? VNC_KEYFRAME_WAIT_MS : VNC_READ_TIMEOUT_MS) * 1000;
            } else if (state.tight && !capture_source_is_usb()) {
                pending_waits = incremental && has_frame;
                pending_seen = last_seq;
                pending_until = now + (int64_t)VNC_READ_TIMEOUT_MS * 1000;
            } else {
                pending_waits = true;
                pending_seen = video_frame_seq();
                pending_until = now + 100 * 1000;
            }
            break;
        }
        case 4: { /* KeyEvent */
            uint8_t event[7];
            if (!recv_exact(&conn, event, sizeof(event), generation)) { alive = false; break; }
            client_key(&state, event[0] != 0, be32(event + 3));
            break;
        }
        case 5: { /* PointerEvent */
            uint8_t event[5];
            if (!recv_exact(&conn, event, sizeof(event), generation)) { alive = false; break; }
            client_pointer(&state, event[0], be16(event + 1), be16(event + 3));
            break;
        }
        case 6: { /* ClientCutText: consume but do not put clipboard data on the target. */
            uint8_t header[7];
            if (!recv_exact(&conn, header, sizeof(header), generation)) { alive = false; break; }
            const uint32_t len = be32(header + 3);
            alive = len <= VNC_CLIENT_TEXT_MAX && discard_bytes(&conn, len, generation);
            break;
        }
        default:
            ESP_LOGW(TAG, "unsupported RFB client message type %u", (unsigned)type);
            alive = false;
            break;
        }
    }
    usb_hid_vnc_release();
    if (stream_open) {
        capture_vnc_snapshot_end();
    }
    free(h264.buf);
    video_frame_viewer_leave();
    conn_close(&conn);
    return alive;
}

static int open_listener(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void set_client_timeouts(int fd)
{
    const struct timeval rx = {.tv_sec = VNC_READ_TIMEOUT_MS / 1000,
                               .tv_usec = (VNC_READ_TIMEOUT_MS % 1000) * 1000};
    const struct timeval tx = {.tv_sec = VNC_SEND_TIMEOUT_MS / 1000,
                               .tv_usec = (VNC_SEND_TIMEOUT_MS % 1000) * 1000};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rx, sizeof(rx));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx, sizeof(tx));
    /* A frame's last part-segment would otherwise wait for the ACK of the
     * one before it, and the viewer delays that ACK: tens of ms a frame, so
     * the next frame went by and the H.264 chain broke on every IDR. */
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void vnc_task(void *arg)
{
    (void)arg;
    uint32_t generation = UINT32_MAX;
    int64_t retry_at_us = 0;
    int listener = -1;
    for (;;) {
        if (generation != s_generation) {
            generation = s_generation;
            retry_at_us = 0;
            if (listener >= 0) close(listener);
            listener = -1;
            if (kvm_setting_bool("vnc_enable")) {
                const char *password = kvm_setting_str("vnc_pass");
                const size_t pass_len = password ? strlen(password) : 0;
                bool printable = pass_len > 0 && pass_len <= 8;
                for (size_t i = 0; printable && i < pass_len; i++) {
                    printable = (unsigned char)password[i] >= 0x20 &&
                                (unsigned char)password[i] <= 0x7e;
                }
                if (!printable) {
                    ESP_LOGW(TAG, "disabled: set a 1-8 character printable ASCII password first");
                } else {
                    const int port = (int)kvm_setting_int("vnc_port");
                    listener = open_listener((uint16_t)port);
                    if (listener >= 0) {
                        ESP_LOGI(TAG, "listening on TCP port %d (VNC authentication and VeNCrypt enabled)", port);
                    } else {
                        ESP_LOGE(TAG, "could not listen on TCP port %d", port);
                        retry_at_us = esp_timer_get_time() + 5000000;
                    }
                }
            }
        }
        if (listener < 0) {
            if (generation == s_generation && kvm_setting_bool("vnc_enable") &&
                retry_at_us && esp_timer_get_time() >= retry_at_us) {
                retry_at_us = esp_timer_get_time() + 5000000;
                const char *password = kvm_setting_str("vnc_pass");
                const size_t pass_len = password ? strlen(password) : 0;
                bool printable = pass_len > 0 && pass_len <= 8;
                for (size_t i = 0; printable && i < pass_len; i++) {
                    printable = (unsigned char)password[i] >= 0x20 &&
                                (unsigned char)password[i] <= 0x7e;
                }
                if (printable) {
                    listener = open_listener((uint16_t)kvm_setting_int("vnc_port"));
                    if (listener >= 0) {
                        ESP_LOGI(TAG, "VNC listener is now available on TCP port %d",
                                 (int)kvm_setting_int("vnc_port"));
                        retry_at_us = 0;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listener, &readfds);
        struct timeval wait = {.tv_sec = 1, .tv_usec = 0};
        const int ready = select(listener + 1, &readfds, NULL, NULL, &wait);
        if (ready <= 0 || !FD_ISSET(listener, &readfds)) continue;
        char ip[16];
        const int client = accept_client(listener, ip, sizeof(ip));
        if (client < 0) continue;
        ESP_LOGI(TAG, "client connected from %s", ip);
        s_listener = listener;
        const bool clean = client_loop(client, generation, ip);
        s_listener = -1;
        viewer_clear();
        ESP_LOGI(TAG, "client disconnected%s", clean ? "" : " (protocol or network error)");
    }
}

esp_err_t kvm_vnc_init(void)
{
    if (s_task) return ESP_OK;
    kvm_web_viewers_source(viewers_list);
    if (xTaskCreateWithCaps(vnc_task, "vnc", VNC_TASK_STACK, NULL, VNC_TASK_PRIO,
                            &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void kvm_vnc_apply(void)
{
    kvm_auth_vnc_tickets_clear();
    s_generation++;
}
