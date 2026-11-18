/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 * The cron parser's tests: parsing, ranges, steps, lists, the dom/dow rule.
 */
#include "sched_cron.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);       \
            g_fail++;                                                \
        }                                                            \
    } while (0)

static cron_t g_c;
static bool m(struct tm t) { return cron_match(&g_c, &t); }
static struct tm at(int min, int hour, int mday, int mon1, int wday)
{
    struct tm t = {0};
    t.tm_min = min;
    t.tm_hour = hour;
    t.tm_mday = mday;
    t.tm_mon = mon1 - 1;
    t.tm_wday = wday;
    return t;
}

int main(void)
{
    char err[64];

    /* Every minute. */
    CHECK(cron_parse("* * * * *", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 0, 1, 1, 0)));
    CHECK(m(at(37, 13, 25, 12, 3)));

    /* 07:00 every day: WoL in the morning. */
    CHECK(cron_parse("0 7 * * *", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 7, 10, 6, 2)));
    CHECK(!m(at(1, 7, 10, 6, 2)));
    CHECK(!m(at(0, 8, 10, 6, 2)));

    /* Every 15 minutes. */
    CHECK(cron_parse("*/15 * * * *", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 3, 1, 1, 0)));
    CHECK(m(at(45, 3, 1, 1, 0)));
    CHECK(!m(at(46, 3, 1, 1, 0)));

    /* A range with a step, and a list. */
    CHECK(cron_parse("0 9-17/4 * * *", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 9, 1, 1, 0)) && m(at(0, 13, 1, 1, 0)) &&
          m(at(0, 17, 1, 1, 0)));
    CHECK(!m(at(0, 10, 1, 1, 0)));
    CHECK(cron_parse("30 8 * * 1,3,5", &g_c, err, sizeof(err)));
    CHECK(m(at(30, 8, 1, 1, 1)) && m(at(30, 8, 1, 1, 5)));
    CHECK(!m(at(30, 8, 1, 1, 2)));

    /* Sunday as 0 and as 7 are the same day. */
    CHECK(cron_parse("0 0 * * 7", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 0, 1, 1, 0)));

    /* The dom/dow rule: both restricted -> either fires. 1st OR a Monday. */
    CHECK(cron_parse("0 0 1 * 1", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 0, 1, 6, 3)));  /* the 1st, a Wednesday */
    CHECK(m(at(0, 0, 15, 6, 1))); /* the 15th, a Monday */
    CHECK(!m(at(0, 0, 15, 6, 3)));
    /* Only dom restricted -> just the day of month. */
    CHECK(cron_parse("0 0 15 * *", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 0, 15, 6, 3)));
    CHECK(!m(at(0, 0, 16, 6, 3)));

    /* A month. */
    CHECK(cron_parse("0 0 1 1 *", &g_c, err, sizeof(err)));
    CHECK(m(at(0, 0, 1, 1, 4)));
    CHECK(!m(at(0, 0, 1, 2, 0)));

    /* Refusals, each with a word in the message. */
    CHECK(!cron_parse("* * * *", &g_c, err, sizeof(err)) && strstr(err, "five fields"));
    CHECK(!cron_parse("* * * * * *", &g_c, err, sizeof(err)) && strstr(err, "five fields"));
    CHECK(!cron_parse("60 * * * *", &g_c, err, sizeof(err)) && strstr(err, "minute"));
    CHECK(!cron_parse("0 24 * * *", &g_c, err, sizeof(err)) && strstr(err, "hour"));
    CHECK(!cron_parse("0 0 0 * *", &g_c, err, sizeof(err)) && strstr(err, "day-of-month"));
    CHECK(!cron_parse("0 0 * 13 *", &g_c, err, sizeof(err)) && strstr(err, "month"));
    CHECK(!cron_parse("0 0 * * 8", &g_c, err, sizeof(err)) && strstr(err, "weekday"));
    CHECK(!cron_parse("0 0 * * mon", &g_c, err, sizeof(err)));
    CHECK(!cron_parse("*/0 * * * *", &g_c, err, sizeof(err)));
    CHECK(!cron_parse("5- * * * *", &g_c, err, sizeof(err)));
    CHECK(!cron_parse("1,,2 * * * *", &g_c, err, sizeof(err)));

    printf(g_fail ? "%d FAILED\n" : "sched cron: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
