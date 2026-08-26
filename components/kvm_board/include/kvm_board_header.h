/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board's expansion header, as it is printed on the board.
 *
 * Everything else about pins in this firmware is a GPIO number, because that is
 * what the chip and the settings deal in. But nobody solders to a GPIO number:
 * they put a wire on a pin of a connector. The two are not the same list - a P4
 * has 55 GPIOs and a board brings maybe half of them out - so the console needs
 * to be told which pins exist physically, in the order they sit on the board.
 *
 * Only the board this image was built for is compiled in, and a board with no
 * entry here simply has no header to draw; the Pins tab then lists GPIOs, which
 * is what it always did.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One pin of a header. */
typedef struct {
    /** The GPIO it carries, or -1 when it is not one (power, ground, nothing). */
    int8_t gpio;
    /** What it is when it is not a GPIO: "3V3", "5V", "GND", "EN", "" for a gap. */
    const char *label;
    /** Anything an operator has to know before using it, or NULL. */
    const char *note;
} kvm_board_pin_t;

/**
 * One header, as two columns read top to bottom with the board the way its
 * silkscreen is written.
 *
 * @c numbered says whether the pins carry printed numbers. Where they do, the
 * numbering is the usual one - left column odd, right column even - so pin 7 is
 * the fourth entry of @c left. Where they do not, the pins are known by their
 * labels alone and a position in the column is all there is.
 */
typedef struct {
    const char *name; /**< silkscreen name ("J1", "GPIO"), or "" when it has none */
    uint8_t rows;     /**< pins per column */
    bool numbered;
    const kvm_board_pin_t *left;
    const kvm_board_pin_t *right; /**< NULL for a single column */
} kvm_board_header_t;

/** What to call this board in the interface. */
const char *kvm_board_name(void);

/**
 * The id this board's published firmware image carries, e.g. "funcev" or
 * "p4-eth-rev3". Matches the release workflow's board matrix, so it names the
 * asset "espkvm-<version>-<id>.bin" in a GitHub release.
 */
const char *kvm_board_id(void);

/**
 * Whether the header layout below was checked against the board in front of us
 * rather than only read off the vendor's diagram. The console says so, because
 * a pinout nobody has confirmed is a wire in the wrong place.
 */
bool kvm_board_header_verified(void);

/**
 * The board's headers, or NULL when this board has no description here.
 * @param[out] count how many there are
 */
const kvm_board_header_t *kvm_board_headers(size_t *count);

#ifdef __cplusplus
}
#endif
