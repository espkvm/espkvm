/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The cron parser: each field becomes a bitmask, and matching is one bit test
 * per field. No allocation, no strtok, so it host-tests cleanly.
 */
#include "sched_cron.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse one item of a field ("*", "5", "1-4", "*'/'2", "0-30/10") into @p mask,
   the bits for values lo..hi. Returns false on anything malformed or out of
   range. */
static bool parse_item(char *item, int lo, int hi, uint64_t *mask)
{
    int step = 1;
    char *slash = strchr(item, '/');
    if (slash) {
        *slash = '\0';
        char *end;
        const long s = strtol(slash + 1, &end, 10);
        if (*end != '\0' || slash[1] == '\0' || s < 1 || s > (hi - lo + 1)) {
            return false;
        }
        step = (int)s;
    }

    int from, to;
    if (strcmp(item, "*") == 0) {
        from = lo;
        to = hi;
    } else {
        char *dash = strchr(item, '-');
        if (dash) {
            *dash = '\0';
            char *e1;
            char *e2;
            const long a = strtol(item, &e1, 10);
            const long b = strtol(dash + 1, &e2, 10);
            if (*e1 || *e2 || item[0] == '\0' || dash[1] == '\0') {
                return false;
            }
            from = (int)a;
            to = (int)b;
        } else {
            char *e;
            const long v = strtol(item, &e, 10);
            if (*e != '\0' || item[0] == '\0') {
                return false;
            }
            from = to = (int)v;
        }
    }
    if (from < lo || to > hi || from > to) {
        return false;
    }
    for (int v = from; v <= to; v += step) {
        *mask |= (uint64_t)1 << v;
    }
    return true;
}

/* A whole field: comma-separated items. */
static bool parse_field(const char *field, int lo, int hi, uint64_t *mask)
{
    *mask = 0;
    char buf[80];
    if (field[0] == '\0' || strlen(field) >= sizeof(buf)) {
        return false;
    }
    strcpy(buf, field);
    char *cur = buf;
    while (cur) {
        char *comma = strchr(cur, ',');
        if (comma) {
            *comma = '\0';
        }
        char *next = comma ? comma + 1 : NULL;
        if (cur[0] == '\0' || !parse_item(cur, lo, hi, mask)) {
            return false;
        }
        cur = next;
    }
    return *mask != 0;
}

bool cron_parse(const char *spec, cron_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));

    /* Split into exactly five whitespace-separated fields. */
    char buf[128];
    if (strlen(spec) >= sizeof(buf)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "the schedule is too long");
        }
        return false;
    }
    strcpy(buf, spec);
    char *f[5];
    int n = 0;
    char *p = buf;
    while (*p && n < 5) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (n != 5 || *p != '\0') {
        if (err && err_cap) {
            snprintf(err, err_cap, "a schedule is five fields: minute hour day month weekday");
        }
        return false;
    }

    static const char *const names[5] = {"minute", "hour", "day-of-month", "month", "weekday"};
    static const int lo[5] = {0, 0, 1, 1, 0};
    static const int hi[5] = {59, 23, 31, 12, 7};
    uint64_t m[5];
    for (int i = 0; i < 5; i++) {
        if (!parse_field(f[i], lo[i], hi[i], &m[i])) {
            if (err && err_cap) {
                snprintf(err, err_cap, "the %s field \"%.20s\" is not valid", names[i], f[i]);
            }
            return false;
        }
    }
    out->minute = m[0];
    out->hour = (uint32_t)m[1];
    out->dom = (uint32_t)m[2];
    out->month = (uint16_t)m[3];
    /* Fold Sunday-as-7 onto Sunday-as-0 so the matcher only checks bit 0. */
    if (m[4] & ((uint64_t)1 << 7)) {
        m[4] |= 1;
    }
    out->dow = (uint8_t)(m[4] & 0x7f);
    out->dom_any = strcmp(f[2], "*") == 0;
    out->dow_any = strcmp(f[4], "*") == 0;
    return true;
}

bool cron_match(const cron_t *c, const struct tm *t)
{
    if (!((c->minute >> t->tm_min) & 1)) {
        return false;
    }
    if (!((c->hour >> t->tm_hour) & 1)) {
        return false;
    }
    if (!((c->month >> (t->tm_mon + 1)) & 1)) {
        return false;
    }
    const bool dom_hit = (c->dom >> t->tm_mday) & 1;
    const bool dow_hit = (c->dow >> (t->tm_wday % 7)) & 1;
    /* The classic rule: if both day fields are restricted, either one firing is
       enough; otherwise the restricted one (or both `*`) must hold. */
    if (!c->dom_any && !c->dow_any) {
        return dom_hit || dow_hit;
    }
    return dom_hit && dow_hit;
}
