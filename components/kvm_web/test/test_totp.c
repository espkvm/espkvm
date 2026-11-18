/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "totp.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(c)                                                   \
    do {                                                           \
        if (!(c)) {                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            fails++;                                               \
        }                                                          \
    } while (0)

int main(void)
{
    /* RFC 2202, HMAC-SHA1 test case 2. */
    uint8_t mac[20];
    totp_hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, mac);
    static const uint8_t want_mac[20] = {0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
                                         0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79};
    CHECK(memcmp(mac, want_mac, 20) == 0);

    /* RFC 6238 appendix B, SHA1, eight digits. */
    const uint8_t *key = (const uint8_t *)"12345678901234567890";
    static const struct {
        uint64_t t;
        uint32_t code;
    } v[] = {{59, 94287082},          {1111111109, 7081804},  {1111111111, 14050471},
             {1234567890, 89005924},  {2000000000, 69279037}, {20000000000ULL, 65353130}};
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        CHECK(totp_code(key, 20, v[i].t / 30, 8) == v[i].code);
        CHECK(totp_code(key, 20, v[i].t / 30, 6) == v[i].code % 1000000);
    }

    /* A code is good for the step either side, once. */
    uint64_t used = 0;
    const uint32_t c = totp_code(key, 20, 1234567890 / 30, 6);
    CHECK(totp_check(key, 20, 1234567890, c, 0, &used) && used == 1234567890 / 30);
    CHECK(totp_check(key, 20, 1234567890 + 30, c, 0, NULL));
    CHECK(!totp_check(key, 20, 1234567890 + 90, c, 0, NULL));
    CHECK(!totp_check(key, 20, 1234567890, c, used, NULL));
    CHECK(!totp_check(key, 20, 1234567890, (c + 1) % 1000000, 0, NULL));

    /* RFC 4648 base32. */
    char b[64];
    totp_base32((const uint8_t *)"foobar", 6, b, sizeof(b));
    CHECK(strcmp(b, "MZXW6YTBOI") == 0);
    totp_base32(key, 20, b, sizeof(b));
    CHECK(strcmp(b, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") == 0);

    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("totp: all checks passed\n");
    return 0;
}
