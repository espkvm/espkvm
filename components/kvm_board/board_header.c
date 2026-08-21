/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Where the pins physically are, per board.
 *
 * Transcribed from each vendor's own pinout, and checked against a second
 * source where one existed: the PoE board's diagram and Espressif's J1 table
 * agree pin for pin except at 27 and 28, which Espressif leaves unconnected and
 * Waveshare wires to GPIO24 and GPIO25.
 *
 * Sources:
 *   Waveshare ESP32-P4-ETH   https://www.waveshare.com/wiki/ESP32-P4-ETH
 *   Waveshare PoE            https://www.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH
 *   Waveshare ESP32-P4-NANO  https://www.waveshare.com/esp32-p4-nano.htm
 *   ESP32-P4 Function EV     https://docs.espressif.com/projects/esp-dev-kits/
 *                            en/latest/esp32p4/esp32-p4-function-ev-board/
 *   Guition JC-ESP32P4-M3    the vendor's own pinout photograph of J1
 */
#include "kvm_board_header.h"

#include "sdkconfig.h"

#include <assert.h>

#define PWR(name) {.gpio = -1, .label = (name), .note = NULL}
#define IO(n) {.gpio = (n), .label = NULL, .note = NULL}
#define IO_NOTE(n, why) {.gpio = (n), .label = NULL, .note = (why)}
#define NC() {.gpio = -1, .label = "NC", .note = NULL}

#if CONFIG_KVM_BOARD_WAVESHARE_ETH
/*
 * Two rows of twenty down the long edges, the way a Pico is built. The pads
 * carry their GPIO number on the silkscreen and nothing else, so there are no
 * pin numbers to quote - a pin here is known by what it is.
 */
static const kvm_board_pin_t s_eth_left[] = {
    IO(54), IO(19), PWR("GND"), IO(18),      IO(17),      IO(16),      IO(15),
    PWR("GND"), IO(14), IO(6),  IO(5),       IO(4),       PWR("GND"),  IO(3),
    IO(2),  IO(8),  IO(7),      PWR("GND"),
    IO_NOTE(24, "USB DM"), IO_NOTE(25, "USB DP"),
};
_Static_assert(sizeof(s_eth_left) / sizeof(s_eth_left[0]) == 20,
               "s_eth_left: the column must hold exactly 20 pins");
static const kvm_board_pin_t s_eth_right[] = {
    PWR("VBUS"), PWR("VSYS"), PWR("GND"), PWR("EN"),  PWR("3V3"), IO(20), IO(21),
    PWR("GND"),  IO(22),      IO(23),     PWR("RUN"), IO(26),     PWR("GND"), IO(27),
    IO(32),      IO(33),      IO(46),     PWR("GND"), IO(47),     IO(48),
};
_Static_assert(sizeof(s_eth_right) / sizeof(s_eth_right[0]) == 20,
               "s_eth_right: the column must hold exactly 20 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "", .rows = 20, .numbered = false, .left = s_eth_left, .right = s_eth_right},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Waveshare ESP32-P4-ETH"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_WAVESHARE_POE
/* One 40-pin header laid out the way a Raspberry Pi's is: odd left, even right. */
static const kvm_board_pin_t s_poe_odd[] = {
    PWR("3V3"), IO(7),  IO(8),  IO(23), PWR("GND"), IO(21), IO(20),
    IO(6),      PWR("3V3"), IO(3), IO(2), IO_NOTE(0, "32 kHz crystal"), PWR("GND"), IO(24),
    IO(33),     IO(26), IO(48), IO(53), IO(47),     PWR("GND"),
};
_Static_assert(sizeof(s_poe_odd) / sizeof(s_poe_odd[0]) == 20,
               "s_poe_odd: the column must hold exactly 20 pins");
static const kvm_board_pin_t s_poe_even[] = {
    PWR("5V"), PWR("5V"), PWR("GND"), IO(37), IO(38),     IO(22), PWR("GND"),
    IO(5),     IO(4),     PWR("GND"), IO_NOTE(1, "32 kHz crystal"), IO(36), IO(32), IO(25),
    PWR("GND"), IO(54),   PWR("GND"), IO(46), IO(27),     IO(45),
};
_Static_assert(sizeof(s_poe_even) / sizeof(s_poe_even[0]) == 20,
               "s_poe_even: the column must hold exactly 20 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "GPIO", .rows = 20, .numbered = true, .left = s_poe_odd, .right = s_poe_even},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Waveshare ESP32-P4-WIFI6-POE-ETH"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_WAVESHARE_NANO
/*
 * Two headers of 2x13 rather than one of 2x20, and some of the pins on the
 * second one belong to the ESP32-C6 co-processor, not the P4. Those are not
 * GPIOs this firmware can drive, so they are named and left at that.
 */
static const kvm_board_pin_t s_nano_a_odd[] = {
    PWR("3V3"), IO(7),  IO(8),  IO(23), PWR("GND"), IO(5), IO(20),
    IO(21),     PWR("3V3"), IO(25), IO(26), IO(32), PWR("GND"),
};
_Static_assert(sizeof(s_nano_a_odd) / sizeof(s_nano_a_odd[0]) == 13,
               "s_nano_a_odd: the column must hold exactly 13 pins");
static const kvm_board_pin_t s_nano_a_even[] = {
    PWR("5V"), PWR("5V"), PWR("GND"), IO(37), IO(38), IO(4), PWR("GND"),
    IO(22),    IO(24),    PWR("GND"), IO(27), IO(33), IO(36),
};
_Static_assert(sizeof(s_nano_a_even) / sizeof(s_nano_a_even[0]) == 13,
               "s_nano_a_even: the column must hold exactly 13 pins");
static const kvm_board_pin_t s_nano_b_odd[] = {
    PWR("5V"), PWR("GND"), PWR("3V3"), PWR("GND"), IO(3), IO(2), IO(54),
    IO(47),    IO(46),     IO(45),     PWR("C6 IO12"), PWR("C6 IO13"), PWR("GND"),
};
_Static_assert(sizeof(s_nano_b_odd) / sizeof(s_nano_b_odd[0]) == 13,
               "s_nano_b_odd: the column must hold exactly 13 pins");
static const kvm_board_pin_t s_nano_b_even[] = {
    PWR("ESP_LDO_VO4"), PWR("GND"), IO_NOTE(0, "32 kHz crystal"),
    IO_NOTE(1, "32 kHz crystal"), PWR("GND"), IO(6), IO(53),
    IO(48), PWR("GND"), PWR("C6 U0RXD"), PWR("C6 U0TXD"), PWR("C6 IO9"), PWR("GND"),
};
_Static_assert(sizeof(s_nano_b_even) / sizeof(s_nano_b_even[0]) == 13,
               "s_nano_b_even: the column must hold exactly 13 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "left", .rows = 13, .numbered = true, .left = s_nano_a_odd, .right = s_nano_a_even},
    {.name = "right", .rows = 13, .numbered = true, .left = s_nano_b_odd, .right = s_nano_b_even},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Waveshare ESP32-P4-NANO"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_FUNCEV
/*
 * J1, 40 pins, the Raspberry Pi arrangement again. Pins 27 and 28 are not
 * connected on this board, and three more are only usable once something else
 * is given up - Espressif marks all of those NC in its own table.
 */
static const kvm_board_pin_t s_j1_odd[] = {
    PWR("3V3"), IO(7),  IO(8),  IO(23), PWR("GND"), IO(21), IO(20),
    IO(6),      PWR("3V3"), IO(3), IO(2), IO_NOTE(0, "32 kHz crystal"), PWR("GND"), NC(),
    IO(33),     IO(26), IO(48), IO(53), IO(47),     PWR("GND"),
};
_Static_assert(sizeof(s_j1_odd) / sizeof(s_j1_odd[0]) == 20,
               "s_j1_odd: the column must hold exactly 20 pins");
static const kvm_board_pin_t s_j1_even[] = {
    PWR("5V"), PWR("5V"), PWR("GND"), IO(37), IO(38),     IO(22), PWR("GND"),
    IO(5),     IO(4),     PWR("GND"), IO_NOTE(1, "32 kHz crystal"), IO(36), IO(32), NC(),
    PWR("GND"), IO(54),   PWR("GND"), IO(46), IO(27),
#if CONFIG_KVM_SD_PWR_GPIO == 45
    IO_NOTE(45, "microSD power on this build"),
#else
    IO(45),
#endif
};
_Static_assert(sizeof(s_j1_even) / sizeof(s_j1_even[0]) == 20,
               "s_j1_even: the column must hold exactly 20 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "J1", .rows = 20, .numbered = true, .left = s_j1_odd, .right = s_j1_even},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Espressif ESP32-P4 Function EV"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_GUITION
/*
 * One 2x13 header. Two of its pins are an I2C bus of the board's own and four
 * belong to the ESP32-C6, so eleven GPIOs of the P4 come out here and no more -
 * the narrowest header of any board we build for.
 */
static const kvm_board_pin_t s_guition_left[] = {
    PWR("3V3"), PWR("3V3"), PWR("GND"), IO(1), IO(2), IO(3), IO(4),
    IO(5), IO(20), IO(32), IO(33), PWR("ES I2C_SDA"), PWR("ES I2C_SCL"),
};
_Static_assert(sizeof(s_guition_left) / sizeof(s_guition_left[0]) == 13,
               "s_guition_left: the column must hold exactly 13 pins");
static const kvm_board_pin_t s_guition_right[] = {
    PWR("5V"), PWR("5V"), PWR("GND"), NC(), IO(47), IO(46), IO(45),
    PWR("GND"), PWR("3V3"), PWR("C6 U0RXD"), PWR("C6 U0TXD"), PWR("C6 IO9"),
    PWR("C6 CHIP_PU"),
};
_Static_assert(sizeof(s_guition_right) / sizeof(s_guition_right[0]) == 13,
               "s_guition_right: the column must hold exactly 13 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "J1", .rows = 13, .numbered = false, .left = s_guition_left,
     .right = s_guition_right},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Guition ESP32-P4-M3-Dev"
#define BOARD_VERIFIED false

#else
/* Ported by hand: no published header to draw. */
#define BOARD_NAME "ESP32-P4"
#define BOARD_VERIFIED false
#define BOARD_HAS_HEADERS 0
#endif

const char *kvm_board_name(void)
{
    return BOARD_NAME;
}

bool kvm_board_header_verified(void)
{
    return BOARD_VERIFIED;
}

const kvm_board_header_t *kvm_board_headers(size_t *count)
{
#if BOARD_HAS_HEADERS
    *count = sizeof(s_headers) / sizeof(s_headers[0]);
    return s_headers;
#else
    *count = 0;
    return NULL;
#endif
}
