/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two things on one pin.
 *
 * Several settings are GPIO numbers the operator picks: the three ATX signals,
 * the round LCD's five. Nothing used to notice when two of them named the same
 * pin, and the result is never an error message - it is a panel that stays
 * dark, or a target that resets when the power LED is read. Worse, the pins the
 * board's own hardware holds were offered too.
 *
 * The rule is a set comparison over a handful of small structs, so it lives
 * here on its own and is tested on a host with no device and no ESP-IDF.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One setting's claim on a pin. */
typedef struct {
    const char *key; /**< the setting's key, for the message */
    int gpio;        /**< the pin it would hold; negative means unassigned */
    /**
     * Whether this request is what sets the pin.
     *
     * Only a claim the operator is making right now can be refused. A device
     * that already has two settings on one pin - set before this check existed,
     * or by an older firmware - must still be able to change anything else,
     * including the setting that would fix it.
     */
    bool changing;
} kvm_pin_claim_t;

/** What was found. Only filled in when there is a conflict. */
typedef struct {
    int gpio;            /**< the pin two things want */
    const char *key;     /**< the claim being made now */
    const char *other;   /**< the setting already on that pin, or NULL */
    const char *held_by; /**< the fixed peripheral holding it, or NULL */
} kvm_pin_conflict_t;

/**
 * Look for a pin claimed twice.
 *
 * @param claims   every pin setting's resulting value, applied request included
 * @param count    how many
 * @param held_by  maps a GPIO to the fixed peripheral holding it, or NULL when
 *                 nothing does; pass NULL to skip that half of the check
 * @param[out] out filled in when this returns true
 * @return true when a claim being made now collides with fixed hardware or with
 *         another setting
 */
bool kvm_pin_conflict_find(const kvm_pin_claim_t *claims, size_t count,
                           const char *(*held_by)(int gpio), kvm_pin_conflict_t *out);

#ifdef __cplusplus
}
#endif
