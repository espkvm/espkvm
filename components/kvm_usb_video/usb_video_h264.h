/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Validate the framing and every SPS in an Annex B access unit before publish. */
bool usb_video_h264_access_unit(const uint8_t *data, size_t len, uint32_t flags,
                                uint32_t max_width, uint32_t max_height);
