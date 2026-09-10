# Porting to another ESP32-P4 board

ESP-KVM is built and tested on the Waveshare ESP32-P4-ETH with a Geekworm C790
(TC358743) capture board, but nothing above the pin map is specific to it.
Moving to another ESP32-P4 board is a `menuconfig` edit, not a code change: the
board's pins are Kconfig options with the Waveshare values as defaults.

For a board you intend to support long-term, capture the deltas as an sdkconfig
overlay under `boards/` instead of a one-off menuconfig edit, and build it into
its own build dir. See `boards/README.md`; the Espressif ESP32-P4 Function EV
Board (chip rev v3.2, 16 MB flash) is set up this way in
`boards/funcev_p4.defaults` + `partitions_funcev.csv` as a worked example.

> Chip revisions matter. ESP32-P4 rev <3.0 and >=3.0 are mutually exclusive
> build targets (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`); a binary built for one
> family will not boot on the other. The reference board is rev v1.3; the
> Function EV Board is rev v3.2. That is why boards are built separately rather
> than as one universal image.

> It has to be an **ESP32-P4**. The value here is the MIPI-CSI capture path and
> the hardware JPEG/H.264 encoders, which the P4 has and the S3 and friends do
> not. There is no path to another chip family.

## Before you start: what rules a board out

Being an ESP32-P4 board is not enough, and none of these is a pin map away:

- **32 MB of PSRAM.** Frame buffers are allocated once for the largest mode the
  bridge can deliver - 12.5 MB at 1080p on rev 3.x (three 1920x1088 YUV422
  buffers), 11.9 MB on rev <3.0 (two 1920x1080 RGB888) - before the H.264
  encoder asks for its reference frame. On 8 MB the capture allocation fails and
  video reports itself unavailable.
- **16 MB of flash.** Two OTA slots plus the rescue image.
- **A camera connector the bridge can reach.** Two CSI-2 data lanes, and 3.3 V
  and I2C either on that connector or somewhere to wire them from. A board that
  brings only 1.5/1.8/2.8 V sensor rails to the FPC and level-shifts the I2C
  down with them needs more than a ribbon.
- **USB OTG-HS on a port or a header**, not only on an edge connector - that is
  the keyboard and mouse.
- **A network**: an Ethernet PHY, or an onboard ESP32-C6 (esp-hosted).

The LILYGO T-Halow P4 is the worked example of a board that looks right and is
not: 8 MB PSRAM, RMII and USB OTG D+/D- only on the M.2 edge, and a 24-pin
camera FPC with no 3.3 V and a 1.8 V I2C.

### The camera connector, in detail

This one is worth checking with a ruler and the schematic rather than a product
photo, because two different connectors are called "MIPI-CSI" on P4 boards:

- **15-pin, 1.0 mm pitch** - the Raspberry Pi camera connector. Pins 2/3 are
  data lane 0, 5/6 lane 1, 8/9 the clock, 11/12 the two camera control lines,
  13/14 I2C SCL and SDA, 15 is 3.3 V and 1/4/7/10 are ground. This is what the
  Geekworm C790 ships a ribbon for, and it plugs in with nothing in between.
- **22-pin, 0.5 mm pitch** - the Pi Zero / CM connector. The same signals, a
  different connector: it needs a 15-to-22-pin adapter ribbon, and those are
  sold for Pi cameras.

So "has a CSI connector" is not the question. The questions are: how many pins
and what pitch, is 3.3 V on it (some boards only bring a 1.8 or 2.8 V sensor
rail), and is the I2C on it the same 3.3 V bus the bridge's registers live on.
A board that answers 15-pin / 1.0 mm / 3.3 V / 3.3 V I2C takes the C790 ribbon
as it comes.

### Boards that have been looked at

Read from schematics and vendor documentation, not from hardware, unless the
board has its own overlay in `boards/`:

| Board | Verdict |
|---|---|
| Waveshare P4-ETH, WIFI6, WIFI6-DEV-KIT, WIFI6-POE-ETH, Module-DEV-KIT, NANO | supported; overlays in `boards/` |
| Espressif ESP32-P4 Function EV | supported; the rev 3.x reference |
| Guition ESP32-P4-M3-Dev | supported, community-tested |
| M5Stack Unit PoE-P4 / PoE-P4X | network half only - its capture add-on is an LT6911D, no driver yet |
| Waveshare ESP32-P4-NANO-WIFI6-DB | fits on paper: 32 MB PSRAM, CSI, Type-A OTG-HS, RJ45 + PoE. Its radio is an **ESP32-C5**, which esp-hosted lists as a target but nobody here has run |
| DFRobot FireBeetle 2 ESP32-P4 / AI Kit | supported; `boards/firebeetle2_p4.defaults`. WiFi only - no wired link |
| VIEWE ESP32-P4-Pi | fits on paper, with one thing to check: its USB is a Type-A **host** port, so how the role is switched and what drives VBUS both need reading |
| MakerGo / Osprey ESP32P4C5 | 15-pin CSI and an ESP32-C5, but RMII only on a header - Ethernet needs your own PHY |
| M5Stack Tab5 | everything is there, but the CSI is taken by its own camera and the tablet is a lot of board to hide behind a server |
| Olimex ESP32-P4-PC | **no**: 32 MB / 16 MB, IP101 Ethernet and a 15-pin CSI, all correct - and then an FE1.1s hub sits on the OTG-HS, so the port is permanently a host and can never present a keyboard |
| Olimex ESP32-P4-DevKit | **no**: same memory, same PHY, same connector, but D+/D- from the OTG-HS are not brought out anywhere; the Type-C is the serial/JTAG bridge |
| Waveshare ESP32-P4-Pico, ESP32-P4-Core-DEV-KIT | **no**: no network at all - no PHY and no co-processor. (The Core kit's CSI is also the 22-pin one.) |
| Espressif ESP32-P4-EYE | **no**: a camera kit; the CSI is occupied by its own sensor |
| Waveshare ESP32-P4-86-Panel-ETH-2RO | **no**: the CSI connector is not fitted on that variant |
| LILYGO T-Halow P4 | **no**: see above |

If a board is not here, open an issue with a link to its schematic.

## What to change

Run `idf.py menuconfig` and open **ESP-KVM**:

**Board pins**
- TC358743 I2C SDA / SCL
- BOOT button GPIO (password reset) - set to `-1` if the board has no reachable
  button; the reset feature is then simply off
- microSD CLK / CMD / D0-D3
- microSD slot power-gate GPIO - set to `-1` if the slot is always powered, and
  the firmware will not touch a power pin; otherwise set the gate pin and whether
  it is active-low

**Video capture**
- TC358743 RESETN GPIO (`-1` if it is not wired to a GPIO)
- TC358743 reference clock - the crystal on the capture board (27 MHz on the C790)

**Network** (only when Ethernet is enabled)
- SMI MDC / MDIO, PHY address, PHY reset GPIO
- RMII REFCLK, TX_EN, TXD0/1, CRS_DV, RXD0/1
- mDNS hostname

**Where the hand-wired things start**
- ATX power and reset button GPIOs, and the round LCD's five SPI pins

These are runtime settings - the operator picks them in the console - and these
Kconfig options only choose what a fresh device starts with. Set them anyway.
The device refuses a pin two things want, so defaults that overlap put a new
owner in an argument on their first edit; defaults naming a pin the header does
not bring out are worse, because the wire has nowhere to go and nothing says so.
Pick seven that are free - two buttons and five for the LCD - and check them
against the header, not against the chip. The LED sense is left unassigned on
purpose and needs no default.

Take the pin numbers straight from your board's schematic.

## Another capture bridge

The TC358743 is the only one this firmware ships a driver for, but it is no
longer the only one it can hold. A driver lives in its own component, fills in
`kvm_bridge_ops_t` with whatever its chip needs doing, and ends with
`KVM_BRIDGE_DRIVER(name, detect_fn)`; the capture path asks `kvm_bridge_detect()`
for whatever answers on the I2C bus and never names a chip. Two things a new
driver must not forget: the `-u kvm_bridge_reg_<name>` line in its CMakeLists,
or the linker drops an object nothing references and the driver is silently
absent, and a `PRIV_REQUIRES` entry in `video_pipeline` so the component is in
the image at all.

Worth knowing before starting: the operations were shaped around the one driver
that exists, so a second is likely to want something that is not there yet.
Espressif's `esp_cam_sensor` carries a TC358743 driver of its own and one for
the Lontium LT6911 - the same p4kvm ancestry as this one - and is worth reading,
though its bridges are fixed-mode and the LT6911 has no EDID control at all,
which a KVM needs to hold a source inside what the CSI lanes can carry.

## What stays in code

A few constants in `components/kvm_board/include/kvm_board.h` are chip- or
capture-level rather than board wiring, and are left as `#define`s: the P4's
internal MIPI-PHY LDO channel and voltage, the capture resolution, and the MIPI
lane rate. Change them only if you know why.

## Caveats

- **Ethernet PHY type.** The driver brings up an IP101-class RMII PHY. A board
  with a different PHY may need a change in `components/kvm_net/ethernet.c`
  (`esp_eth_phy_new_*`), not just the pins.
- **microSD is marginal on the reference board.** Reads run at 4 MHz and writes
  are disabled because the reference board cannot do either reliably at speed -
  a known ESP32-P4 SD limitation. A board with better SD wiring may tolerate a
  higher clock; the cap lives in `components/kvm_storage/kvm_storage.c`
  (`host.max_freq_khz`). See `HARDWARE-NOTES.md`.
- **Strapping pins.** The Waveshare BOOT button doubles as a boot strapping pin
  and shares GPIO 35 with the Ethernet interface, which is why the password
  reset reads it once at start-up and hands the pin back. A different button pin
  without those constraints is simpler, not harder. Check what your board's
  button does to the ROM: if it is the download strap (as on the Function EV),
  holding it through a reset enters download mode and the recovery window never
  runs - which is why the gesture is documented as reset first, then hold.
