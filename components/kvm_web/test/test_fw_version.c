/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fw_version.h"

#include <stdio.h>

static int fails;

#define CHECK(c)                                                   \
    do {                                                           \
        if (!(c)) {                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            fails++;                                               \
        }                                                          \
    } while (0)

static int cmp(const char *a, const char *b)
{
    int r = 99;
    return fw_version_cmp(a, b, &r) ? r : 99;
}

int main(void)
{
    CHECK(cmp("v.0.82.0", "v.0.81.0") > 0);
    CHECK(cmp("v.0.81.0", "v.0.82.0") < 0);
    CHECK(cmp("v.0.81.0", "v.0.81.0") == 0);
    CHECK(cmp("v.0.100.0", "v.0.99.9") > 0); /* numbers, not text */
    CHECK(cmp("v1.2", "v1.2.0") == 0);
    CHECK(cmp("v.0.81.1", "v0.81.0") > 0);
    /* Not releases: no answer. */
    CHECK(cmp("v.0.81.0-3-gabc1234-dirty", "v.0.81.0") == 99);
    CHECK(cmp("v.0.81.0", "abc1234") == 99);
    CHECK(cmp("", "v.0.81.0") == 99);
    CHECK(cmp("v.1", "v.0.81.0") == 99);
    CHECK(cmp("v.0.81.0.1", "v.0.81.0") == 99);
    if (fails) {
        printf("versions: %d failed\n", fails);
        return 1;
    }
    printf("versions: all passed\n");
    return 0;
}
