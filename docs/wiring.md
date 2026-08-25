<!--
SPDX-FileCopyrightText: 2026 ESP-KVM contributors
SPDX-License-Identifier: Apache-2.0
-->

# ATX power control wiring

ESP-KVM can press the target PC's front-panel power and reset buttons and read
its power LED, so the console can turn the machine on, off, hard-off and reset,
and show whether it is running. None of it needs a custom board: two cheap
optocoupler modules and a few jumper wires do the job.

This is optional. With nothing wired, the firmware reports ATX as unavailable
and the console hides the controls. Everything below is set at runtime under
**Settings -> Power** - no rebuild to change a pin or fix a polarity.

![ATX wiring: ESP32-P4 GPIO to PC817 optocouplers to the motherboard front-panel header](atx-wiring.svg)

## Why optocouplers

The target and the KVM must not share a ground through these signals: the front
panel switches sit at the motherboard's own reference, and tying the ESP32
directly to them risks ground loops and, if anything is miswired, the P4. An
optocoupler passes the signal as light, so the two sides stay electrically
isolated. A relay would also isolate, but it has no polarity to get wrong on the
switch side - and it cannot sense the LED, and some motherboards do not like the
contact bounce for a momentary power press. Optocouplers cover both directions
with one kind of part.

A **PC817 two-channel optocoupler module** (the small board with screw
terminals, about a euro) is all you need - one for the two outputs (power,
reset), one for the LED input. These are "high-level trigger" boards: driving
the input high turns the optocoupler on, which is the firmware default
(`atx_active_high` on). If yours presses on a low instead, flip that setting.

## The front-panel header

The buttons and LEDs are on the motherboard's front-panel header - the same
block the PC case's power button, reset button and LEDs plug into. Vendors name
and arrange it differently (`F_PANEL`, `PANEL1`, `JFP1`, ...), so there is no
single universal pinout; always read the silkscreen next to the header or the
motherboard manual.

As one example, a common layout groups the signals in two rows like this:

```
        (even pins)                 (odd pins)
   2  PW   power switch        1  PLED+  power LED anode
   4  PW   power switch        3  PLED-  power LED cathode
   6  RES  reset switch        5  HD+    HDD LED anode
   8  RES  reset switch        7  HD-    HDD LED cathode
                               9  NC
```

Yours may differ - confirm the exact pins before wiring.

Two things matter:

- **Power and reset are just switches.** Shorting the two pins is a press;
  there is no polarity to a contact. With an optocoupler output (a transistor),
  though, there *is* a polarity: the collector goes to the pin that reads a small
  positive voltage to ground (measure it with a multimeter on a plugged-in but
  powered-off machine), the emitter to the other.
- **Sense the power LED, not the HDD LED.** `HD` only blinks on disk activity;
  `PLED` tells you the machine is on. LEDs have polarity - wire `PLED+` to the
  module input's `+`, `PLED-` to its `-`.

Many motherboards drive the power LED through a modest series resistor, so a
stock PC817 input module (about 1 kohm in series) may only pass a milliamp or
two and read weakly. If the reported power state is unreliable, lower that
module's input resistor to ~220-330 ohm - the one place a soldering iron might be
needed.

## To the ESP32-P4

Pick any free GPIO for each signal. On the Waveshare ESP32-P4-ETH these are safe
defaults (not used by the capture, Ethernet, microSD, USB or console UART, and
not strapping pins):

| Signal | Setting | Suggested GPIO |
|---|---|---|
| Power button output | `atx_pwr_gpio` | 20 |
| Reset button output | `atx_rst_gpio` | 21 |
| Power LED input | `atx_led_gpio` | 22 |

Confirm those pins are broken out on your board's 40-pin header. GPIO 9-13 are
left free deliberately - the board routes its I2S there for a future HDMI-audio
capture.

On the module, the `IN` terminals are the input (driven side) and the `V`
terminals are the output (switched side): for the two button channels the ESP32
drives `IN`, and `V` goes across the button pins; for LED sensing the power LED
drives `IN`, and the ESP32 reads `V`.

## Settings

Under **Settings -> Power**:

| Setting | Meaning | Default |
|---|---|---|
| `atx_enable` | Turn ATX control on | off |
| `atx_pwr_gpio` | Power button GPIO | -1 (off) |
| `atx_rst_gpio` | Reset button GPIO | -1 (off) |
| `atx_led_gpio` | Power LED sense GPIO | -1 (no sensing) |
| `atx_short_ms` | Normal press length | 200 ms |
| `atx_long_ms` | Hard-off hold length | 5000 ms |
| `atx_active_high` | Drive high to press (high-level-trigger module) | on |
| `atx_led_active_high` | LED reads high when the target is on | on |

Set the pins and turn `atx_enable` on; the power button at the bottom of the
console's side rail lights up.
If the target reacts to the wrong button, or the reported power state is
inverted, flip the matching polarity setting - no reflash.
