# Board targets

ESP-KVM is built per board (separate binaries), not as one universal image: the
ESP32-P4 rev <3.0 and >=3.0 families are mutually exclusive build targets, and
the fps-relevant H.264 RGB path is a compile-time choice tied to the minimum chip
revision.

## Waveshare ESP32-P4-ETH (chip rev v1.3, 32 MB flash) - default

The default build:

```
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Config: `sdkconfig.defaults` + `partitions.csv` (32 MB, `storage` and `rescue`
both 4 MB). Chip target rev <3.0.

## Waveshare ESP32-P4-WIFI6 (chip rev v1.3, 32 MB flash)

The WiFi-only SKU uses its onboard ESP32-C6 over SDIO and opens its own setup
hotspot on a fresh ESP-KVM install:

```
idf.py -B build.waveshare_wifi6 \
  -D SDKCONFIG=build.waveshare_wifi6/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare_p4_wifi6.defaults" \
  build

idf.py -B build.waveshare_wifi6 -p /dev/ttyACM0 flash
```

Config deltas: Ethernet disabled, WiFi/esp-hosted enabled on the documented
Function-EV-compatible SDIO pins, microSD power-gated by GPIO45, and the UART
console retained for the onboard CH343 bridge. The separate four-pin USB port
is the target-facing HID connection. The expansion-header layout was checked
against the physical board.

## Espressif ESP32-P4 Function EV Board (chip rev v3.2, 16 MB flash)

An overlay on the common defaults, built into a separate directory so it never
clobbers the default build:

```
idf.py -B build.funcev \
  -D SDKCONFIG=build.funcev/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/funcev_p4.defaults" \
  build

idf.py -B build.funcev -p /dev/ttyACM0 flash
```

Config deltas (in `boards/funcev_p4.defaults`): chip target rev >=3.0
(`REV_MIN_300`, which unlocks the esp_h264 RGB-direct path), 16 MB flash with
`partitions_16mb.csv` (`storage` is 3.875 MB), the microSD power-gate
disabled, and `CONFIG_KVM_WIFI=y` - this board carries an onboard ESP32-C6, so
WiFi (station, access point, and the rescue hotspot) is compiled in via
esp_wifi_remote + esp-hosted over SDIO. The Waveshare board has no radio and
leaves it off. The I2C, microSD and Ethernet management pins (IP101: MDC/MDIO/refclk/
reset/addr) match the Waveshare reference and are inherited. The RMII data pins
and the BOOT button GPIO should be taken from the board schematic and set in the
overlay if a given board differs.

## Configured from datasheets, not yet tested

These two boards have a build target configured entirely from their datasheets -
each carries an ESP32-P4, a MIPI-CSI connector, USB OTG-HS, IP101 Ethernet and an
onboard ESP32-C6 (same SDIO wiring as the Function EV), so in theory they should
work. **Nobody has run ESP-KVM on either yet.** A few pins - the RMII data lines,
the microSD lines, the BOOT button, and the chip revision - are only a
best-guess from the reference design and are marked `CONFIRM` in the overlays;
they may need correcting from the board schematic. Reports from anyone with the
hardware are very welcome.

### Waveshare ESP32-P4-NANO (chip rev unconfirmed, 16 MB flash)

```
idf.py -B build.nano \
  -D SDKCONFIG=build.nano/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/nano_p4.defaults" \
  build

idf.py -B build.nano -p /dev/ttyACM0 flash
```

Its C6 SDIO and IP101 Ethernet management pins are documented and match the
reference, so those are wired; see `boards/nano_p4.defaults` for the `CONFIRM`
items.

### Guition ESP32-P4-M3-Dev / JC-ESP32P4-M3 (chip rev unconfirmed, 16 MB flash)

```
idf.py -B build.guition \
  -D SDKCONFIG=build.guition/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/guition_p4.defaults" \
  build

idf.py -B build.guition -p /dev/ttyACM0 flash
```

A display board - the MIPI-DSI touch panel is unused by the KVM. It has two USB-C
ports; the target must be on the OTG-HS one. Being Guition's own design (not a
Waveshare layout), its undocumented data pins are less certain than the NANO's;
see `boards/guition_p4.defaults`.
