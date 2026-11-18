/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which device profile fits the attached device. Plain C over strings, so it
 * runs in the host tests.
 */
#pragma once

#include <stddef.h>

/* What a profile's rule asks for. "" means "any". */
typedef struct {
    const char *cec_name;   /**< start of the CEC name, any case ("Steam Deck") */
    const char *cec_vendor; /**< CEC vendor id, six hex digits ("0009bf") */
    const char *usb_os;     /**< the OS guessed from USB ("linux") */
} kvm_prof_rule_t;

/* What is attached now. "" when not known. */
typedef struct {
    const char *cec_name;
    const char *cec_vendor;
    const char *usb_os;
} kvm_prof_seen_t;

/**
 * How well @p r fits @p s: -1 when it does not, else 2 for the CEC part plus 1
 * for the USB part. The CEC part fits when the name or the vendor does. An
 * empty rule fits nothing.
 */
int kvm_prof_rule_score(const kvm_prof_rule_t *r, const kvm_prof_seen_t *s);

/** The index of the best rule, the first of equals; -1 when none fits. */
int kvm_prof_pick(const kvm_prof_rule_t *rules, size_t n, const kvm_prof_seen_t *s);
