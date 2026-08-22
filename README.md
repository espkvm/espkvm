<p align="center">
  <img src="docs/icon.svg" width="96" height="96" alt="">
</p>

# ESP-KVM

<p align="center">
  <a href="https://github.com/espkvm/espkvm/actions/workflows/firmware.yml"><img src="https://github.com/espkvm/espkvm/actions/workflows/firmware.yml/badge.svg" alt="Build"></a>
  <a href="https://github.com/espkvm/espkvm/releases/latest"><img src="https://img.shields.io/github/v/release/espkvm/espkvm?sort=semver" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/espkvm/espkvm" alt="License: Apache-2.0"></a>
  <img src="https://img.shields.io/badge/ESP--IDF-v6.0.1-blue" alt="ESP-IDF v6.0.1">
  <img src="https://img.shields.io/badge/target-ESP32--P4-informational" alt="Target: ESP32-P4">
  <br>
  <a href="https://t.me/espkvm"><img src="https://img.shields.io/badge/Telegram-%40espkvm-26A5E4?logo=telegram&logoColor=white" alt="Telegram: @espkvm"></a>
  <a href="https://x.com/espkvm"><img src="https://img.shields.io/badge/X-%40espkvm-000000?logo=x&logoColor=white" alt="X: @espkvm"></a>
</p>

An IP-KVM built from an ESP32-P4 and a Toshiba TC358743 HDMI-to-CSI bridge. It
captures the target machine's HDMI output, presents itself to that machine as a
USB keyboard and mouse, and puts both in a browser.

The point is to reach a machine that has no working operating system - a BIOS
screen, a boot menu, a kernel that will not come up - from a device that costs a
fraction of a commercial KVM-over-IP.

<!-- The buttons sit below the description on purpose: a search engine quotes the
     first prose in this file, and that should say what the project is rather than
     ask for a star. The ask itself lives in Support, at the bottom. -->
<p align="center">
  <a href="https://github.com/espkvm/espkvm"><img src="docs/star-on-github.svg" height="44" alt="Star us on GitHub"></a>
  &nbsp;
  <a href="https://buymeacoffee.com/dexif"><img src="https://img.buymeacoffee.com/button-api/?text=Buy%20me%20a%20coffee&emoji=&slug=dexif&button_colour=FFDD00&font_colour=000000&font_family=Inter&outline_colour=000000&coffee_colour=ffffff" height="40" alt="Buy me a coffee"></a>
</p>

![The ESP-KVM console driving a real machine: its desktop, a right-click menu open on it, the live status bar, and the video settings panel.](docs/console.webp)

> **Built on [jrowny/p4kvm](https://github.com/jrowny/p4kvm).** The hard part -
> bringing up the TC358743 and getting frames out of the ESP32-P4's CSI
> receiver - was solved there first, and this project would not exist without
> it. See [Credits](#credits).

## Status

Useful for what it does today, and honest about the rest.

| | |
|---|---|
| Video capture, following the target's resolution changes | works |
| MJPEG streaming | works |
| H.264 streaming | works; needs HTTPS in the browser (see below) |
| Keyboard, absolute and relative pointer, media keys | works |
| Pasting text with a keyboard layout | works; US English, Russian, Czech, Ukrainian and Lithuanian. Characters the layout cannot produce are reported rather than guessed at |
| Use from a phone or tablet (touch trackpad and on-screen keyboard) | works |
| Multiple viewers, one in control at a time with takeover | works |
| User-defined key macros | works |
| Settings, capability reporting, diagnostics | works |
| Firmware update over the network, with rollback | works |
| HTTPS with a certificate the device issues itself | works; a downloadable CA you can trust to clear the warning and enable H.264 |
| Bring your own TLS certificate | works; install your own cert and key (Settings, or `PUT /api/v1/tls/cert`), self-signed by default |
| Login, and a physical password reset | works |
| Thermal protection | works |
| Virtual media: boot the target from a disk image | works; from a FAT32 card (up to 4 GB each) or a small image in the device's own flash |
| Reading a text screen as text (BIOS, boot loader, console) | works; select and copy with the mouse, or copy the whole screen. Character modes only &mdash; a graphical UEFI setup is a picture and is not read. Screens up to 1024x768 are read as they come; wider ones, up to 1080p, are read while you ask for them |
| Watching the screen for words while nobody is looking | works; off by default. Give it phrases (`no boot device`, `kernel panic`) and it alerts in the log and in Home Assistant when one appears |
| Guessing the target's OS from how it enumerates USB | works |
| Wake-on-LAN (magic packet to the target's MAC) | works |
| WiFi &mdash; station or its own access point, on boards with an ESP32-C6 | works; verified on the ESP32-P4 Function EV. Also built for the NANO, Guition and PoE boards, which carry the same co-processor; the Waveshare ESP32-P4-ETH has no radio at all. One link at a time (Ethernet, WiFi, or AP). A rescue hotspot keeps a device reachable if its network is out of range, and a captive portal opens the console from a phone on connect |
| ATX power control (power/reset buttons and power LED through optocouplers) | works; wiring in [docs/wiring.md](docs/wiring.md) |
| Small status display (IP, link, capture, health) | works; optional, off by default. An I2C OLED (SSD1306/SH1106, auto-detected on the capture bus) or a round GC9A01 colour LCD. Enable it and assign any pins from the console |
| Home Assistant integration over MQTT | works; off by default, auto-discovered sensors and power/reset/Wake-on-LAN buttons, TLS optional |
| VPN &mdash; WireGuard or native Tailscale | works; off by default, pick one in Settings &rarr; VPN. WireGuard is a split-tunnel client with on-device key generation; Tailscale joins a tailnet natively (a 100.x address reachable from anywhere, NAT traversal handled, no gateway or port-forward). Both share one WireGuard stack |
| HDMI audio | not implemented |

What is coming next is in the [roadmap](ROADMAP.md).

<p align="center">
  <img src="docs/ha.png" width="300" alt="The ESP-KVM device in Home Assistant, its sensors and diagnostics reported live over MQTT.">
</p>
<p align="center"><em>The device in Home Assistant &mdash; every value reported live over MQTT, discovered automatically.</em></p>

**Still: do not put this on the public internet.** There is a login now, and
TLS. But nothing here has been through a security review, and a device that
holds the keyboard of someone else's machine is worth attacking. Keep it on a
network you trust, or reach it over a VPN. It has two built in, in Settings
&rarr; VPN.

Tailscale needs no port forward, no gateway and no VPS: the device joins your
tailnet and answers at its 100.x address, or its MagicDNS name, from anywhere.
Its certificate is valid there too. WireGuard is a split tunnel - it makes its
own key on the device and shows the public half in the VPN tab, to register on
your hub.

## Hardware

An ESP32-P4 board does the work, a TC358743 bridge turns the target's HDMI into a
stream it can read, and a 15-pin CSI ribbon joins the two. An optocoupler module
is an optional add-on for ATX power control.

### The device — pick one ESP32-P4 board

<table>
<tr>
<td width="50%"><img src="docs/board-p4.webp" alt="Waveshare ESP32-P4-ETH board"></td>
<td width="50%"><img src="docs/esp32-p4x-function-ev-board-isometric_v1.6.png" alt="Espressif ESP32-P4 Function EV Board"></td>
</tr>
<tr>
<td valign="top">

**[Waveshare ESP32-P4-ETH](https://www.waveshare.com/esp32-p4-eth.htm)** &mdash; chip rev v1.3, the default

ESP32-P4 with 32 MB PSRAM, 32 MB flash, 100M Ethernet, a Raspberry-Pi-compatible
CSI connector, USB 2.0 OTG HS and a microSD slot. Built by plain `idf.py build`.

</td>
<td valign="top">

**[Espressif ESP32-P4 Function EV Board](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html)** &mdash; chip rev v3.2

Espressif's own board, with an onboard ESP32-C6 &mdash; so it also does WiFi
(station, access point, and the rescue hotspot). Its own build target
(`boards/funcev_p4.defaults`), and the [browser flasher](https://espkvm.io/flash/)
offers it directly. The rev 3.2 silicon feeds native YUV422 straight into the
H.264 and JPEG encoders (no PPA colour-convert pass), which frees ~4 MB of PSRAM
for a deeper capture ring and lifts 1080p to a little over 20 fps.

</td>
</tr>
</table>

Any other ESP32-P4 board with Ethernet and the same CSI connector can run it too
&mdash; the pins are set in [menuconfig](docs/PORTING.md), not in the code.

**The chip revision matters.** Below revision 3.0 several peripherals behave
differently and rev <3.0 and >=3.0 are mutually exclusive build targets. On rev
<3.0 the colour conversion the H.264 encoder needs goes through the PPA; on rev
>=3.0 the encoders take the captured YUV422 (or RGB) directly and the PPA is not
used. The default build (`sdkconfig.defaults`) selects the pre-3.0 family, so a
v1.x part builds and runs as shipped; a rev 3.x board is built from its own
overlay (see [boards/](boards/README.md)). What was measured on the boards in
front of us, including the documented claims that turned out to be false, is
written down in [docs/HARDWARE-NOTES.md](docs/HARDWARE-NOTES.md).

### More boards

These carry an ESP32-P4, a MIPI-CSI connector, USB OTG-HS, IP101 Ethernet and an
onboard ESP32-C6, and a contributor has confirmed capture, USB and Ethernet on both.
The units tested were pre-3.0 silicon, so the overlays build for that by default (a
rev-3.x unit can flip one config for the faster H.264 path). WiFi and the finer
details aren't exhaustively tested yet - reports welcome.

<table>
<tr>
<td width="50%"><img src="docs/board-nano.jpg" alt="Waveshare ESP32-P4-NANO board"></td>
<td width="50%"><img src="docs/board-guition.webp" alt="Guition ESP32-P4-M3-Dev (JC-ESP32P4-M3) board"></td>
</tr>
<tr>
<td valign="top">

**[Waveshare ESP32-P4-NANO](https://www.waveshare.com/esp32-p4-nano.htm)** &mdash; *community-tested*

Same IP101 Ethernet and onboard ESP32-C6 as the boards above; 32 MB PSRAM, 16 MB
flash. Build overlay: `boards/nano_p4.defaults`.

</td>
<td valign="top">

**Guition ESP32-P4-M3-Dev (JC-ESP32P4-M3)** &mdash; *community-tested*

A display board (4.3&Prime; MIPI-DSI touch, unused by the KVM) that also carries
Ethernet and an ESP32-C6; 32 MB PSRAM, 16 MB flash. It has two USB-C ports - the
target goes on the OTG-HS one. Build overlay: `boards/guition_p4.defaults`.

</td>
</tr>
</table>

<table>
<tr>
<td width="50%"><img src="docs/board-poe.jpg" alt="Waveshare ESP32-P4-WIFI6-POE-ETH board"></td>
<td width="50%" valign="top">

**[Waveshare ESP32-P4-WIFI6-POE-ETH](https://www.waveshare.com/esp32-p4-wifi6-poe-eth.htm)**
&mdash; *built from the schematic, not yet run on one*

The first supported board that takes **PoE**, so a KVM in a rack needs one cable
instead of two. Same IP101 Ethernet, same microSD wiring and the same ESP32-C6
over SDIO as the boards above, on the same pins; 32 MB PSRAM, 32 MB flash, and a
full-size USB-A port for the target. Build overlay: `boards/poe_p4.defaults`.

It ships built for pre-3.0 silicon, which is most likely what is in the box:
Waveshare confirmed (August 2026) that these boards ship rev 1.3 today, with rev
3.x still ramping up at Espressif. **Check the boot log before flashing** - it
prints `Chip rev:`. The two revisions need different images and neither runs on
the other's silicon, so a rev 3.x board wants the `-rev3` build instead. A
product code does not tell you which chip is inside; ask the seller.

</td>
</tr>
</table>

### Companion boards

<table>
<tr>
<td width="50%"><img src="docs/board-c790.webp" alt="Geekworm C790 TC358743 HDMI-to-CSI capture board"></td>
<td width="50%"><img src="docs/817.webp" alt="PC817 two-channel optocoupler isolation module"></td>
</tr>
<tr>
<td valign="top">

**The capture — [Geekworm C790](https://wiki.geekworm.com/C790)** (required)

A TC358743 HDMI -> MIPI CSI-2 bridge that turns the target's HDMI output into a
camera stream the ESP32-P4 can read. Any other TC358743 capture board should do
just as well.

</td>
<td valign="top">

**ATX power control — an optocoupler module** (optional)

A small board that presses the target's power and reset buttons and senses the
power LED without a direct electrical connection. Wiring is in
[docs/wiring.md](docs/wiring.md).

</td>
</tr>
</table>

**Status display — a small OLED or round LCD** (optional)

<table>
<tr>
<td width="50%"><img src="docs/SSD1306.jpg" alt="SSD1306/SH1106 128x64 I2C OLED module"></td>
<td width="50%"><img src="docs/GC9A01.webp" alt="GC9A01 240x240 round colour SPI LCD module"></td>
</tr>
<tr>
<td valign="top">

**I2C OLED (SSD1306 / SH1106)**

A 0.96&Prime; 128&times;64 mono OLED on four wires (VCC, GND, SCL, SDA). It shares
the capture chip's I2C bus, needs no pins of its own, and is auto-detected. In
hotspot mode it prints the network name and its password, so a phone can join
from what is on the glass.

</td>
<td valign="top">

**Round colour LCD (GC9A01)**

A 1.28&Prime; 240&times;240 round SPI LCD, e.g. the Waveshare module. The cheap
1.5&Prime; GC9A01A modules work too. Wire its SPI pins to any free GPIOs and pick
them in the console. In hotspot mode it shows a QR code a phone camera can join
from.

Settings &rarr; Pins draws the board's expansion header the way it is printed,
with what holds each pin, so a free GPIO can be found where the wire actually
goes rather than in a list of numbers.

</td>
</tr>
</table>

Both are off by default &mdash; turn the display on in Settings and choose its type.

The LCD needs five free GPIOs, and which ones are free depends on the board - so
a build offers a set that is known to work there, and you only change them if you
wired it differently. Boards that nobody has checked yet offer the Function EV
set. What is offered:

| Board | SCLK | MOSI | CS | DC | RST |
|---|---|---|---|---|---|
| ESP32-P4 Function EV | 20 | 21 | 22 | 26 | 33 |
| Waveshare ESP32-P4-NANO | 22 | 5 | 32 | 33 | 36 |

On the Function EV board, **do not use GPIO 45 for DC** &mdash; it carries SD_PWRn,
so the panel gets no clean logic level and stays dark with nothing in the log.
The NANO pins come from [@DaveDavenport](https://github.com/DaveDavenport), who
tested them.

**Cables:** a 15-pin CSI ribbon between the two boards, HDMI from the target,
and a cable from the board's USB 2.0 OTG-HS port to the target. Add a microSD
card if you want boot-from-image.

Which connector the OTG-HS port is depends on the board. The Function EV, NANO
and Guition put it on USB-C, and the PoE board on a full-size USB-A. On the
Waveshare ESP32-P4-ETH it is not a USB socket at all but the **MX1.25 header**,
so that one needs an MX1.25-to-USB-A cable. The other USB-C on any of these
boards is the serial bridge, for flashing.

<sub>Board and module photos (c) their makers, taken from product pages and used
only to identify the hardware. ESP-KVM is not affiliated with
[Espressif](https://github.com/espressif), [Waveshare](https://github.com/waveshareteam),
[Geekworm](https://github.com/geekworm-com) or [Guition](https://github.com/guitionofficial).
The pin map is in `components/kvm_board/include/kvm_board.h`.</sub>

## Quick start

Download your board's `espkvm-<version>-<board>-merged.bin` (for example
`-p4-eth-` for the Waveshare ESP32-P4-ETH, `-funcev-` for the Function EV) from the
[releases](https://github.com/espkvm/espkvm/releases) and write it at offset 0
with [esptool](https://github.com/espressif/esptool) - one file, no unpacking:

```sh
esptool --chip esp32p4 -b 921600 write-flash 0x0 espkvm-<version>-<board>-merged.bin
```

Or flash straight from the browser - Chrome or Edge, nothing to install - at
[espkvm.io/flash](https://espkvm.io/flash/). Full step-by-step instructions,
including the serial-port and driver notes for Linux, macOS and Windows, are in
[docs/FLASHING.md](docs/FLASHING.md).

Then connect Ethernet, HDMI from the target, and the board's USB 2.0 OTG-HS port
to the target (see **Cables** above - on the Waveshare ESP32-P4-ETH that port is
the MX1.25 header, not the USB-C socket). The device announces itself over mDNS:
open **https://espkvm.local/**.

The device is its own certificate authority. It makes a CA on first boot and
signs its own certificate with it, so the browser warns you the first time.
Clicking through that warning is enough to sign in and watch the MJPEG stream.

H.264, the keyboard and the mouse need more: a real *secure context*, which a
self-signed certificate does not give until it is trusted. Without it the browser
runs neither the WebSocket channel nor the H.264 decoder.

To clear the warning for good, trust the device's CA once. Download it from
**Settings -> Security -> Download CA certificate**, or from
`http://espkvm.local/cert.pem` - that one is served in the clear, so you can
fetch it before trusting anything. Import it into your operating system or
browser as a **trusted authority**, not under "your certificates". Then reach the
device by name, at **https://espkvm.local**.

On a static IP the certificate names the address as well, so `https://<ip>` works
once the CA is in. On DHCP, use the name.

The device is dual-stack. On a network that advertises IPv6 it takes an address
there too, with nothing to configure, and `espkvm.local` resolves to it. From the
next restart the certificate names those addresses, so `https://[address]/` is
trusted like the v4 one. There is a switch in **Settings -> Network -> IPv6**.

Tapping the **network icon** in the console footer shows the whole picture: what
the device is connected by, the name it answers to, its IPv4 address, every IPv6
address with what each is good for, and the MAC for a DHCP reservation. Each one
is a link and has a Copy button.

A network with no IPv4 at all works too. The one thing that cannot follow is
Wake-on-LAN - the magic packet is an IPv4 broadcast, and IPv6 has none - so the
console says it is unavailable instead of pretending. The WireGuard and Tailscale
tunnels still need a peer reachable over IPv4.

Sign in as **admin / admin**. The console will not go any further until that
password is changed: a KVM left on the password it shipped with is a keyboard
on someone else's machine, offered to whoever finds it.

After that the cable is only needed if something goes badly wrong - updates are
installed from the console itself.

## If it does not work

Five things account for most of it, and each has a way to tell it apart from
the others without a multimeter.

**The target never sees a keyboard or a mouse.** Check which connector the cable
is in. On the Waveshare ESP32-P4-ETH the USB-C socket is only the serial bridge,
for flashing and the log. The port that presents the keyboard and mouse is the
small 4-pin **MX1.25 header** next to it, marked `USB` on the silkscreen. No
cable from that USB-C will ever reach the target. The other boards put the same
port on USB-C or USB-A - see **Cables** above.

To check it from the device instead of by eye, open
`https://espkvm.local/api/v1/system/usbprobe`. An empty trace means the target
has never seen the KVM at all, which is exactly what a cable in the serial port
looks like.

**The keyboard does not work, or the picture never starts and the console keeps
reconnecting.** Both are the same cause, seen from two sides: the keyboard, the
mouse and H.264 all ride a WebSocket, and a browser only opens one on a page it
considers secure - which a self-signed certificate is not, until it is trusted.
MJPEG and signing in work anyway, which is why the rest looks fine.

One step tells it apart from a capture fault: set **Settings -> Video -> Stream
codec** to `mjpeg`, which is plain HTTP. If the picture appears, it is the
certificate. Install the device's CA and reach it by name - Quick start says how.

**No signal, or the target only shows its screen after it has booted.** The KVM
holds HDMI hotplug low until it has loaded its EDID and started capture, and that
takes a good ten seconds from power on. A machine that looks for a monitor during
its own POST, and finds none, may decide it has no external screen and never turn
the output on. So power the KVM first, or replug HDMI once the KVM is up. If the
picture arrives but the source refuses a mode, try a capped **EDID profile** in
Settings -> Video.

**A pin you picked in Settings does nothing.** It may not be a pin at all. The P4
has 55 GPIOs and a board brings out maybe half. **Settings -> Pins** draws the
board's expansion header the way it is printed, with what holds each pin, so you
can find a free one where the wire actually goes. Some pins are also taken by
hardware the firmware never touches: on the Function EV, GPIO 45 carries the
microSD power net whether we use it or not.

Flashing problems - the port not appearing, drivers, permissions - are in
[docs/FLASHING.md](docs/FLASHING.md). Anything else: the device keeps its own log
across a restart, and **Diagnostics -> Download the log** is the fastest way to
show what happened.

## Building from source

The web console lives in a submodule ([espkvm/console](https://github.com/espkvm/console)),
so clone with `--recursive` (or run `git submodule update --init` in an existing
clone):

```sh
git clone --recursive https://github.com/espkvm/espkvm
tools/install-idf.sh                          # ESP-IDF 6.0.1, once
. tools/env.sh
cd web && npm ci && npm run build && cd ..    # the console is embedded in the firmware
idf.py build
idf.py -p /dev/ttyACM0 -b 921600 flash
```

There is no `menuconfig` step for the default board: everything it needs is in
`sdkconfig.defaults`. To build for another board, apply its overlay - see
[boards/](boards/README.md).

The console can also be developed against a simulated device, with no hardware
attached at all:

```sh
cd web && npm run dev:mock
```

## How it works

![The target's HDMI goes to the TC358743 capture board, which feeds the ESP32-P4 over MIPI CSI-2. The ESP32-P4 reaches your browser over HTTPS on the LAN, and plugs back into the target as a USB keyboard and mouse.](docs/diagram.svg)

**Video.** The bridge is polled for signal state and timings, so a target
switching from an 800x600 firmware screen to a 1080p desktop is followed
without intervention. Encoding is done by hardware - the JPEG engine, or the
H.264 encoder with the PPA converting colour on the way in.

Measured at 1080p, on both silicon revisions:

| | MJPEG | H.264 |
|---|---|---|
| Frame rate, Waveshare ESP32-P4-ETH (rev v1.3) | 20 fps | ~7 fps |
| Frame rate, Function EV (rev v3.2) | 23 fps | 22-24 fps |
| Idle screen | 0 kbit/s (unchanged frames are dropped) | 170 kbit/s |
| Screen in motion | 8.5 Mbit/s | ~500 kbit/s |
| Chip temperature at full load | 46 &deg;C (rev v1.3) | 34 &deg;C (rev v3.2) |

Browsers decode H.264 through WebCodecs, and they only offer it on a secure page.
Over plain HTTP no browser can play it, however new - the console says so when it
cannot.

**The gap between those two rows is the silicon.** Below revision 3.0 the CSI
receiver cannot produce YUV420, and the H.264 encoder will not take RGB. So every
frame detours through the pixel accelerator to change colour space, and that
detour costs ~104 ms of the ~150 ms a 1080p frame takes.

On revision 3.x there is no detour: capture hands the encoder YUV422 and it takes
it as it is. H.264 went from ~7 to 22-24 fps at 1080p, and 28 at 720p - as fast
as MJPEG, at a fraction of the bandwidth, and cooler. On the older silicon the
conversion and the encode at least overlap now instead of running one after the
other. The measurements are in
[docs/HARDWARE-NOTES.md](docs/HARDWARE-NOTES.md).

**Input.** One composite USB HID device with four parts. A boot-protocol
keyboard, which firmware screens understand. An absolute pointer, so a click
lands where it was aimed whatever the target's mouse acceleration is doing. A
relative pointer, for software that captures the cursor. And the consumer keys.
Everything is released when the browser goes away, so a dropped connection cannot
leave a key held down on the target.

**Target OS.** How a machine enumerates a USB device is a fingerprint. Windows
asks for a Microsoft OS descriptor, macOS reads each string twice, Linux does
neither. So the console guesses whether the target is Windows, macOS, Linux or
Android, and shows it next to the USB status, with the raw trace behind a click.

The guess decides which key combinations the console offers - magic SysRq on
Linux, Task Manager on Windows - and whether the Meta key is labelled Win, Cmd or
Super. A `Target OS` setting overrides it when the guess is wrong, or when the
target never showed enough to tell. If it reads your machine wrong, send that
trace and a later build can learn it.

**Every feature is optional.** Each one reports whether it is compiled in,
whether the hardware supports it, and whether it is switched on. A control the
hardware cannot support is shown disabled, with the device's own explanation on
it - not hidden, and not left to fail silently. `GET /api/capabilities` is that
registry.

**Virtual media.** A disk image is presented to the target as a USB drive it can
boot from - a rescue system, an installer, a live image. The console lists what
is there and lets you pick which one the target sees. Images live in two places.

A **microSD card** holds the large ones: format it FAT32, up to 4 GB per file.
Below chip revision 3.0 the card is read-only - that SD controller reads
reliably, but its writes time out - so prepare it in an ordinary card reader. On
revision 3.x the card is writable and the console can upload to it.

The **device's own flash** holds one small image, in a 4 MB partition: enough for
iPXE, memtest or a DOS floppy, with no card at all. Flash writes work on every
board, so that one uploads from the browser, or over the cable with
`tools/fetch-rescue.sh` (netboot.xyz by default). It ships empty, and taking the
partition table that carries it is a one-time full flash - the browser flasher
does it - after which the image updates over the network.

**Reading the screen.** When the target is in a character mode - a BIOS setup, a
UEFI boot menu, memtest, a Linux console - the device reads the screen back as
text. The console lets you select it with the mouse, or copy the whole screen.

This is not OCR. A text screen is drawn by a character generator, so each cell is
looked up in the font's own shapes. It either matches exactly, or it comes back
as `�`. Never a guess, never a quiet blank, and no "recognised with errors" to
act on by mistake.

Three fonts are known: the 8x16 a legacy BIOS draws with, the slightly different
8x16 a Linux console draws with, and the 8x19 a UEFI console draws with. So a
720x400 setup screen, a 1024x768 boot menu and a Linux console all read. It costs
one pass over the frame, only in character modes, and only once the picture has
stopped moving. A firmware that paints its setup as a picture has no grid, and is
left alone.

The reading can also be left running with nobody connected. Give the device a
phrase to watch for and it keeps looking with the console closed - which is the
state a KVM is bought for, because the machine that falls over does it at three
in the morning. The alert is raised when the phrase appears and cleared when it
goes. Both edges reach the log, and Home Assistant if MQTT is on.

**Security.** The device serves HTTPS with a certificate it issues itself on
first boot, and asks for a password before it will do anything. The password is
stored as a salted PBKDF2 hash, sessions are HttpOnly cookies held in memory -
so a reboot signs everyone out - and repeated failures pay a growing delay.

A forgotten password is cleared with the board button. Reset the board, then
press and hold the button for two seconds, while the panel or the log asks you
to. Hold it *after* the reset, not through it: on boards where that button is
also the ROM's download strap, holding it through a reset drops the chip into
firmware-download mode instead.

Physical presence is the credential here, because whoever can hold that button
can also unplug the machine this device is attached to. The same gesture puts the
network back somewhere reachable - DHCP, the wired link, no operator certificate
- because a WiFi network that has vanished locks a device away just as a
forgotten password does.

**Heat.** The chip is watched, and if it ever gets hot the frame rate is halved
and then encoding stops - but the keyboard, the mouse and the web interface keep
running. A KVM that stops accepting keystrokes because it is warm has failed at
the job it was bought for, at exactly the moment someone is using it to fix
something. In practice it does not come up: 1080p at full rate settles
around 46 C in open air on rev v1.3 and 34 C on rev v3.2, against thresholds of
70 and 85 C.

**Updates.** Two app slots, with automatic rollback: an image that fails to come
up puts the device back on the one that worked. This also holds later, once the
new image has been accepted: four crashes in a row without one boot staying up,
and the device starts the other slot by itself. That matters here, because this
is often the only way to reach the machine it is attached to. The console can
check for a published build and install it in one click. The browser does the
fetching - the device never reaches the internet on its own.

## Interface

A Vue 3 console served from the device as a single gzipped file of about 70 KB,
with no external fonts, scripts or requests: the device has to work on a network
with no way out.

## API

Everything the console does is available over HTTP.

| | |
|---|---|
| `GET /api/capabilities` | what this device can do, and why it cannot do the rest |
| `GET /api/v1/settings`, `PUT` | settings, validated and applied as a whole |
| `GET /api/v1/settings/schema` | title, range and help text for every setting |
| `GET /api/v1/video/status` | resolution, frame rate, bitrate, encoder load, viewers, and whether this mode can be read as text |
| `GET /api/v1/screen/text` | the screen as characters when the target is in a text mode; 204 when it is showing a picture |
| `GET /api/v1/system/usbprobe` | the target's USB enumeration fingerprint and the OS guessed from it |
| `GET /api/v1/storage/images` | disk images on the card and in flash, and which one is active |
| `POST /api/v1/storage/upload`, `/rescue`, `/delete` | manage the virtual-media images |
| `POST /api/v1/power/wake` | send a Wake-on-LAN magic packet to the target's MAC |
| `POST /api/v1/power/click`, `/hold`, `/reset` | ATX: tap power, hold power for a hard off, tap reset |
| `GET /api/v1/video/frame.jpg` | one frame as a JPEG; 409 while H.264 is selected |
| `POST /api/v1/hid/key`, `/type`, `/move`, `/click` | the keyboard and pointer, for automation. Off until the agent API is enabled in Settings &rarr; Security |
| `GET /api/v1/system/info` | version, uptime, free memory, chip temperature, thermal state, Ethernet link, ATX power state |
| `GET /api/v1/system/log` | the device's own log, as a file |
| `POST /api/v1/system/update` | firmware image, written to the spare slot |
| `POST /api/v1/system/boot-slot` | boot the other slot on the next restart |
| `POST /api/v1/system/restart` | restart, for settings that need one |
| `POST /api/v1/settings/reset` | put every setting back to its default |
| `GET /api/v1/tls`, `PUT`/`DELETE /api/v1/tls/cert` | install your own certificate and key, or go back to the self-signed one |
| `POST`/`GET /api/v1/wifi/scan` | start a scan, then read what it found (boards with an ESP32-C6) |
| `GET /api/v1/pins` | the board's expansion header, and which GPIOs it leaves free |
| `GET /api/v1/auth/session` | whether a login is required, and who is signed in |
| `POST /api/v1/auth/login`, `/logout`, `/password` | the session, and changing the password |
| `GET /stream` | MJPEG as `multipart/x-mixed-replace`; answers 409 while H.264 is selected |
| `GET /cert.pem` | the device's CA certificate, to import and trust the device (also on port 80) |
| `WS /video` | video frames, JPEG or H.264, behind a 12-byte header |
| `WS /ws` | keyboard and pointer |

## Repository layout

```
components/
  tc358743/       HDMI bridge driver, EDID profiles
  video_pipeline/ CSI capture, MJPEG and H.264 codecs, published frame store
  kvm_hid/        composite USB HID
  kvm_storage/    microSD and on-flash rescue image, virtual media
  kvm_config/     settings registry and capability registry
  kvm_screentext/ reading a text-mode screen back as characters
  kvm_web/        HTTP/HTTPS server, REST API, WebSockets, TLS identity
  kvm_net/        Ethernet, WiFi (station/AP + rescue hotspot, captive portal),
                  IPv6, mDNS, Wake-on-LAN, and the VPN clients (WireGuard,
                  or Tailscale through third_party/microlink)
  kvm_atx/        power and reset buttons, power-LED sensing
  kvm_display/    the optional status screen (I2C OLED, round SPI LCD)
  kvm_log/        the log kept in RTC memory across a restart
  kvm_mqtt/       Home Assistant discovery and state
  kvm_board/      pin map
  esp_tinyusb/    vendored, patched for the mass-storage path
third_party/
  microlink/      submodule: the native Tailscale client
web/              the console (Vue 3 + TypeScript + Vite), a submodule
boards/           per-board build overlays - Function EV, NANO, Guition, PoE,
                  and the rev 3.x twins
tools/            toolchain setup, EDID generation, hardware probes
docs/             what the hardware actually does
```

## In the media

- [Hackaday](https://hackaday.com/2026/07/30/a-capable-kvm-built-with-the-esp32/) - *A Capable KVM Built With The ESP32*
- [CNX Software](https://www.cnx-software.com/2026/07/30/esp-kvm-an-open-source-ip-kvm-solution-based-on-esp32-p4-risc-v-mcu/) - *ESP-KVM - An open-source IP KVM solution based on ESP32-P4 RISC-V MCU*
- [Circuit Rocks](https://blog.circuit.rocks/esp-kvm-turns-an-esp32-p4-into-a-45-open-source-ip-kvm) - *ESP-KVM Turns an ESP32-P4 Into a $45 Open-Source IP KVM*
- [Open Source For You](https://www.opensourceforu.com/2026/07/microcontroller-enables-remote-device-access/) - *Microcontroller Enables Remote Device Access*
- [Solid State Bytes](https://ssbytes.org/p/a-raspberry-pi-that-boots-straight-into-ai-an-esp32-p4-kvm-and-more) - *A Raspberry Pi That Boots Straight Into AI, an ESP32-P4 KVM, and More*
- [EdigE](http://edige.xyz/news/esp32-p4-hdmi-ip-kvm) - *An ESP32-P4 turned into a real IP-KVM with HDMI, H.264, USB HID and boot images* (in Russian, and it goes into the capture path and the frame budget rather than only the feature list)

On video: **Wels** covered it in
[5 Minutos de Miercoles #22](https://youtu.be/nw-8a1GmJLE?t=342), from 5:42.
The original is in Spanish and YouTube carries dubbed audio tracks, English
among them - pick the language in the player.

Also picked up and translated internationally - French, Greek, Spanish, Russian,
Chinese, Japanese, Thai and German.

It has also started turning up as a reference point in reviews of other devices,
not only as a story of its own:
[PC de Mano](https://www.pcdemano.com/sc/internet/44120/) names it alongside the
Sipeed NanoKVM in a review of the USBridge-KVM 2.0 (in Spanish).

I also wrote up the project's origin story on
[Habr](https://habr.com/ru/articles/1069442/) - *ESP-KVM: how I built an IP-KVM
on the ESP32-P4, and what I tripped over along the way* (the article is in
Russian).

Waveshare, the maker of the capture adapter, links ESP-KVM from its
[HDMI to CSI adapter wiki](https://www.waveshare.com/wiki/HDMI_to_CSI_Adapter),
and from the documentation for its
[ESP32-P4-WIFI6-POE-ETH board](https://docs.waveshare.com/ESP32-P4-WIFI6-POE-ETH/Resources-And-Documents).

## Credits

This project exists because of **Jonathan Rowny** and his
**[p4kvm](https://github.com/jrowny/p4kvm)** proof of concept. He was the one
who got an ESP32-P4 to pull frames off a TC358743 at all, and put it out in
the open with a working [demonstration](https://youtu.be/f21f6RnW5Yc) for
anyone to build on. This is that thing built on. Start with his repository and
his video; everything here stands on them.

The history was not carried over into this repository, so the debt is recorded
here instead, and it is a real one. Two pieces of that work represent reverse
engineering that no datasheet would have handed us:

- **the TC358743 bring-up sequence** - PLL dividers, MIPI timing counters, the
  order in which hotplug and the CSI transmitter have to be brought up. One
  wrong register and the result is a black screen with nothing to debug;
- **direct programming of the ESP32-P4's `MIPI_CSI_BRIDGE` registers**, which
  the `esp_cam_ctlr` API does not expose. Espressif's examples target ordinary
  camera sensors, not an HDMI bridge.

Both still carry this firmware. The layers above them - HTTP, WebSockets, HID,
the interface - were rewritten, and the aim is different: p4kvm is explicit
about being a proof of concept, while this tries to be a KVM you would leave
installed. Thank you, Jonathan, for publishing it.

The register sequence also follows the Linux kernel driver
`drivers/media/i2c/tc358743.c` and Toshiba's TC358743XBG functional
specification.

The interface icons follow the Feather and Lucide conventions closely enough
that those sets deserve the credit; see [NOTICE](NOTICE).

A 3D-printed enclosure for the original parts is published by jrowny on
[MakerWorld](https://makerworld.com/en/models/2961981-esp32-p4-ip-kvm-enclosure).

## Support

ESP-KVM is free and open source. The easiest way to help is to
[star it on GitHub](https://github.com/espkvm/espkvm) - it costs nothing and helps
others find the project. If it saved you a trip to a dead machine and you want to say
thanks, you can [buy me a coffee](https://buymeacoffee.com/dexif) - entirely optional,
and contributions of code, issues and ideas are just as welcome.

Release notes and work in progress go out on
[Telegram](https://t.me/espkvm) and [X](https://x.com/espkvm).

## Licence

Apache-2.0, the same licence p4kvm and ESP-IDF use. See [LICENSE](LICENSE) for
the text and [NOTICE](NOTICE) for the attribution it requires.

One licence for the whole repository, deliberately: the files inherited from
p4kvm have to stay Apache-2.0 whatever the rest does, and a split would mean
every new file needs someone to remember which side it falls on. Apache also
carries an explicit patent grant from contributors, which MIT does not - worth
having on a hardware project, though it says nothing about third-party patents:
H.264 is encumbered no matter what licence sits on this code.
