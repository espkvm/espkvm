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

### Waveshare ESP32-P4-Module-DEV-KIT (chip rev unconfirmed, 16 MB flash)

```
idf.py -B build.moduledevkit \
  -D SDKCONFIG=build.moduledevkit/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/moduledevkit_p4.defaults" \
  build

idf.py -B build.moduledevkit -p /dev/ttyACM0 flash
```

The WIFI6-DEV-KIT's arrangement on a module: P4, ESP32-C6 and 16 MB of flash
under one shield, 32 MB PSRAM in the package, on a carrier with 100M Ethernet
(IP101GRI, PoE through an add-on), a microSD slot, four USB-A sockets and a 2x20
header. Read off the vendor schematic; the only delta that is not inherited is
the 16 MB flash table. The -A / -B / -C kits are the same board with a different
DSI screen in the box.

Two things about it are worth knowing before wiring:

- **The CSI connector is the 15-pin Raspberry Pi one** - two lanes, 3.3 V, I2C
  and the two camera control lines in the standard order - so the ribbon that
  comes with a C790 fits without an adapter. The control lines go nowhere but a
  pull-up, hence `CONFIG_KVM_TC358743_RST_GPIO=-1`.
- **The USB OTG-HS runs through a mux.** A jumper sends it either straight to
  one Type-A socket (what the KVM needs) or into a CH334F hub for three host
  ports. That socket drives its own VBUS, so the lead to the target has to be an
  A-to-A cable with the 5 V wire cut.

No `-rev3` twin is published yet - see the note in the overlay if a rev 3.x unit
turns up.

### DFRobot FireBeetle 2 ESP32-P4 / AI Kit (chip rev v1.0, 16 MB flash)

```
idf.py -B build.firebeetle \
  -D SDKCONFIG=build.firebeetle/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/firebeetle2_p4.defaults" \
  build

idf.py -B build.firebeetle -p /dev/ttyACM0 flash
```

60 x 25 mm, 32 MB PSRAM, 16 MB flash, an ESP32-C6-MINI-1 and no Ethernet at all,
so it runs like the ESP32-P4-WIFI6: setup hotspot first, then a station. Its
pins were read from the vendor schematic and checked against Espressif's Arduino
variant for the board (`variants/dfrobot_firebeetle2_esp32p4`), which agrees:
C6 SDIO on 18/19/14-17 with reset 54, microSD on the usual six with an active-low
power gate on 45, I2C on 7/8, BOOT on 35.

Three things that are specific to it:

- **Two USB-C ports, and they are not interchangeable.** The one by the RST
  button is USB1_P/N - the P4's GPIO 25/24, its USB-serial-JTAG: power, flashing
  and the log. The other is USB0_P/N, the OTG-HS, and that is the target's.
- **The console goes over USB-serial-JTAG**, because there is no bridge chip on
  the board. Set to UART, the log would come out on header pins nobody has
  wired and the device would look like it hung at boot (#42).
- **The CSI connector is the 15-pin Raspberry Pi one**, so the C790 ribbon fits;
  its two camera control lines are pull-ups only, so `TC358743_RST_GPIO=-1`.

### Waveshare ESP32-P4-NANO-WIFI6-DB (chip rev 3.x, 16 MB flash)

```
idf.py -B build.nanodb \
  -D SDKCONFIG=build.nanodb/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/nano_wifi6_db_p4.defaults" \
  build

idf.py -B build.nanodb -p /dev/ttyACM0 flash
```

The NANO with a dual-band **ESP32-C5** where the C6 used to be, on an external
antenna. Everything the KVM touches is the family layout, and Waveshare publish
the whole pin map on the board's documentation page, so this overlay is read off
a table rather than a netlist. The CSI connector is the 15-pin Raspberry Pi one,
so a C790 ribbon fits.

Two things make it different from every other target here:

- **It is rev 3.x silicon and only that.** The board carries an
  ESP32-P4NRW32**X**, and the X parts are what Espressif's rev 3.x guide lists.
  So there is no pre-3.0 twin: this image declares min rev 3.0, and the older
  family's image would be refused by the bootloader.
- **The co-processor is a C5,** so the esp-hosted profile is
  `ESP_HOSTED_P4X_C5_DEV_BOARD_FUNC_BOARD` - the one whose pins are CLK 18,
  CMD 19, D0-D3 14-17 and reset 54, which is what this board wires. The plain
  Function-EV preset moves CLK and CMD elsewhere when the co-processor is a C5.
  Dual-band means this is the first board here that can sit on a 5 GHz network,
  and none of it has been powered up yet.

Two `CONFIRM` items, both in the overlay: whether the Type-C reaches UART0
through a bridge (as on the NANO) or is the P4's own USB-serial-JTAG, and the
C5's own firmware version. Waveshare has not published a schematic for this SKU.
Its two 2x13 headers are drawn from Waveshare's pinout picture. The left one is
the NANO's pin for pin; the right one differs only where the NANO has its C6 -
three pins are the C5's serial and boot lines, and two are USBD_P and USBD_N,
the P4's high-speed USB. So the target can be wired from the header.

### VIEWE ESP32-P4-Pi (chip rev 1.3, 16 MB flash)

```
idf.py -B build.viewe \
  -D SDKCONFIG=build.viewe/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/viewe_p4_pi.defaults" \
  build

idf.py -B build.viewe -p /dev/ttyACM0 flash
```

A Raspberry-Pi-shaped carrier for VIEWE's own ESP32-P4-Core module (P4, C6 and
16 MB flash under one shield). IP101GRI Ethernet, microSD, a 15-pin camera FPC,
a 40-pin header, and audio and an IMU the KVM does not use. Both schematics are
published, carrier and module, so every pin here is read off a netlist - down to
the C6's SDIO wiring, which on the other module boards had to be inferred. All
of it lands on the firmware defaults: Ethernet on 28-31/34/35/49-52 with PHY
address 1, microSD on 43/44/39-42 with an active-low gate on 45, I2C on 7/8,
BOOT on 35, C6 SDIO on 18/19/14-17 with enable on 54.

Three things specific to it:

- **Three USB ports.** Two Type-C: the one marked UART is a CH340C for power,
  flashing and the log; the other is the OTG-HS and that is the target's, so the
  lead is C-to-A. The Type-A socket is the full-speed port as a host and the KVM
  does not use it.
- **PoE is half wired.** The RJ45's centre taps come out on a 4-pin header for
  an external module, but there is no pin to feed the board's 5 V back in - that
  has to go to the expansion header.
- **The 40-pin header is the Waveshare PoE header, pin for pin**, which is now
  three boards with the same layout. VIEWE's schematic numbers the connector
  mirrored; their pinout picture and the silkscreen agree with everyone else.
  One pin to avoid: GPIO 6, on pin 16, is tied to the C6's IO2 inside the
  module.

No `-rev3` twin: the module's lid reads ESP32P4NRW32, without the X.
