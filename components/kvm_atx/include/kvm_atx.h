/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ATX power control: "press" the target's front-panel power and reset buttons
 * through optocouplers, and read its power LED back the same way. The wiring is
 * two isolated outputs (power, reset) and one isolated input (power LED); a
 * PC817 two-channel module on each side does the whole job without a custom
 * board. See docs/wiring.md.
 *
 * Everything is configured at runtime from the settings (GPIO numbers, pulse
 * lengths, drive polarity), so the same firmware runs on a board with no ATX
 * wiring at all - it simply reports the capability as unavailable. Nothing here
 * is verified on hardware yet; the pins and polarity are guesses until a module
 * is in hand.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;   /**< ATX is enabled and the power/reset pins are valid. */
    bool have_led;  /**< A power-LED sense pin is configured. */
    bool power_on;  /**< Last read of the power LED; only meaningful with have_led. */
    bool have_hdd;  /**< An HDD-LED sense pin is configured. */
    bool hdd_busy;  /**< The HDD LED is lit or blinked in the last moment; only with have_hdd. */
} kvm_atx_status_t;

/** One-time setup. Configures nothing until kvm_atx_apply() runs. */
esp_err_t kvm_atx_init(void);

/**
 * (Re)read the settings and reconfigure the GPIOs, then report KVM_CAP_ATX.
 * Safe to call again whenever an atx_* setting changes. Pins that need a reboot
 * to change carry the KVM_SF_REBOOT flag in the settings table, so a live call
 * re-reads them but the hardware only follows on the next boot.
 */
esp_err_t kvm_atx_apply(void);

/** Fill @p out with the current state. Never fails; fields are false when off. */
void kvm_atx_status(kvm_atx_status_t *out);

/** Short press of the power button (power on, or a graceful shutdown request). */
esp_err_t kvm_atx_power_click(void);

/** Hold the power button for atx_long_ms (a hard power off). */
esp_err_t kvm_atx_power_hold(void);

/** Short press of the reset button. */
esp_err_t kvm_atx_reset(void);

/*
 * A KVM switch in front of several machines. Either one more button output,
 * wired to the switch's own select button: each press moves it to the next
 * machine, the device counts the presses to know where it is, and an optional
 * LED input on the first machine's light puts it right after a press by hand.
 * Or a TESmart switch, told the machine over the network (kvmsw_tesmart.c).
 */
typedef struct {
    bool enabled; /**< A switch is set up: a button on a GPIO, or TESmart over the network. */
    int ports;    /**< How many machines the switch has, 2 to 16. */
    int port;     /**< The machine selected now, 0-based; -1 while a network switch has not answered. */
    bool busy;    /**< Presses or a command are queued or running. */
    bool error;   /**< A network switch did not answer the last command. */
} kvm_kvmsw_status_t;

void kvm_kvmsw_status(kvm_kvmsw_status_t *out);

/** Move the switch to machine @p port (0-based), pressing as often as needed. */
esp_err_t kvm_kvmsw_select(int port);

/** One press: the next machine. */
esp_err_t kvm_kvmsw_next(void);

#ifdef __cplusplus
}
#endif
