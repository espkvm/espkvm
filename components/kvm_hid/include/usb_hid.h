/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB HID device presented to the target machine.
 *
 * Two interfaces:
 *   0  keyboard, boot protocol compatible, no report IDs - firmware setup
 *      screens and legacy BIOSes only understand this shape
 *   1  pointer, report IDs for absolute, relative and consumer control
 *
 * The absolute pointer is what makes a KVM usable: it puts the target's cursor
 * exactly where the operator clicked, regardless of the pointer acceleration
 * and speed settings on the target. Relative motion stays available for
 * software that captures the pointer, such as games and 3D viewers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Full-scale value for absolute coordinates, independent of capture resolution. */
#define USB_HID_ABS_MAX 32767

/** Keyboard LED bits as reported by the target. */
enum {
    USB_HID_LED_NUM_LOCK = 1u << 0,
    USB_HID_LED_CAPS_LOCK = 1u << 1,
    USB_HID_LED_SCROLL_LOCK = 1u << 2,
    USB_HID_LED_COMPOSE = 1u << 3,
    USB_HID_LED_KANA = 1u << 4,
};

/** Start TinyUSB and the background report task. */
esp_err_t usb_hid_init(void);

/** The target has enumerated and configured the device. */
bool usb_hid_ready(void);

/**
 * Absolute pointer position in 0..@ref USB_HID_ABS_MAX, origin top-left.
 * @param wheel vertical scroll clicks, @param pan horizontal scroll clicks.
 */
void usb_hid_mouse_abs(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel, int8_t pan);

/** Relative motion in mickeys, for pointer-lock and captured-cursor software. */
void usb_hid_mouse_rel(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

/** Boot keyboard report: modifier bitmap plus up to six key usages. */
void usb_hid_keyboard(uint8_t modifier, const uint8_t keycode[6]);

/** Consumer control usage (volume, media, power). 0 releases. */
void usb_hid_consumer(uint16_t usage);

/**
 * Release every key and button.
 * Called when the browser tab loses focus or the socket drops, so a held key
 * cannot be left stuck on the target with no way to clear it.
 */
void usb_hid_release_all(void);

/** Last LED state reported by the target; see USB_HID_LED_*. */
uint8_t usb_hid_leds(void);

/** Invoked from the USB task whenever the target updates the keyboard LEDs. */
typedef void (*usb_hid_led_cb_t)(uint8_t leds, void *user);
void usb_hid_set_led_callback(usb_hid_led_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
