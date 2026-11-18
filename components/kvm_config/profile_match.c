/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "profile_match.h"

#include <ctype.h>
#include <stdbool.h>

static bool empty(const char *s)
{
    return !s || !s[0];
}

static bool same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return !*a && !*b;
}

static bool starts_with(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return false;
        }
    }
    return true;
}

int kvm_prof_rule_score(const kvm_prof_rule_t *r, const kvm_prof_seen_t *s)
{
    const bool want_name = !empty(r->cec_name);
    const bool want_vendor = !empty(r->cec_vendor);
    const bool want_os = !empty(r->usb_os);
    int score = 0;

    if (want_name || want_vendor) {
        const bool name_ok = want_name && !empty(s->cec_name) && starts_with(s->cec_name, r->cec_name);
        const bool vendor_ok = want_vendor && !empty(s->cec_vendor) && same(s->cec_vendor, r->cec_vendor);
        if (!name_ok && !vendor_ok) {
            return -1;
        }
        score += 2;
    }
    if (want_os) {
        if (empty(s->usb_os) || !same(s->usb_os, r->usb_os)) {
            return -1;
        }
        score += 1;
    }
    return score > 0 ? score : -1;
}

int kvm_prof_pick(const kvm_prof_rule_t *rules, size_t n, const kvm_prof_seen_t *s)
{
    int best = -1, best_score = -1;
    for (size_t i = 0; i < n; i++) {
        int sc = kvm_prof_rule_score(&rules[i], s);
        if (sc > best_score) {
            best = (int)i;
            best_score = sc;
        }
    }
    return best;
}
