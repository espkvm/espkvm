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
 *   Waveshare ESP32-P4-WIFI6 https://docs.waveshare.com/ESP32-P4-WIFI6
 *   Waveshare PoE            https://www.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH
 *   Waveshare ESP32-P4-NANO  https://www.waveshare.com/esp32-p4-nano.htm
 *   Waveshare Module DEV KIT https://files.waveshare.com/wiki/
 *                            ESP32-P4-Module-DEV-KIT/ESP32-P4-Module-DEV-KIT.pdf
 *   ESP32-P4 Function EV     https://docs.espressif.com/projects/esp-dev-kits/
 *                            en/latest/esp32p4/esp32-p4-function-ev-board/
 *   Guition JC-ESP32P4-M3    the vendor's own pinout photograph of J1
 *   DFRobot FireBeetle 2 P4  its schematic, DFR1172_..._schematics_V1.0.pdf
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
#define BOARD_ID_BASE "p4-eth"
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
#define BOARD_ID_BASE "p4-poe"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_WAVESHARE_WIFI6_DEVKIT
/* Ethernet and a C6 on one board; its 2x20 header is not described either. */
#define BOARD_HAS_HEADERS 0
#define BOARD_NAME "Waveshare ESP32-P4-WIFI6-DEV-KIT"
#define BOARD_ID_BASE "p4-wifi6-devkit"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_WAVESHARE_MODULE_DEVKIT
/*
 * Pin for pin the PoE board's 40-pin header, which is a useful thing to find:
 * these arrays were read out of this board's own schematic netlist (P6 in
 * ESP32-P4-Module-DEV-KIT.pdf) and came out identical to the PoE pinout
 * transcribed from Waveshare's diagram. Two boards, two sources, same header.
 */
static const kvm_board_pin_t s_moddev_odd[] = {
    PWR("3V3"), IO(7),  IO(8),  IO(23), PWR("GND"), IO(21), IO(20),
    IO(6),      PWR("3V3"), IO(3), IO(2), IO(0),    PWR("GND"), IO(24),
    IO(33),     IO(26), IO(48), IO(53), IO(47),     PWR("GND"),
};
_Static_assert(sizeof(s_moddev_odd) / sizeof(s_moddev_odd[0]) == 20,
               "s_moddev_odd: the column must hold exactly 20 pins");
static const kvm_board_pin_t s_moddev_even[] = {
    PWR("5V"), PWR("5V"), PWR("GND"), IO(37), IO(38),     IO(22), PWR("GND"),
    IO(5),     IO(4),     PWR("GND"), IO(1),  IO(36),     IO(32), IO(25),
    PWR("GND"), IO(54),   PWR("GND"), IO(46), IO(27),     IO(45),
};
_Static_assert(sizeof(s_moddev_even) / sizeof(s_moddev_even[0]) == 20,
               "s_moddev_even: the column must hold exactly 20 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "GPIO", .rows = 20, .numbered = true, .left = s_moddev_odd,
     .right = s_moddev_even},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Waveshare ESP32-P4-Module-DEV-KIT"
#define BOARD_ID_BASE "p4-module-devkit"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_FIREBEETLE2_P4
/*
 * Not a 2x20: two single rows down the long edges, 18 pins on one side and 14
 * on the other, as P1 and P2 in the vendor schematic. Read from that netlist,
 * in connector order - pin 1 of P1 is the RST end of the board. The silkscreen
 * prints signal names rather than numbers, so the numbers are not shown.
 */
static const kvm_board_pin_t s_firebeetle_p1[] = {
    PWR("RST"), PWR("3V3"), PWR("GND"), PWR("GND"), PWR("GND"),
    IO(28), IO(29), IO(30), IO(8), IO(7),
    IO(48), IO(49), IO(50), IO(52), IO(4), IO(5),
    IO_NOTE(37, "UART0 TX, silk D1"), IO_NOTE(38, "UART0 RX, silk D0"),
};
_Static_assert(sizeof(s_firebeetle_p1) / sizeof(s_firebeetle_p1[0]) == 18,
               "s_firebeetle_p1: the row must hold exactly 18 pins");
static const kvm_board_pin_t s_firebeetle_p2[] = {
    IO(31), IO(34), IO_NOTE(35, "BOOT button and strapping pin"),
    IO_NOTE(36, "strapping pin"), IO(20), IO(21), IO(22), IO(23), IO(51),
    IO(33), IO(32), PWR("GND"), PWR("3V3"), PWR("5V"),
};
_Static_assert(sizeof(s_firebeetle_p2) / sizeof(s_firebeetle_p2[0]) == 14,
               "s_firebeetle_p2: the row must hold exactly 14 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "P1", .rows = 18, .numbered = false, .left = s_firebeetle_p1,
     .right = NULL},
    {.name = "P2", .rows = 14, .numbered = false, .left = s_firebeetle_p2,
     .right = NULL},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "DFRobot FireBeetle 2 ESP32-P4"
#define BOARD_ID_BASE "firebeetle2-p4"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_WAVESHARE_WIFI6
/*
 * Two rows of twenty down the long edges. Waveshare prints the carried signal
 * beside each pad instead of numbering the connector, so expose the two rows in
 * the same top-to-bottom orientation as the board's official pinout image.
 */
static const kvm_board_pin_t s_wifi6_left[] = {
    IO(52), IO(51), PWR("GND"), IO(31), IO(30), IO(29), IO(28),
    PWR("GND"), IO(50), IO(49), IO(5), IO(4), PWR("GND"), IO(3),
    IO(2), IO(8), IO(7), PWR("GND"),
    IO_NOTE(24, "USB DM"), IO_NOTE(25, "USB DP"),
};
_Static_assert(sizeof(s_wifi6_left) / sizeof(s_wifi6_left[0]) == 20,
               "s_wifi6_left: the column must hold exactly 20 pins");
static const kvm_board_pin_t s_wifi6_right[] = {
    PWR("VBUS"), PWR("VSYS"), PWR("GND"), PWR("EN"), PWR("3V3"),
    IO(20), IO(21), PWR("GND"), IO(22), IO(23), PWR("RUN"), IO(26),
    PWR("GND"), IO(27), IO(32), IO(33), IO(46), PWR("GND"), IO(47), IO(48),
};
_Static_assert(sizeof(s_wifi6_right) / sizeof(s_wifi6_right[0]) == 20,
               "s_wifi6_right: the column must hold exactly 20 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "", .rows = 20, .numbered = false, .left = s_wifi6_left,
     .right = s_wifi6_right},
};
#define BOARD_HAS_HEADERS 1
#define BOARD_NAME "Waveshare ESP32-P4-WIFI6"
#define BOARD_ID_BASE "p4-wifi6"
#define BOARD_VERIFIED true

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
#define BOARD_ID_BASE "p4-nano"
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
#define BOARD_ID "funcev"
#define BOARD_VERIFIED false

#elif CONFIG_KVM_BOARD_M5_POE_P4
/*
 * Four connectors, all of them small.
 *
 * The Hat2-Bus is the one to wire to: sixteen pins in two rows of eight. The
 * other three are single rows - a 9-pin SDIO bus, a 6-pin bus
 * carrying the console UART and the strapping pins, and a Grove port that is an
 * I2C pair and its power. The Display In add-on plugs into the SDIO and ISP
 * rows, so with capture fitted nothing on those two is free; they are drawn
 * anyway, because a board without the add-on can use them.
 *
 * From M5Stack's pin map (U213_PinMap_01) and their product photograph, which
 * shows the Hat2-Bus printed on the case: two rows of names, GND beside G44
 * through to 5V beside G22, in that order. Their documentation's tables agree.
 *
 * None of these connectors is drawn with numbers here, because the board does
 * not print any - it prints what each pin carries, the way the P4-ETH does. The
 * numbering would be a guess on top of that: M5Stack's map counts the Hat2-Bus
 * from the end their schematic of the mating hat calls sixteen, which is what a
 * mating pair does. The names are unambiguous, so the names are what is shown.
 */
static const kvm_board_pin_t s_m5_hat_odd[] = {
    PWR("GND"), PWR("5V"), IO(21), IO(20), IO(19), NC(), PWR("3V3"), PWR("5V"),
};
_Static_assert(sizeof(s_m5_hat_odd) / sizeof(s_m5_hat_odd[0]) == 8,
               "s_m5_hat_odd: the column must hold exactly 8 pins");
static const kvm_board_pin_t s_m5_hat_even[] = {
    IO(44), IO(43), IO(42), IO(41), IO(40), IO(39), IO(23), IO(22),
};
_Static_assert(sizeof(s_m5_hat_even) / sizeof(s_m5_hat_even[0]) == 8,
               "s_m5_hat_even: the column must hold exactly 8 pins");
static const kvm_board_pin_t s_m5_sdio[] = {
    IO(13), IO(12), IO(11), IO(10), IO(9), IO(8),
    IO_NOTE(27, "USB1 DP"), IO_NOTE(26, "USB1 DM"), PWR("5V"),
};
_Static_assert(sizeof(s_m5_sdio) / sizeof(s_m5_sdio[0]) == 9,
               "s_m5_sdio: the row must hold exactly 9 pins");
static const kvm_board_pin_t s_m5_isp[] = {
    PWR("GND"), IO_NOTE(35, "BOOT strap, also Ethernet TXD1"), PWR("RST"),
    IO_NOTE(38, "console UART RX"), IO_NOTE(37, "console UART TX"), PWR("3V3"),
};
_Static_assert(sizeof(s_m5_isp) / sizeof(s_m5_isp[0]) == 6,
               "s_m5_isp: the row must hold exactly 6 pins");
static const kvm_board_pin_t s_m5_grove[] = {
    PWR("GND"), PWR("5V"), IO(53), IO(54),
};
_Static_assert(sizeof(s_m5_grove) / sizeof(s_m5_grove[0]) == 4,
               "s_m5_grove: the port has exactly 4 pins");
static const kvm_board_header_t s_headers[] = {
    {.name = "Hat2-Bus", .rows = 8, .numbered = false, .left = s_m5_hat_odd,
     .right = s_m5_hat_even},
    {.name = "SDIO-Bus", .rows = 9, .numbered = false, .left = s_m5_sdio, .right = NULL},
    {.name = "ISP-Bus", .rows = 6, .numbered = false, .left = s_m5_isp, .right = NULL},
    {.name = "Grove", .rows = 4, .numbered = false, .left = s_m5_grove, .right = NULL},
};
#define BOARD_HAS_HEADERS 1
/* Two products, two names and two ids: the P4X is the rev 3.x one. */
#if CONFIG_ESP32P4_REV_MIN_300
#define BOARD_NAME "M5Stack Unit PoE-P4X"
#define BOARD_ID "m5-poe-p4x"
#else
#define BOARD_NAME "M5Stack Unit PoE-P4"
#define BOARD_ID "m5-poe-p4"
#endif
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
#define BOARD_ID_BASE "p4-guition"
#define BOARD_VERIFIED false

#else
/* Ported by hand: no published header to draw. */
#define BOARD_NAME "ESP32-P4"
#define BOARD_ID_BASE "p4-eth"
#define BOARD_VERIFIED false
#define BOARD_HAS_HEADERS 0
#endif

/*
 * The id this board's published image is named with - the same string the
 * release workflow's matrix uses, so "espkvm-<version>-<id>.bin" names a file
 * that exists. Kept beside the human name rather than derived from it, because
 * a typo here downloads an image built for another board.
 *
 * Every board except the Function EV also has a rev 3.x twin, and the two are
 * fenced against each other in the image header - neither starts on the other's
 * silicon - so the revision this build targets is part of the id. The Function
 * EV is only made on rev 3.x, so it has one image and no suffix.
 */
#ifndef BOARD_ID
#if CONFIG_ESP32P4_REV_MIN_300
#define BOARD_ID BOARD_ID_BASE "-rev3"
#else
#define BOARD_ID BOARD_ID_BASE
#endif
#endif

const char *kvm_board_id(void)
{
    return BOARD_ID;
}

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
