/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pin_conflict.h"

bool kvm_pin_conflict_find(const kvm_pin_claim_t *claims, size_t count,
                           const char *(*held_by)(int gpio), kvm_pin_conflict_t *out)
{
    if (!claims || !out) {
        return false;
    }

    /*
     * Fixed hardware first. It is the more useful answer of the two - "that pin
     * is the Ethernet PHY's" tells an operator something they cannot work out
     * from the settings page - and it is the one that breaks the device rather
     * than a feature.
     */
    if (held_by) {
        for (size_t i = 0; i < count; i++) {
            if (!claims[i].changing || claims[i].gpio < 0) {
                continue;
            }
            const char *use = held_by(claims[i].gpio);
            if (use) {
                out->gpio = claims[i].gpio;
                out->key = claims[i].key;
                out->other = NULL;
                out->held_by = use;
                return true;
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (claims[i].gpio < 0) {
            continue;
        }
        for (size_t j = i + 1; j < count; j++) {
            if (claims[j].gpio != claims[i].gpio) {
                continue;
            }
            /* One of the two has to be this request's doing; a pair that was
             * already stored that way is somebody else's problem to fix. */
            if (!claims[i].changing && !claims[j].changing) {
                continue;
            }
            /* Name the one being set now first - it is the field the operator
             * just typed into, and the one they will go back to. */
            const size_t mine = claims[j].changing ? j : i;
            const size_t theirs = (mine == j) ? i : j;
            out->gpio = claims[mine].gpio;
            out->key = claims[mine].key;
            out->other = claims[theirs].key;
            out->held_by = NULL;
            return true;
        }
    }
    return false;
}
