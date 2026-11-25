/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usb_video_info.h"

#include <string.h>

#include "cJSON.h"

/* Bound cJSON's stack depth and node count before passing it host input. */
static bool bounded_info_json(const uint8_t *bytes, size_t size)
{
    if (size > INFO_SIZE_MAX || memchr(bytes, 0, size)) {
        return false;
    }
    unsigned depth = 0, members = 0;
    bool quoted = false;
    for (size_t i = 0; i < size; i++) {
        const uint8_t c = bytes[i];
        if (quoted) {
            if (c == '\\') {
                if (size - i >= 6 && memcmp(bytes + i + 1, "u0000", 5) == 0) {
                    return false;
                }
                i++;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            if (++depth > 2) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            if (!depth) {
                return false;
            }
            depth--;
        } else if (c == ',' || c == ':') {
            if (++members > DISPLAY_COUNT_MAX + 8) {
                return false;
            }
        }
    }
    return !quoted && depth == 0;
}

bool usb_video_parse_displays(uint8_t *payload, size_t payload_size, size_t *packed_size)
{
    if (!bounded_info_json(payload, payload_size)) {
        return false;
    }
    payload[payload_size] = 0;
    cJSON *info = cJSON_ParseWithLengthOpts((const char *)payload, payload_size + 1, NULL, true);
    const cJSON *displays = cJSON_GetObjectItemCaseSensitive(info, "displays");
    if (!cJSON_IsArray(displays) || cJSON_GetArraySize(displays) > DISPLAY_COUNT_MAX) {
        cJSON_Delete(info);
        return false;
    }
    const cJSON *display;
    size_t size = 0;
    cJSON_ArrayForEach(display, displays) {
        const char *name = cJSON_GetStringValue(display);
        const size_t len = name ? strlen(name) : 0;
        if (!len || len > DISPLAY_NAME_MAX) {
            cJSON_Delete(info);
            return false;
        }
        payload[size] = len;
        payload[size + 1] = len >> 8;
        memcpy(payload + size + 2, name, len);
        size += len + 2;
    }
    cJSON_Delete(info);
    *packed_size = size;
    return true;
}
