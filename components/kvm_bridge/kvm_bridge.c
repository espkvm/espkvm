/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_bridge.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "bridge";

/*
 * Room for every bridge driver this firmware could hold.
 *
 * Four is generous for something that grows by one chip every year or two, and
 * a fixed array means registration cannot fail for want of memory at a point in
 * start-up where nothing could be done about it. Registration happens in
 * constructors, before any task runs, so no lock is needed here.
 */
#define KVM_BRIDGE_MAX_DRIVERS 4

typedef struct {
    const char *name;
    kvm_bridge_detect_fn detect;
} driver_t;

static driver_t s_drivers[KVM_BRIDGE_MAX_DRIVERS];
static size_t s_count;

esp_err_t kvm_bridge_register(const char *name, kvm_bridge_detect_fn fn)
{
    if (!name || !fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= KVM_BRIDGE_MAX_DRIVERS) {
        /* Cannot log usefully this early, and dropping a driver silently is the
         * worst outcome, so leave the count for the start-up line to report. */
        return ESP_ERR_NO_MEM;
    }
    s_drivers[s_count].name = name;
    s_drivers[s_count].detect = fn;
    s_count++;
    return ESP_OK;
}

size_t kvm_bridge_driver_count(void)
{
    return s_count;
}

esp_err_t kvm_bridge_detect(i2c_master_bus_handle_t bus, kvm_bridge_t *out)
{
    if (!bus || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (s_count == 0) {
        /* Every driver was linked out. That is a build problem - the -u line
         * missing from a driver's CMakeLists - not a cable problem, so say so
         * rather than sending someone to look at their ribbon. */
        ESP_LOGE(TAG, "no bridge drivers registered - none were linked in");
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < s_count; i++) {
        esp_err_t err = s_drivers[i].detect(bus, out);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s found", out->name ? out->name : s_drivers[i].name);
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            /* It answered and then went wrong, which is worth knowing: a chip
             * that is present but unhappy reads as an absent one otherwise. */
            ESP_LOGW(TAG, "%s: %s", s_drivers[i].name, esp_err_to_name(err));
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool kvm_bridge_timings_valid(const kvm_bridge_timings_t *t)
{
    if (!t || !t->tmds || !t->sync) {
        return false;
    }
    /* Guard against half-latched counters while the source retrains: a mode is
     * only believable if the active area fits inside the total. */
    return t->hact >= 320u && t->vact >= 200u && t->htotal > t->hact && t->vtotal > t->vact;
}
