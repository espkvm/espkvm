/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_bridge.h"
#include "kvm_bridge_edid_data.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/*
 * Who else is on the bus, when no driver recognised anything.
 *
 * "Nothing answered" and "something answered, and this firmware does not know
 * it" look the same from outside, and they send you to different places: a
 * ribbon in the first case, a missing driver in the second.
 */
int kvm_bridge_scan(i2c_master_bus_handle_t bus, char *out, size_t out_len)
{
    int count = 0;
    size_t len = 0;
    if (out && out_len) {
        out[0] = '\0';
    }
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            continue;
        }
        count++;
        if (out && len + 6 < out_len) {
            len += (size_t)snprintf(out + len, out_len - len, " 0x%02x", addr);
        }
    }
    return count;
}

/*
 * Chips that sit on a board's own I2C bus and are never a capture bridge. The
 * Guition M3-Dev's ES8311 at 0x18 answered on the capture bus with the C790
 * unplugged, and the message said "no driver here knows it" - which sent
 * someone looking for a firmware problem instead of at the ribbon (#61).
 */
static const struct {
    uint8_t addr;
    const char *what;
} k_board_chips[] = {
    {0x18, "ES8311 audio codec"},
    {0x40, "ES7210 microphone ADC"},
    {0x14, "GT911 touch controller"},
    {0x5D, "GT911 touch controller"},
    {0x38, "FT5x06 touch controller"},
    {0x3C, "OLED"},
    {0x3D, "OLED"},
    {0x34, "AXP power chip"},
    {0x51, "PCF8563 clock"},
};

static const char *board_chip(uint8_t addr)
{
    for (size_t i = 0; i < sizeof(k_board_chips) / sizeof(k_board_chips[0]); i++) {
        if (k_board_chips[i].addr == addr) {
            return k_board_chips[i].what;
        }
    }
    return NULL;
}

int kvm_bridge_scan_split(i2c_master_bus_handle_t bus, char *unknown, size_t unknown_len,
                          char *known, size_t known_len)
{
    int count = 0;
    size_t ul = 0;
    size_t kl = 0;
    if (unknown && unknown_len) {
        unknown[0] = '\0';
    }
    if (known && known_len) {
        known[0] = '\0';
    }
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            continue;
        }
        const char *what = board_chip(addr);
        if (what) {
            if (known && kl + 40 < known_len) {
                kl += (size_t)snprintf(known + kl, known_len - kl, "%s0x%02x %s", kl ? ", " : "",
                                       addr, what);
            }
            continue;
        }
        count++;
        if (unknown && ul + 6 < unknown_len) {
            ul += (size_t)snprintf(unknown + ul, unknown_len - ul, " 0x%02x", addr);
        }
    }
    return count;
}

static void log_bus_scan(i2c_master_bus_handle_t bus, const char *when)
{
    char found[96];
    if (kvm_bridge_scan(bus, found, sizeof(found)) == 0) {
        ESP_LOGE(TAG, "%s: nothing at all answers on the capture I2C bus - is the ribbon to "
                      "the capture board seated?",
                 when);
    } else {
        ESP_LOGE(TAG, "%s: on the bus, unrecognised:%s", when, found);
    }
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

    /*
     * Ask more than once. A bridge that runs its own firmware can be seconds
     * behind its reset line before it answers I2C at all, and a single probe
     * taken too early reads exactly like an empty bus.
     */
    for (int waited_s = 0; waited_s <= 2; waited_s++) {
        if (waited_s > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        for (size_t i = 0; i < s_count; i++) {
            esp_err_t err = s_drivers[i].detect(bus, out);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "%s found", out->name ? out->name : s_drivers[i].name);
                return ESP_OK;
            }
            if (err != ESP_ERR_NOT_FOUND) {
                /* It answered and then went wrong, which is worth knowing: a
                 * chip that is present but unhappy reads as an absent one
                 * otherwise. */
                ESP_LOGW(TAG, "%s: %s", s_drivers[i].name, esp_err_to_name(err));
            }
        }
        char when[32];
        snprintf(when, sizeof(when), "%d s after reset", waited_s);
        log_bus_scan(bus, when);
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

/* ---- EDID ------------------------------------------------------------------ */

_Static_assert(KVM_EDID_TOTAL_LEN == KVM_BRIDGE_EDID_LEN, "regenerate kvm_bridge_edid_data.h");

/* The operator's monitor name and IDs over the built-in ones, in block 0,
 * then its checksum again. The layout is EDID 1.3/1.4: maker at 8-9,
 * product 10-11 and serial 12-15 little-endian, the name in the 0xFC
 * descriptor (gen_edid.py puts one in every profile). */
static void edid_apply_id(uint8_t *e, const kvm_bridge_edid_id_t *id)
{
    if (id->mfr[0]) {
        const uint16_t m = (uint16_t)(((id->mfr[0] - '@') & 31) << 10 |
                                      ((id->mfr[1] - '@') & 31) << 5 | ((id->mfr[2] - '@') & 31));
        e[8] = (uint8_t)(m >> 8);
        e[9] = (uint8_t)m;
    }
    if (id->has_product) {
        e[10] = (uint8_t)id->product;
        e[11] = (uint8_t)(id->product >> 8);
    }
    if (id->has_serial) {
        for (int i = 0; i < 4; i++) {
            e[12 + i] = (uint8_t)(id->serial >> (8 * i));
        }
    }
    if (id->name[0]) {
        for (int off = 54; off <= 108; off += 18) {
            if (e[off] == 0 && e[off + 1] == 0 && e[off + 2] == 0 && e[off + 3] == 0xFC) {
                /* 13 bytes: the name, 0x0A after a short one, spaces after that. */
                const size_t n = strnlen(id->name, 13);
                memset(e + off + 5, ' ', 13);
                memcpy(e + off + 5, id->name, n);
                if (n < 13) {
                    e[off + 5 + n] = 0x0A;
                }
                break;
            }
        }
    }
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) {
        sum += e[i];
    }
    e[127] = (uint8_t)(0x100 - sum);
}

void kvm_bridge_edid_build(kvm_bridge_edid_profile_t profile, const kvm_bridge_edid_id_t *id,
                           uint8_t out[KVM_BRIDGE_EDID_LEN], const char **what)
{
    const uint8_t *bin = kvm_edid_full;
    const char *w = "full mode list";
    switch (profile) {
    case KVM_BRIDGE_EDID_1080P30:
        bin = kvm_edid_1080p30;
        w = "1080p30 only";
        break;
    case KVM_BRIDGE_EDID_720P:
        bin = kvm_edid_720p;
        w = "up to 720p";
        break;
    case KVM_BRIDGE_EDID_1024X768:
        bin = kvm_edid_1024x768;
        w = "up to 1024x768";
        break;
    case KVM_BRIDGE_EDID_480P:
        bin = kvm_edid_480p;
        w = "up to 720x480";
        break;
    case KVM_BRIDGE_EDID_FULL_16BPP:
        bin = kvm_edid_full16;
        w = "full mode list, 1080p50 preferred";
        break;
    default:
        break;
    }
    memcpy(out, bin, KVM_BRIDGE_EDID_LEN);
    if (id) {
        edid_apply_id(out, id);
    }
    if (what) {
        *what = w;
    }
}
