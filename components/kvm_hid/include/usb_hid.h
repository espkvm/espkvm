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
 * There is a live USB bus on the target's side.
 *
 * True while the device controller sees the target's port, whether or not the
 * target has got as far as enumerating us. False means the port is unpowered or
 * the cable is out - which no amount of re-plugging from this end will fix.
 */
bool usb_hid_bus_alive(void);

/**
 * Absolute pointer position in 0..@ref USB_HID_ABS_MAX, origin top-left.
 * @param wheel vertical scroll clicks, @param pan horizontal scroll clicks.
 */
void usb_hid_mouse_abs(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel, int8_t pan);

/** Relative motion in mickeys, for pointer-lock and captured-cursor software. */
void usb_hid_mouse_rel(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

/**
 * When something was last sent to the target, on the esp_timer clock.
 *
 * The jiggler uses it to keep out of the way: a nudge while the operator is
 * moving the mouse would fight them. Its own nudges land here too, so it
 * remembers the value it left and treats a later one as somebody real.
 */
int64_t usb_hid_last_input_us(void);

/**
 * Start the mouse jiggler, if `jiggle_s` asks for one.
 *
 * A nudge of one pixel and straight back, so the target counts it as activity
 * and nothing on screen moves. Runs on the device rather than in the console
 * because the point of it is a machine left alone - a browser tab that is closed
 * would take a console-side jiggler with it.
 */
void usb_hid_jiggler_start(void);

/** How many nudges it has sent since boot - what makes it testable. */
uint32_t usb_hid_jiggler_nudges(void);

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

/**
 * The current host's USB enumeration trace, as a compact string ("D" device,
 * "Cx" configuration, "Sxx" string index in hex) in request order. Used to
 * fingerprint the target OS. Writes into @p out, returns its length.
 */
size_t usb_hid_probe_trace(char *out, size_t len);

/**
 * The target OS inferred from how it enumerated us: "windows", "macos",
 * "linux", "android", or "unknown" before enough of an enumeration has been
 * seen. A best-effort fingerprint; the console lets the operator override it.
 */
const char *usb_hid_target_os(void);

/**
 * Choose the device type the virtual-media drive presents: a CD-ROM (for .iso
 * images) or a removable disk. If the drive is already enumerated and the type
 * changes, the device re-attaches so the host reads the new type. A no-op when
 * virtual media was not enabled at boot (no drive to re-type).
 */
void usb_hid_msc_set_type(bool cdrom);

/**
 * Come back to the target as a new device: drop off the bus for ~100 ms and
 * attach again, which is what pulling the cable and putting it back does. The
 * host forgets the address and configuration it handed out and enumerates from
 * scratch. Use it when the target has stopped taking input - after a restart or
 * an OTA above all, where this side forgets the bus but the target does not.
 * Returns at once; the device is back a fraction of a second later.
 */
void usb_hid_reattach(void);

#ifdef __cplusplus
}
#endif
