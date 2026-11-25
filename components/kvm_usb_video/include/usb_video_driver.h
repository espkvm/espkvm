/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "tusb.h"
#include "device/usbd_pvt.h"

#define USB_VIDEO_SUBCLASS 0x4a
#define USB_VIDEO_PROTOCOL 1
#define USB_VIDEO_INTERFACE_LEN (TUD_VENDOR_DESC_LEN)

extern const usbd_class_driver_t usb_video_driver;
