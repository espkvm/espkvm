/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The XInput (wired Xbox 360) pad, or the original Xbox pad (XID). Private to kvm_hid; usb_hid.c owns when it
 * is part of the device and what it sends.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

#define XINPUT_IFACE_DESC_LEN (9 + 17 + 7 + 7)
#define XID_IFACE_DESC_LEN (9 + 7 + 7)
#define XINPUT_REPORT_LEN 20

extern const tusb_desc_device_t xinput_device_desc;
extern const tusb_desc_device_t xid_device_desc;

/** The original Xbox pad's interface block; it is always alone, interface 0. */
size_t xid_iface_desc(uint8_t *out, uint8_t ep_in, uint8_t ep_out);

/** Write the interface block (interface, class descriptor, two endpoints). */
size_t xinput_iface_desc(uint8_t *out, uint8_t itf, uint8_t ep_in, uint8_t ep_out);

/** Register the class driver with TinyUSB, as XInput or as XID. Call before the stack starts. */
void xinput_enable(bool on, bool xid);

/** Called from the USB task when an input report has left. */
void xinput_set_done_cb(void (*cb)(void));

bool xinput_ready(void);

/** Queue one 20-byte input report; false when the endpoint is busy or closed. */
bool xinput_send(const uint8_t report[XINPUT_REPORT_LEN]);
