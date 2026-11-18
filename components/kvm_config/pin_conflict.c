/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pin_conflict.h"

#include <string.h>

/* Lines of one I2C bus that two devices share by design: the status OLED and
 * the clock chip on pins of their own sit on the same second bus, each at its
 * own address. */
static bool shared_bus_line(const char *a, const char *b)
{
    static const char *const pairs[][2] = {{"disp_sda", "rtc_sda"}, {"disp_scl", "rtc_scl"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if ((strcmp(a, pairs[i][0]) == 0 && strcmp(b, pairs[i][1]) == 0) ||
            (strcmp(a, pairs[i][1]) == 0 && strcmp(b, pairs[i][0]) == 0)) {
            return true;
        }
    }
    return false;
}

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
            if (claims[i].key && claims[j].key && shared_bus_line(claims[i].key, claims[j].key)) {
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
