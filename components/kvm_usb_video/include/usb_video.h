/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_VIDEO_FRAME_CAP (512u * 1024u)

typedef struct {
    uint32_t width, height, fps, frames, sequence;
    int64_t last_frame_us;
    bool playing;
} usb_video_status_t;

bool usb_video_init(uint8_t interface_number, void (*set_visible)(bool));
bool usb_video_status(usb_video_status_t *out);
bool usb_video_take(uint32_t sequence, uint8_t *out, size_t capacity,
                    size_t *length, bool *keyframe);
void usb_video_wait(void);
void usb_video_request_keyframe(void);
void usb_video_console_set(bool enabled, uint8_t display, uint32_t revision);
uint8_t *usb_video_console_state(uint8_t opcode, uint32_t known_revision, size_t *length);
