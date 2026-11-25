/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_NAME_MAX 256
#define DISPLAY_COUNT_MAX 64
#define INFO_SIZE_MAX (DISPLAY_COUNT_MAX * (DISPLAY_NAME_MAX + 3) + 256)

/* Replace the JSON with little-endian u16 length-prefixed UTF-8 display names.
 * The writable buffer must have one byte of spare capacity for the terminator. */
bool usb_video_parse_displays(uint8_t *payload, size_t payload_size, size_t *packed_size);
