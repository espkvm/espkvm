# Porting to another ESP32-P4 board

ESP-KVM is built and tested on the Waveshare ESP32-P4-ETH with a Geekworm C790
(TC358743) capture board, but nothing above the pin map is specific to it.
Moving to another ESP32-P4 board is a `menuconfig` edit, not a code change: the
board's pins are Kconfig options with the Waveshare values as defaults.

> It has to be an **ESP32-P4**. The value here is the MIPI-CSI capture path and
> the hardware JPEG/H.264 encoders, which the P4 has and the S3 and friends do
> not. There is no path to another chip family.

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

Take the pin numbers straight from your board's schematic.

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
  without those constraints is simpler, not harder.
