/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A small cron parser and matcher. Plain C over a string and a struct tm, so
 * it runs on a host too. Five fields, the usual meanings:
 *
 *   minute   0-59
 *   hour     0-23
 *   dom      1-31   (day of month)
 *   month    1-12
 *   dow      0-7    (day of week, Sunday is 0 or 7)
 *
 * Each field is `*`, a number, a range `a-b`, a step `*'/'n` or `a-b/n`, or a
 * comma list of those. When BOTH day-of-month and day-of-week are restricted a
 * match on either fires, which is how cron has always behaved.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t minute; /* bits 0..59 */
    uint32_t hour;   /* bits 0..23 */
    uint32_t dom;    /* bits 1..31 */
    uint16_t month;  /* bits 1..12 */
    uint8_t dow;     /* bits 0..6, Sunday 0 */
    bool dom_any;    /* the day-of-month field was `*` */
    bool dow_any;    /* the day-of-week field was `*` */
} cron_t;

/**
 * Parse a five-field cron spec into @p out. False on the first bad field, with
 * @p err holding "what", so an editor can say why before it is saved.
 */
bool cron_parse(const char *spec, cron_t *out, char *err, size_t err_cap);

/** Whether @p t (local time) satisfies @p c. Seconds are ignored. */
bool cron_match(const cron_t *c, const struct tm *t);

#ifdef __cplusplus
}
#endif
