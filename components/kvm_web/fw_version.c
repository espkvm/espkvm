/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fw_version.h"

#include <ctype.h>
#include <stdlib.h>

/* "v1.2.3", "v.1.2.3" or "v1.2"; nothing before or after. */
static bool parse(const char *v, long n[3])
{
    if (!v || *v != 'v') {
        return false;
    }
    v++;
    if (*v == '.') {
        v++;
    }
    n[2] = 0;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*v)) {
            return false;
        }
        char *end;
        n[i] = strtol(v, &end, 10);
        v = end;
        if (*v == '\0') {
            return i >= 1;
        }
        if (*v != '.' || i == 2) {
            return false;
        }
        v++;
    }
    return false;
}

bool fw_version_cmp(const char *a, const char *b, int *out)
{
    long x[3], y[3];
    if (!parse(a, x) || !parse(b, y)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) {
            *out = x[i] < y[i] ? -1 : 1;
            return true;
        }
    }
    *out = 0;
    return true;
}
