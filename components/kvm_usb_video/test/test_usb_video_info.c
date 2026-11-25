/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "usb_video_info.h"

static void accept(const char *json, const char *packed, size_t expected_size)
{
    uint8_t payload[INFO_SIZE_MAX + 1];
    const size_t size = strlen(json);
    assert(size < sizeof(payload));
    memcpy(payload, json, size);
    size_t packed_size;
    assert(usb_video_parse_displays(payload, size, &packed_size));
    assert(packed_size == expected_size);
    assert(memcmp(payload, packed, packed_size) == 0);
}

static void reject(const char *json)
{
    uint8_t payload[INFO_SIZE_MAX + 1];
    const size_t size = strlen(json);
    assert(size < sizeof(payload));
    memcpy(payload, json, size);
    size_t packed_size;
    assert(!usb_video_parse_displays(payload, size, &packed_size));
}

int main(void)
{
    accept("{\"displays\":[\"2\",\"1\"],\"version\":\"0.4.5\"}", "\1\0" "2" "\1\0" "1", 6);
    accept("{\"displays\":[\"Desk [A], B\",\"A\\\"B\",\"caf\\u00e9\"]}",
           "\13\0" "Desk [A], B" "\3\0" "A\"B" "\5\0" "caf\xc3\xa9", 25);
    accept("{\"displays\":[]}", "", 0);
    reject("{\"displays\":[\"A\\u0000B\"]}");
    reject("{\"displays\":[[\"A\"]]}");
    reject("{\"displays\":[\"A\",true]}");
    reject("{\"displays\":[]} extra");

    char json[INFO_SIZE_MAX + 1];
    size_t end = (size_t)snprintf(json, sizeof(json), "{\"displays\":[");
    for (size_t i = 0; i < DISPLAY_COUNT_MAX + 1; i++) {
        end += (size_t)snprintf(json + end, sizeof(json) - end, "%s\"A\"", i ? "," : "");
    }
    snprintf(json + end, sizeof(json) - end, "]}");
    reject(json);
    puts("USB video display report tests passed");
}
