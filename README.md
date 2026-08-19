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
| Pasting text with a keyboard layout | works |
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
| Guessing the target's OS from how it enumerates USB | works |
| Wake-on-LAN (magic packet to the target's MAC) | works |
| WiFi &mdash; station or its own access point, on boards with an ESP32-C6 | works; ESP32-P4 Function EV board only (the Waveshare board has no radio). One link at a time (Ethernet, WiFi, or AP). A rescue hotspot keeps a device reachable if its network is out of range, and a captive portal opens the console from a phone on connect |
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
TLS, but nothing here has been through a security review, and a device that
holds a keyboard on someone else's machine is worth more to an attacker than
most things on a network. Keep it on a network you trust, or reach it over a
VPN &mdash; it has both a built-in WireGuard client and native Tailscale
(Settings &rarr; VPN; enable one). Tailscale needs no port-forward, gateway or
VPS: the device joins your tailnet directly and is reachable at its 100.x
address (or MagicDNS name) from anywhere, with its TLS certificate valid over
the tailnet, and it relays through the DERP region nearest to it rather than a
fixed one. WireGuard is a split-tunnel client that generates its own key on the
device and shows the matching public key in the VPN tab to register on your hub.

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
the capture chip's I2C bus, needs no pins of its own, and is auto-detected.

</td>
<td valign="top">

**Round colour LCD (GC9A01)**

A 1.28&Prime; 240&times;240 round SPI LCD, e.g. the Waveshare module. The cheap
1.5&Prime; GC9A01A modules work too. Wire its SPI pins to any free GPIOs and pick
them in the console.

</td>
</tr>
</table>

Both are off by default &mdash; turn the display on in Settings and choose its type.

The LCD needs five free GPIOs, and which ones are free depends on the board - so
each build already offers the right ones for its board, and you only change them
if you wired it differently. What is offered:

| Board | SCLK | MOSI | CS | DC | RST |
|---|---|---|---|---|---|
| ESP32-P4 Function EV | 20 | 21 | 22 | 26 | 33 |
| Waveshare ESP32-P4-NANO | 22 | 5 | 32 | 33 | 36 |

On the Function EV board, **do not use GPIO 45 for DC** &mdash; it carries SD_PWRn,
so the panel gets no clean logic level and stays dark with nothing in the log.
The NANO pins come from [@DaveDavenport](https://github.com/DaveDavenport), who
tested them.

**Cables:** a 15-pin CSI ribbon between the two boards, HDMI from the target,
and USB-C from the board's OTG port to the target. A microSD card if you want
boot-from-image.

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

Then connect Ethernet, HDMI from the target, and the USB-C OTG port to the
target. The device announces itself over mDNS: open **https://espkvm.local/**.

The device is its own certificate authority - it generates a CA on first boot
and signs its own certificate with it - so the browser warns the first time.
Accepting the warning is enough to sign in and watch the MJPEG stream. For H.264
and for the keyboard and mouse, though, the browser needs a real *secure
context*, which a self-signed certificate does not give until it is trusted: the
WebSocket channel and the H.264 decoder (WebCodecs) will not run otherwise.

To trust the device once and clear the warning for good, download its CA -
**Settings -> Security -> Download CA certificate**, or `http://espkvm.local/cert.pem`
(served in the clear so you can fetch it before trusting anything) - import it
into your operating system or browser's **trusted authorities** (not "your
certificates"), and reach the device by name at **https://espkvm.local**. On a
static IP the certificate names the address too, so `https://<ip>` works after
importing; on DHCP use the name.

The device is dual-stack: on a network that advertises IPv6 it picks up an
address there as well, with nothing to configure, and `espkvm.local` resolves to
it. The certificate names those addresses from the next restart, so
`https://[address]/` is trusted like the v4 literal. Turn it off in
**Settings -> Network -> IPv6** if the KVM should stay off IPv6.

Tapping the **network icon** in the console footer shows the whole picture: what
the device is connected by, the name it answers to, its IPv4 address, every IPv6
address labelled with what it is good for, and the MAC for a DHCP reservation.
Each is a link and has a Copy button.

A network with no IPv4 at all works too. The one thing that cannot follow is
Wake-on-LAN: the magic packet is an IPv4 broadcast and IPv6 has no broadcast, so
the console reports it unavailable there rather than pretending. The WireGuard
and Tailscale tunnels still need a peer reachable over IPv4.

Sign in as **admin / admin**. The console will not go any further until that
password is changed: a KVM left on the password it shipped with is a keyboard
on someone else's machine, offered to whoever finds it.

After that the cable is only needed if something goes badly wrong - updates are
installed from the console itself.

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

Measured at 1080p on this hardware:

| | MJPEG | H.264 |
|---|---|---|
| Frame rate | 20 fps | 5-7 fps |
| Idle screen | 0 kbit/s (unchanged frames are dropped) | 170 kbit/s |
| Screen in motion | 8.5 Mbit/s | ~500 kbit/s |

H.264 is therefore not the faster option; it is the one that fits down a narrow
link. Browsers decode it through WebCodecs, which they expose on secure pages
only - over plain HTTP no browser can play it, however new. Turn HTTPS on and
it works; the console says exactly that when it cannot.

**Those are this board's numbers, not the ceiling.** The chip here is revision
v1.3, and below revision 3.0 the CSI receiver cannot deliver YUV420 while the
H.264 encoder refuses RGB - so every frame detours through the pixel
accelerator to change colour space, which is 76 ms of the 145 ms a 1080p frame
costs. Revision 3.0 and later removes that detour: the capture path can hand
the encoder what it wants directly. That is untested here for want of the
silicon, but it rests on a revision check in Espressif's own driver rather than
on hope - see [docs/HARDWARE-NOTES.md](docs/HARDWARE-NOTES.md). There is room
on our side too: the conversion and the encode run one after the other in the
same task where they could overlap.

**Input.** A composite USB HID device: a boot-protocol keyboard that firmware
screens understand, an absolute pointer so a click lands where it was aimed
regardless of the target's mouse acceleration, a relative pointer for software
that captures the cursor, and consumer keys. Everything is released when the
browser goes away, so a dropped connection cannot leave a key held down on the
target.

**Target OS.** How a machine enumerates a USB device is a fingerprint - Windows
asks for a Microsoft OS descriptor, macOS reads each string twice, Linux does
not - so the console guesses whether the target is Windows, macOS, Linux or
Android and shows it next to the USB status, with the raw request trace behind a
click. That guess is what the console follows when it offers OS-specific key
combinations (the Linux magic-SysRq sequences, Task Manager on Windows) and when
it labels the Meta key Win, Cmd or Super. A `Target OS` setting overrides it, for
when the guess is wrong or the target never showed enough to tell; if it reads a
machine wrong, that trace is what to send so a later build can learn it.

**Every feature is optional.** Each one reports whether it is compiled in,
whether the hardware supports it, and whether it is switched on. A control the
hardware cannot support is shown disabled, carrying the device's own
explanation, rather than hidden or left to fail silently. `GET
/api/capabilities` is that registry.

**Virtual media.** A disk image is presented to the target as a USB drive it can
boot from - a rescue system, an installer, a live image. The console lists what
is available and lets you pick which one the target sees, from two places at
once. Images on a **microSD card** are served read-only: this board reads the
card reliably but cannot write it at a useful speed, so the card is prepared in
an ordinary reader (format it FAT32, up to 4 GB per file, a FAT32 limit). A small
image can also live in the **device's own flash** - a 4 MB partition, enough for
iPXE, memtest or a DOS floppy, and no card needed. Flash writes are reliable
here, so that one can be uploaded from the browser (or written over the cable
with `tools/fetch-rescue.sh`, which fetches netboot.xyz by default). The flash
partition ships empty; adopting the partition table that carries it is a one-time
full flash (the browser flasher does it), after which the image updates over the
network.

**Security.** The device serves HTTPS with a certificate it issues itself on
first boot, and asks for a password before it will do anything. The password is
stored as a salted PBKDF2 hash, sessions are HttpOnly cookies held in memory -
so a reboot signs everyone out - and repeated failures pay a growing delay.

A forgotten password is cleared with the board button: reset the board, then
press and hold the button for two seconds while the panel (or the log) asks you
to. Hold it *after* the reset, not through it - on boards where that button is
also the ROM's download strap, holding it through a reset drops the chip into
firmware-download mode instead. Physical presence is the credential, because
whoever can hold that button can also unplug the machine this device is attached
to. The same gesture also puts the network back somewhere reachable - DHCP, the
wired link, and no operator TLS certificate - since a WiFi network that has
vanished locks a device away exactly as a forgotten password does.

**Heat.** The chip is watched, and if it ever gets hot the frame rate is halved
and then encoding stops - but the keyboard, the mouse and the web interface keep
running. A KVM that stops accepting keystrokes because it is warm has failed at
the job it was bought for, at exactly the moment someone is using it to fix
something. On this board it does not come up in practice: 1080p at full rate
settles around 46 C in open air, against thresholds of 70 and 85.

**Updates.** Two app slots and automatic rollback: an image that fails to come
up returns the device to the one that worked - which matters on a device that
is often the only way to reach the machine it is attached to. The console can
check for a published build and install it in one click; the browser does the
fetching, and the device never reaches out to the internet on its own.

## Interface

A Vue 3 console served from the device as a single gzipped file of about 50 KB,
with no external fonts, scripts or requests: the device has to work on a network
with no way out.

## API

Everything the console does is available over HTTP.

| | |
|---|---|
| `GET /api/capabilities` | what this device can do, and why it cannot do the rest |
| `GET /api/v1/settings`, `PUT` | settings, validated and applied as a whole |
| `GET /api/v1/settings/schema` | title, range and help text for every setting |
| `GET /api/v1/video/status` | resolution, frame rate, bitrate, encoder load, viewers |
| `GET /api/v1/system/usbprobe` | the target's USB enumeration fingerprint and the OS guessed from it |
| `GET /api/v1/storage/images` | disk images on the card and in flash, and which one is active |
| `POST /api/v1/storage/upload`, `/rescue`, `/delete` | manage the virtual-media images |
| `POST /api/v1/power/wake` | send a Wake-on-LAN magic packet to the target's MAC |
| `POST /api/v1/power/click`, `/hold`, `/reset` | ATX: tap power, hold power for a hard off, tap reset |
| `GET /api/v1/system/info` | version, uptime, free memory, chip temperature, thermal state, Ethernet link, ATX power state |
| `POST /api/v1/system/update` | firmware image, written to the spare slot |
| `POST /api/v1/system/restart` | restart, for settings that need one |
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
  kvm_web/        HTTP/HTTPS server, REST API, WebSockets, TLS identity
  kvm_net/        Ethernet, WiFi (station/AP + rescue hotspot, captive portal),
                  IPv6, mDNS, Wake-on-LAN, WireGuard tunnel
  kvm_board/      pin map
web/              the console (Vue 3 + TypeScript + Vite)
boards/           per-board build overlays (Waveshare, Function EV)
tools/            toolchain setup, EDID generation, hardware probes
docs/             what the hardware actually does
```

## In the media

- [Hackaday](https://hackaday.com/2026/07/30/a-capable-kvm-built-with-the-esp32/) - *A Capable KVM Built With The ESP32*
- [CNX Software](https://www.cnx-software.com/2026/07/30/esp-kvm-an-open-source-ip-kvm-solution-based-on-esp32-p4-risc-v-mcu/) - *ESP-KVM - An open-source IP KVM solution based on ESP32-P4 RISC-V MCU*
- [Circuit Rocks](https://blog.circuit.rocks/esp-kvm-turns-an-esp32-p4-into-a-45-open-source-ip-kvm) - *ESP-KVM Turns an ESP32-P4 Into a $45 Open-Source IP KVM*
- [Open Source For You](https://www.opensourceforu.com/2026/07/microcontroller-enables-remote-device-access/) - *Microcontroller Enables Remote Device Access*
- [Solid State Bytes](https://ssbytes.org/p/a-raspberry-pi-that-boots-straight-into-ai-an-esp32-p4-kvm-and-more) - *A Raspberry Pi That Boots Straight Into AI, an ESP32-P4 KVM, and More*

On video: **Wels** covered it in
[5 Minutos de Miercoles #22](https://youtu.be/nw-8a1GmJLE?t=342), from 5:42.
The original is in Spanish and YouTube carries dubbed audio tracks, English
among them - pick the language in the player.

Also picked up and translated internationally - French, Greek, Spanish, Russian,
Chinese, Japanese, Thai and German.

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

## Licence

Apache-2.0, the same licence p4kvm and ESP-IDF use. See [LICENSE](LICENSE) for
the text and [NOTICE](NOTICE) for the attribution it requires.

One licence for the whole repository, deliberately: the files inherited from
p4kvm have to stay Apache-2.0 whatever the rest does, and a split would mean
every new file needs someone to remember which side it falls on. Apache also
carries an explicit patent grant from contributors, which MIT does not - worth
having on a hardware project, though it says nothing about third-party patents:
H.264 is encumbered no matter what licence sits on this code.
