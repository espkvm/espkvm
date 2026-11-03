/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * VeNCrypt wire negotiation, including short reads/writes and retryable socket
 * results. The queued ClientHello bytes must remain untouched for the TLS layer.
 */
#include "vencrypt_protocol.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
            failures++;                                                      \
        }                                                                     \
    } while (0)

typedef struct {
    uint8_t input[64];
    size_t input_len;
    size_t input_pos;
    uint8_t output[64];
    size_t output_len;
    unsigned reads, writes, waits;
    bool stop_wait;
    bool fail_status;
} fake_io_t;

static int fake_send(void *ctx, const uint8_t *data, size_t len)
{
    fake_io_t *f = ctx;
    f->writes++;
    if (f->writes == 1) return VENC_IO_INTERRUPTED;
    if (f->writes == 2) return VENC_IO_WANT_WRITE;
    if (f->fail_status && len == 1 && data[0] == 0) return VENC_IO_ERROR;
    const size_t n = len > 2 ? 2 : len;
    if (f->output_len + n > sizeof(f->output)) return VENC_IO_ERROR;
    memcpy(f->output + f->output_len, data, n);
    f->output_len += n;
    return (int)n;
}

static int fake_recv(void *ctx, uint8_t *data, size_t len)
{
    fake_io_t *f = ctx;
    f->reads++;
    if (f->reads == 1) return VENC_IO_INTERRUPTED;
    if (f->reads == 2) return VENC_IO_WANT_READ;
    if (f->input_pos == f->input_len) return VENC_IO_CLOSED;
    size_t n = len > 1 ? 1 : len;
    if (n > f->input_len - f->input_pos) n = f->input_len - f->input_pos;
    memcpy(data, f->input + f->input_pos, n);
    f->input_pos += n;
    return (int)n;
}

static bool fake_wait(void *ctx, bool writing)
{
    fake_io_t *f = ctx;
    (void)writing;
    f->waits++;
    return !f->stop_wait;
}

static void init(fake_io_t *f, uint8_t major, uint8_t minor, uint32_t subtype,
                 const uint8_t *tail, size_t tail_len)
{
    memset(f, 0, sizeof(*f));
    f->input[f->input_len++] = major;
    f->input[f->input_len++] = minor;
    f->input[f->input_len++] = (uint8_t)(subtype >> 24);
    f->input[f->input_len++] = (uint8_t)(subtype >> 16);
    f->input[f->input_len++] = (uint8_t)(subtype >> 8);
    f->input[f->input_len++] = (uint8_t)subtype;
    if (tail_len) memcpy(f->input + f->input_len, tail, tail_len);
    f->input_len += tail_len;
}

static vencrypt_result_t negotiate(fake_io_t *f, uint32_t *subtype)
{
    const vencrypt_io_t io = {
        .ctx = f,
        .send = fake_send,
        .recv = fake_recv,
        .wait = fake_wait,
    };
    return vencrypt_server_negotiate(&io, subtype);
}

static void test_tlsvnc_fragmented_and_clienthello_boundary(void)
{
    static const uint8_t hello[] = {0x16, 0x03, 0x01, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05};
    fake_io_t f;
    init(&f, 0, 2, VENC_SUBTYPE_TLS_VNC, hello, sizeof(hello));
    uint32_t subtype = 0;
    CHECK(negotiate(&f, &subtype) == VENC_RESULT_OK);
    CHECK(subtype == VENC_SUBTYPE_TLS_VNC);
    CHECK(f.waits >= 2);
    CHECK(f.input_len - f.input_pos == sizeof(hello));
    CHECK(memcmp(f.input + f.input_pos, hello, sizeof(hello)) == 0);

    static const uint8_t offer[] = {0, 2, 0, 2, 0, 0, 1, 5, 0, 0, 1, 2, 0};
    CHECK(f.output_len == sizeof(offer));
    CHECK(memcmp(f.output, offer, sizeof(offer)) == 0);
}

static void test_x509vnc(void)
{
    fake_io_t f;
    init(&f, 0, 2, VENC_SUBTYPE_X509_VNC, NULL, 0);
    uint32_t subtype = 0;
    CHECK(negotiate(&f, &subtype) == VENC_RESULT_OK);
    CHECK(subtype == VENC_SUBTYPE_X509_VNC);
}

static void test_invalid_version_and_subtype(void)
{
    fake_io_t f;
    uint32_t subtype = 0;
    init(&f, 0, 1, VENC_SUBTYPE_TLS_VNC, NULL, 0);
    CHECK(negotiate(&f, &subtype) == VENC_RESULT_BAD_VERSION);
    CHECK(f.output_len == 3 && memcmp(f.output, (uint8_t[]){0, 2, 1}, 3) == 0);

    init(&f, 0, 2, 3, NULL, 0);
    CHECK(negotiate(&f, &subtype) == VENC_RESULT_BAD_SUBTYPE);
}

static void test_wait_failure(void)
{
    fake_io_t f;
    uint32_t subtype = 0;
    init(&f, 0, 2, VENC_SUBTYPE_TLS_VNC, NULL, 0);
    f.stop_wait = true;
    CHECK(negotiate(&f, &subtype) == VENC_RESULT_IO_ERROR);
}

static void test_status_send_failure(void)
{
    fake_io_t f;
    uint32_t subtype = 0;
    init(&f, 0, 2, VENC_SUBTYPE_TLS_VNC, NULL, 0);
    f.fail_status = true;
    CHECK(negotiate(&f, &subtype) == VENC_RESULT_IO_ERROR);
    CHECK(f.output_len == 12);
}

int main(void)
{
    test_tlsvnc_fragmented_and_clienthello_boundary();
    test_x509vnc();
    test_invalid_version_and_subtype();
    test_wait_failure();
    test_status_send_failure();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("vencrypt: all checks passed");
    return 0;
}
