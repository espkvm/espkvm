# Flashing ESP-KVM

You do **not** need to build anything or install ESP-IDF to run a release. Grab a
prebuilt image from the [releases page](https://github.com/espkvm/espkvm/releases)
and write it to the board. There are two ways to do it.

Plug the board's USB-C into your computer for flashing. (The separate MX1.25 OTG
connector is what later goes to the target machine - it is not used for
flashing.)

Which chip answers on that port differs by board, and it is worth knowing before
anything goes wrong. On the Waveshare ESP32-P4-ETH it is a **CH343 USB-serial
bridge** (USB `1a86:55d3`), which needs a driver on older systems. On boards that
bring out the P4's own USB instead - the ESP32-P4 Function EV Board, for one -
the port is the chip's built-in **USB-Serial-JTAG** (USB `303a:1001`) and no
driver is involved. `lsusb` on Linux, System Information on macOS or Device
Manager on Windows says which one you have.

## Which file

Each release has, per board (`waveshare` = Waveshare ESP32-P4-ETH, `funcev` =
ESP32-P4 Function EV Board):

| File | What it is |
|---|---|
| `espkvm-<version>-<board>-merged.bin` | The whole flash in one file. **Use this.** |
| `espkvm-<version>-<board>.bin` | The app partition only (for over-the-air / advanced use; not bootable on its own). |
| `espkvm-<version>-<board>-full-flash.zip` | The same image as separate parts, for tooling that wants them. |

For a first flash from a computer, the **merged** image is the simplest: one file,
written at offset 0.

### Which revision

The ESP32-P4 exists in two silicon families, and an image built for one **refuses
to start on the other**. The same product code arrives as either chip, so boards
that ship as both have a `-rev3` file next to the plain one. Ask the board:

```bash
esptool --chip esp32p4 -p /dev/ttyACM0 chip-id
```

It prints something like `Chip type: ESP32-P4 (revision v3.1)`. **v3.x takes the
`-rev3` file; anything lower takes the plain one.** Picking wrong damages
nothing - esptool refuses the write ("requires chip revision in range
[v1.0 - v1.99]"), or, if it did write, the board waits in its boot loader until
the other file is flashed over USB. Do not reach for `--force`: it writes an
image the chip will not run.

---

## Option 1 - flash from the browser (easiest, nothing to install)

Chrome or Edge on a desktop can flash over USB with nothing installed.

1. Plug the board's USB-C into the computer.
2. Open the flasher: **https://espkvm.io/flash/**
3. Pick your board, click connect, choose the serial port, and let it write.

That is it. If the browser cannot see a port, see the driver notes below - the
same CH343 driver applies.

If the browser finds the port but says **"Failed to initialize"**, while
`esptool` on the same machine works: that is the browser failing to put the chip
into its download mode, not a problem with the image. Put the board there
yourself - hold **BOOT**, tap **RESET**, let go of BOOT - then press Install and
pick the port again. On a board whose port is the P4's own USB-Serial-JTAG the
board re-appears as a new device when it resets, so the port has to be chosen
again after that. Option 2 below always works.

---

## Option 2 - command line with esptool

### Install esptool

You need Python 3 and `esptool`:

```sh
pip install esptool          # or: pip3 install esptool
```

`esptool` v4 or v5 both work.

### Find the serial port

| OS | Typical port | Notes |
|---|---|---|
| **Linux** | `/dev/ttyACM0` | Works out of the box on modern kernels. If you get "permission denied", add yourself to the `dialout` group (`sudo usermod -aG dialout $USER`, then log out and back in) or run the command with `sudo`. |
| **macOS** | `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*` | Recent macOS recognises the CH343 on its own. If no port shows up, install WCH's [CH34x macOS driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html). |
| **Windows** | `COM3`, `COM4`, ... | Check Device Manager under "Ports (COM & LPT)". If the board shows as an unknown device, install WCH's [CH343 Windows driver](https://www.wch-ic.com/downloads/CH343SER_EXE.html) (Windows Update often installs it automatically). |

To list ports: `ls /dev/cu.*` (macOS), `ls /dev/ttyACM*` (Linux), or Device
Manager (Windows).

### Write it

Download `espkvm-<version>-<board>-merged.bin` for your board, then (substituting
your port and the file name):

```sh
# Linux
esptool --chip esp32p4 -p /dev/ttyACM0 -b 921600 write-flash 0x0 espkvm-<version>-<board>-merged.bin

# macOS
esptool --chip esp32p4 -p /dev/cu.usbserial-XXXX -b 921600 write-flash 0x0 espkvm-<version>-<board>-merged.bin

# Windows
esptool --chip esp32p4 -p COM4 -b 921600 write-flash 0x0 espkvm-<version>-<board>-merged.bin
```

If auto-reset does not start the flash, hold **BOOT**, tap **RST**, release
**BOOT**, and run the command again.

> The merged image writes the bootloader, partition table and app together at
> offset 0. It is meant for a fresh flash and resets the device's stored settings.
> To **update** an already-configured device, use the console's own
> **Settings -> System -> Update** (OTA) instead, or flash only the app
> `espkvm-<version>-<board>.bin` at `0x20000`, which leaves settings intact.

---

## First boot

After flashing, connect:

- **Ethernet** to your network,
- **HDMI** from the target machine into the capture board,
- the **MX1.25 OTG** cable to the target's USB port.

Then open **https://espkvm.local/** and sign in as **admin / admin**. The console
forces a password change before it will do anything else. The device is its own
certificate authority, so the browser warns the first time; download and trust its
CA (**Settings -> Security**, or `http://espkvm.local/cert.pem`) to clear the
warning and unlock H.264 and keyboard/mouse (which need a trusted secure context).

See the main [README](../README.md) for the rest.
