# Changelog

All notable changes to ESP-KVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning while it is pre-1.0 (a new feature bumps the minor, a fix
bumps the patch).

## [Unreleased]

### Added
- **The console says when H.264 is slow on this board.** On the M5Stack Unit
  PoE-P4, H.264 above 720p runs at 5-8 fps: the bridge writes every frame it
  receives into PSRAM, 60 a second at 1080p60, and the byte shuffle the
  encoder needs there gets what is left. A note offers MJPEG, and suggests
  720p (17 fps) or 30 Hz (8 fps instead of 5-6 at 1080p) in the target's
  display settings. `h264Cpu` in `GET /api/v1/video/status` says a board
  works this way.
- `psramLargest` in `GET /api/v1/system/info`: the biggest free piece of
  PSRAM, next to `psramFree`.

### Fixed
- **Uploads failed with "out of memory" after a few hours.** An upload takes
  four 256 KB buffers in PSRAM, and after a while the free PSRAM is there but
  in smaller pieces. Seen on the M5Stack Unit PoE-P4: one upload went
  through, every one after it failed. Now it takes smaller buffers, or fewer,
  instead of giving up.
- **The EDID profile setting is hidden where it does nothing.** The LT6911D
  on M5Stack's add-on holds its own EDID, so the setting had no effect on
  the Unit PoE-P4. A new `edid` capability says whether the bridge takes one.

## [0.61.0] - 2026-10-08

### Added
- **The M5Stack Unit OLED (SH1107).** "SH1107 128x64" in Settings ->
  Display. The SH1107 sees this panel on its side, so the picture is turned
  as it is sent. Checked on a Unit OLED plugged into the Grove port of the
  M5Stack Unit PoE-P4.
- **A button on the box.** Settings -> Power -> Button on the box: a push
  button on a free pin (the M5Stack Unit Button on a Grove port, or any switch
  to ground), with an action for a short press and one for a 1.5 s hold -
  power, a hard off, reset, Wake-on-LAN, a runbook, save a dashcam clip, or a
  screenshot. The same actions a schedule runs, and a schedule can now save a
  clip or take a screenshot too. Not tried with real hardware yet.
- **The M5Stack Unit Glass2.** "SSD1309 128x64" in Settings -> Display, for
  the 1.51" transparent OLED. Not tried on one yet.
- **The M5Stack Unit Relay, documented.** One relay presses one button: wire
  it as the power (or reset) button GPIO with "Buttons active-high" on. No new
  code; not tried with the unit yet.
- **M5Stack Unit PoE-P4: the add-on's two spare lines, used.** M5Stack's
  schematic of the Add-on Display In shows the LT6911D pulsing G38 on every
  resolution change, and the microSD socket's card-detect switch on G37. The
  picture now follows a mode change the moment it happens rather than at the
  next poll, and a card going in or out is noticed at once rather than within
  five seconds. (Card detect there reads high with a card in - the opposite of
  most - which a first try got wrong and read every card as gone.)
- **A pin probe, for diagnosis.** `GET /api/v1/system/pinprobe?pins=19,20`
  counts how fast each pin switches (and `&test=<pin>` puts a known 48 kHz
  signal on a free pin to prove it counts). It settled that the M5Stack
  Add-on Display In brings no HDMI audio to the board: every header pin it
  sits on is the microSD, USB or the UART, and all read 0 Hz with sound
  playing into the HDMI input.
- **A frame counter in the video status.** `GET /api/v1/video/status` now
  has `frames` (captured since boot) and `frameAgeMs` (since the last one).
  "signal" only says the HDMI is locked; these say pictures are really
  arriving, even on a still screen that nobody is encoding. The idea came
  from a fork that tests games on a real Xbox.
- **A tick under the finger on a phone.** The on-screen keyboard, the arrow
  keys and the gamepad buttons give a short vibration on each press, like a
  phone's own keyboard, and the touchpad gives a faint tick every few
  millimetres the finger travels, the way the Steam Controller's pads do.
  Android only - Safari on an iPhone cannot vibrate.
- **Full screen is all picture.** In full screen the status strip, the rail
  and the bottom bar slide away. They come back over the picture - it does
  not resize - with the mouse at an edge of the screen (after a short pause
  while you have control, so the target's own taskbar stays usable), the
  small tab at the top for a finger, or a tap of the right Ctrl key on its
  own; and go again a few seconds after you leave them. Settings -> UI ->
  "Hide the bars in full screen" turns it off.
- **A UI tab in Settings.** For how the console looks and feels: "Panel
  side" moved here from System; "Shown in the console" is a tick per button
  (the gamepad, the TV remote, recording, screen text, the serial console
  and the rest) to take the ones you never use out of the bar - it hides the
  button only, the feature stays as its own settings have it; and "Vibration
  on a phone" has three settings - on key presses, on the touchpad, and the
  strength (a browser can only make a tick longer, not stronger, so that is
  12, 25 or 45 ms).

### Fixed
- **The M5Stack Unit RTC was not found.** Its BM8563 reads one bit of a
  register differently from the NXP PCF8563 it copies, and a new one holds no
  valid time - either was enough for "Auto" to pass it over. Both are
  accepted now, and "Auto" also looks on the status display's own bus, so a
  clock plugged in beside the display (the Grove port of the Unit PoE-P4) is
  found with nothing to set. Checked on the board.
- **Settings opened empty.** The window showed only the search field until a
  tab was clicked: it chose the first tab by the whole section instead of its
  name, which matched no setting. Broken since settings moved into a window.
- **Full screen on a phone.** In Chrome on a Pixel the bottom bar slid off
  the bottom of the screen in full screen, and the page cannot be scrolled
  to reach it. In full screen the console is now sized to what is actually
  visible.
- **The touchpad in the demo.** On a phone the demo ignored the touchpad: it
  took no mouse reports at all, only the keyboard. Its pointer now follows
  them, and a tap clicks.
- The read-only card note in Media reads as a sentence and no longer tells
  you to reformat the card.

## [0.60.0] - 2026-10-06

### Added
- **The target's serial console.** For a box with no screen at all - a NAS,
  a router, a headless server - or a BIOS that talks over serial. Turn it on
  in Settings &rarr; Power &rarr; Serial console and pick two pins: a 3.3 V
  console wires straight in, a real RS-232 port goes through a MAX3232
  module. The console gets a terminal button: a VT100 screen with colours,
  keys sent as a terminal sends them, and everything since boot replayed when
  you open it (the device keeps the last 64 KB). Scripts can read
  `GET /api/v1/serial/log` and type with `POST /api/v1/serial/send`. Asked
  for in #69. With no HDMI signal, the "No signal" screen offers a button
  that opens it.
- **The target's log over the network.** Turn on Settings &rarr; Network
  &rarr; Netconsole and point the target's netconsole (or any plain syslog
  over UDP) at the device: `modprobe netconsole
  netconsole=@/,6666@<device IP>/`. It keeps the last 64 KB, shows it live
  behind a new button, and sends a notification when a line says "Kernel
  panic", "Oops" or another phrase you pick - at most one every 30 s. No
  wiring, and it still works when the target's disk is gone. The lines are
  unauthenticated UDP, so they only ever notify, never run anything; set
  "Accept from" to the target's address. An idea from Reddit.
- **The device's own log, live.** Diagnostics &rarr; Live log shows it as it
  is written, with errors and warnings in colour, levels you can hide and a
  search. The device now keeps 64 KB of it, five times as much as before; the
  part that survives a restart is the same 12 KB as ever.
- **Download an image from a link.** The Media panel puts netboot.xyz into
  the rescue slot with one click, and downloads any http/https link onto the
  card. The device fetches it itself, so nothing goes through the browser.
  Promised in #70.

### Fixed
- **HTTPS downloads dying part-way.** With video running, the hardware AES
  could not get internal RAM for a record and the connection broke ("invalid
  MAC"). Downloads from the device - the link download above and installing
  a release from GitHub - now ask for ChaCha20 first, and only fall back to
  the usual ciphers when the server has no ChaCha20.

## [0.59.0] - 2026-10-04

### Added
- **A gamepad.** Settings &rarr; Input &rarr; Gamepad makes the
  device a game controller as well: a HORI Pokken pad for a Nintendo Switch,
  or a wired Xbox 360 pad (XInput) for Windows, a Steam Deck and Linux. The
  console gets a gamepad panel, or an overlay drawn over the picture like a
  phone emulator, with button names in Nintendo, Xbox or PlayStation style. A
  controller plugged into your own computer drives it too, and so does
  `POST /api/v1/hid/pad`. Tried on a Switch with "switch_alone": the pad
  plays Minecraft. The Xbox pad is not tried on hardware yet.
- **Ethernet with WiFi as the backup.** A new choice under Connection:
  "Auto". The device uses the cable and keeps the WiFi network joined and
  waiting. Pull the cable and traffic moves to WiFi; plug it back and it
  returns, with no restart. The console answers on both addresses and the
  certificate names both. Only on boards with a network port and a WiFi chip.
  Tried on the Function EV: the switch took about a second each way, the
  console stayed open over WiFi, Tailscale stayed up and MQTT came back in
  15 s. IPv6 stays on the cable.
- **EDID profile "480p".** Offers 720x480 and nothing bigger, for old
  consoles and TV boxes. Settings &rarr; Video.

### Changed
- **A narrow screen fits the bottom bar again.** Below 640 px the less used
  buttons fold into a "..." menu; the menu glows red while a recording runs.
- **Floating windows stay on the screen.** The remote, the gamepad and the
  floating keyboard come back inside when the browser window shrinks.
- **The version badge stays on one line.** On a phone it shows the release
  part only; the full name is in its tooltip.

### Fixed
- **No picture at 720x480 on rev 3 boards with a TC358743.**
  The chip's line buffer was set to start sending a line later than the line
  is long at this width (640x480 too, by the same sum), so every frame came
  out a little short and capture never finished one. The level now follows
  the line width; wider modes keep the old value. Found by Zach in his fork
  with an original Xbox; checked here with a Steam Deck at 720x480: 30 fps,
  and 720p and 1080p30 unchanged.

## [0.58.0] - 2026-10-03

### Added
- **HDMI-CEC.** On boards with a TC358743 capture chip the device now talks
  over the CEC line of the HDMI cable. It acts as a TV, finds what is
  connected and shows its name, vendor and power state. From the console's
  new remote panel you can send remote-control keys, put the source to
  standby or wake it. Also in the API (`/api/v1/cec`), in runbooks
  (`hdmi standby`, `hdmi wake`, `hdmi key <name>`) and in Home Assistant.
  Only media boxes, consoles and the like speak CEC; an ordinary PC does
  not. What the source says about itself is under the HDMI icon at the
  bottom, and the remote button shows up only when a source answers. Tried
  with a Steam Deck in its dock: the name, the remote keys in the Steam menus
  and standby work; wake does not, SteamOS does not wake on CEC. Off with one
  switch in Settings &rarr; Power.
- **Pointer lock in relative mode.** While you are in control, the console
  hides your own cursor and sends only movements, so the target's cursor is the
  only one on the screen. Esc gives it back. This is what SteamOS in game mode
  needs: an absolute pointer does not move there.

### Changed
- The buttons under the picture are icons now: touch, fit / stretch / 1:1,
  select text, copy text.

### Fixed
- **Clicks in relative mode moved the cursor first.** They went out as
  absolute reports, so the target's cursor jumped to the browser's spot before
  every click.
- **A Steam Deck was shown as Android.** Steam re-reads the USB strings the
  way Android does. Android also re-reads the device's own name strings, and
  that is what the guess looks for now.

## [0.57.4] - 2026-10-03

### Fixed
- **Locked out after the first restart on boards with no network port.** On
  the ESP32-P4-WIFI6 and the FireBeetle 2, a password set over the open setup
  hotspot was followed, at the next boot, by a hotspot with a made-up password
  only the serial log showed. Now the first password comes with a choice:
  join your WiFi, keep the hotspot with a password you pick, or (on boards
  with a port) use the cable. The device restarts into it, which also closes
  the open hotspot at once.

## [0.57.3] - 2026-10-03

### Fixed
- **The first hotspot asked for a password on boards with no network port**
  (the ESP32-P4-WIFI6 and the FireBeetle 2). They start in hotspot mode, and
  that path made up a WPA2 password that only the serial log of the very first
  boot showed. Now a device with no console password set opens the same open
  setup hotspot as a board with Ethernet does, as the README always said.
  Reported in #67.

## [0.57.2] - 2026-10-03

### Fixed
- **Devices on 0.56 and older did not see new releases.** Their update address,
  espkvm.github.io/espkvm, started answering with a redirect to fw.espkvm.io,
  which the browser refused (no CORS on the redirect) and the device did not
  follow. That address answers directly again, and fw.espkvm.io is now served
  from its own repository. The device's own update check also follows
  redirects now.
- **The device's update check failed behind Cloudflare.** Cloudflare's
  certificate chain ends in a root that is cross-signed by one the device does
  not carry. Such chains are accepted now.
- **A boot loop on Wi-Fi boards.** When the Wi-Fi co-processor did not answer
  at start-up, the network stack was never started, and the web server then
  crashed the device on every boot. It now starts without the network instead.

## [0.57.1] - 2026-10-02

### Fixed
- **H.264 over Wi-Fi flashed "Cannot play this stream" every few seconds.**
  When a viewer's socket was not ready, the device skipped a frame for it, and
  the browser's decoder broke on the next one. The device now holds that
  viewer's frames until a keyframe and asks for one at once. The console also
  asks for a keyframe after a decoder error and shows the message only when
  errors keep coming.

## [0.57.0] - 2026-10-02

### Added
- **A battery-backed clock, if you fit one.** A DS3231 or DS3231M module on
  the capture board's I2C bus (on the Function EV and the Waveshare boards,
  pins 1, 3, 5 and 9 of the 40-pin header - the "DS3231 for Pi" module plugs
  straight on) is found at start-up and sets the clock, so the device knows the
  time after a restart with no network. Once the device learns the time
  elsewhere (NTP, or a browser signing in), it writes it to the chip.
  Diagnostics says when one is there, and shows its thermometer as the
  temperature by the board - also a "Board temperature" sensor in Home
  Assistant. Anything else answering at 0x68 (a DS1307, a PCF8523, a motion
  sensor) is recognised as not a DS3231 and left alone. Settings -> System ->
  Clock can name the chip - a PCF8563 / BM8563, PCF85063 or PCF8523 as well,
  written from their datasheets and not yet tried on hardware - or turn it
  off, and can put it on pins of its own (a second I2C bus, shared with a
  status OLED on the same pins).
- **Two-factor sign-in.** In Settings -> Security: after the password, a
  six-digit code from an authenticator app, set up by scanning a QR code the
  device draws, with eight one-time recovery codes for a lost phone. The code
  is checked against the device's clock, or against the browser's when the
  device has none yet, and each code works once, even across a restart. The
  board's reset button clears it along with the password.
- **The console asks once whether to check for updates.** Update checks are
  off by default, so a freshly flashed device never heard of a fix unless
  someone went looking for the switch. A banner now offers to turn them on, and
  "Not now" is remembered in that browser.
- **Links to where things are explained**: the version in the update panel
  opens its release notes, Diagnostics has "Tell me what you think" (the
  feedback thread), "Report a bug" and "If it does not work", the
  notifications panel links its write-up, and the ATX settings link the
  wiring.
- **Alerts wait for the network.** A notification that could not go out - the
  network down, a timeout, Telegram busy - used to be lost. Now it waits and is
  sent when the device can reach the server again, oldest first, with the time
  it really happened added; the screenshot and the log tail are the ones from
  that moment. Up to 50 wait. With a writable microSD card they are kept there,
  so they survive a restart and the screenshots stay out of memory; without
  one they wait in memory, within 2 MB and never below 3 MB of free PSRAM. It
  came out of a Reddit thread about the dashcam and a "the server stopped
  answering" trigger.

### Changed
- **Updates now come from fw.espkvm.io.** The old espkvm.github.io/espkvm
  address redirects there, so older firmware keeps updating. A device that
  still has the old default address in its settings switches to the new one
  by itself on the next boot.
- **The DFRobot FireBeetle 2 ESP32-P4 is confirmed on hardware** by
  @Diego-fe in #63: capture, USB and Wi-Fi, about 9 fps of 1080p MJPEG over
  Wi-Fi on 0.56.2. The flasher no longer marks it untested.

### Fixed
- **A console left open through a restart now asks you to sign in.** The
  restart signs everyone out, and if the device was still starting when the
  console noticed, the console gave up asking: no sign-in form, and buttons
  that did nothing. Now any "signed out" answer brings the form back.
- **Tailscale: steadier links.** The microlink client no longer drops
  WireGuard packets silently, waits for a peer's direct address to be
  confirmed before using it (DERP until then), frees a failed DERP connection's
  TLS memory, and takes lwIP's lock for its own sends. Thanks to the upstream
  microlink contributors.
- **Wi-Fi on four boards not yet run on hardware**: the Waveshare
  ESP32-P4-Module-DEV-KIT, ESP32-P4-WIFI6-DEV-KIT and ESP32-P4-NANO-WIFI6-DB,
  and the VIEWE ESP32-P4-Pi. Their schematics show the same weak 51k pull-ups
  on the SDIO lines as the ESP32-P4-WIFI6, where the link stalled until the
  chip's own pull-ups were added; they are on for these boards now. GPIO 6, the
  co-processor's wake line, is no longer offered as a free pin on them or on the
  Function EV boards. Found by checking every untested board against its
  schematic.
- **Board notes that would have sent people wrong.** The ESP32-P4-WIFI6-DB's
  camera connector is the 22-pin 0.5 mm kind, not 15-pin, so a C790 needs a
  15-to-22-pin ribbon. The NANO-WIFI6-DB's Type-A always carries 5 V, so the
  lead to the target is an A-to-A cable with the 5 V wire cut, as on the NANO.
- **The M5Stack Unit PoE-P4X image no longer claims a product that exists.**
  M5Stack list only the Unit PoE-P4, with a rev 1.x chip. The rev 3.x image
  stays, for a unit with the newer chip should one appear, and says it has
  never run and that its colours on that chip are untested.
- **Building from a release archive.** GitHub's source archives leave out the
  console and microlink, and with them fetched by hand the build still stopped
  at `wireguard_lwip`, which microlink reaches through a symlink. The build now
  names its real path, and each release carries
  `espkvm-<version>-source.tar.gz` with both submodules in it. Reported in
  #63, which also answers the open question in #27.

## [0.56.2] - 2026-10-01

### Fixed
- **Wi-Fi dropping the page and the video.** Over Wi-Fi the P4 ran out of
  internal RAM: esp-hosted took a buffer for every packet from it, and lwIP
  kept every sent segment there until the browser acked it. Acks come slower
  over Wi-Fi, so loading the console alone took internal RAM from 85 KB to
  1 KB, and TLS stopped opening connections. Both now use PSRAM. This is most
  likely what #63 ran into. Every board with a Wi-Fi co-processor.
- **Wi-Fi power saving is off.** The C6 woke its radio only every third
  beacon, about 300 ms, which the keyboard and the video both felt.

### Added
- Code to install a newer esp-hosted into the Wi-Fi chip from the console,
  switched off in every build for now. The update itself works on the
  Function EV, but esp-hosted 3.0.9's faster SDIO mode then stalled under
  video, and the old firmware runs clean since the fixes above.

## [0.56.1] - 2026-09-30

### Fixed
- **Wi-Fi on the DFRobot FireBeetle 2 ESP32-P4.** Its schematic has no pull-up
  resistors on the SDIO lines to the ESP32-C6, which esp-hosted needs; the
  firmware now adds the chip's own, as it already did for the Waveshare
  ESP32-P4-WIFI6. GPIO 6, the C6's wake line, is no longer offered as a free
  pin. Found through #63, the first report from this board: capture and USB
  work on it.

## [0.56.0] - 2026-09-29

### Added
- Two new boards, built from their makers' documentation and not yet run on
  hardware: the **Espressif ESP32-P4X-C5-Function-EV-Board** (`funcev-c5`) and
  the **Waveshare ESP32-P4-WIFI6-DB** (`p4-wifi6-db`). Both are boards already
  supported with a dual-band ESP32-C5 in place of the C6, so they can join a
  5 GHz network. The WIFI6-DB has no Ethernet, like the WIFI6.
- A pre-3.0 image for the Espressif ESP32-P4 Function EV Board
  (`funcev-rev1`). The funcev image is rev 3.x, because the board it was
  brought up on carries a rev 3.2 chip, but earlier units of the same board
  carry rev 1.x. The flasher now asks which one you have. This image sends the
  log to both the USB-to-UART port (board v1.4) and the USB Serial/JTAG one
  (v1.5). Not tried on hardware yet.
- A rev 3.x image for the Waveshare ESP32-P4-Module-DEV-KIT
  (`p4-module-devkit-rev3`). Waveshare's shop now lists the ESP32-P4-Module
  with an ESP32-P4NRW32X, and the older image does not start on that chip.

### Fixed
- "Install any release" on the NANO-WIFI6-DB asked for a file named
  `...-p4-nano-wifi6-db-rev3.bin`, which releases do not have. The board's id no
  longer carries the suffix.

### Changed
- **The Waveshare ESP32-P4-WIFI6-DEV-KIT is confirmed on hardware** by
  @brooklyn5w4g: a v1.2 board with a rev 3.1 chip, H.264 at about 23 fps. The
  docs said to set its USB jumper to DEVICE; that is right only on v1.1. The
  jumper picks USB-A port 1 or the hub, and Waveshare swapped its labels on
  v1.2, so there the target goes on port 1 with the jumper on HOST.

## [0.55.1] - 2026-09-28

### Fixed
- The Waveshare ESP32-P4-WIFI6-DEV-KIT has 16 MB of flash, not 32. Its builds
  used the 32 MB partition table, where the last partition ends past 16 MB.
  Thanks to @brooklyn5w4g, who fixed the rev3 build. An update over the network
  keeps the old table; flash once over USB (the web flasher) to get the new one.

## [0.55.0] - 2026-09-28

### Added
- **The virtual CD-ROM answers like a real optical drive.** An `.iso` was
  served with the CD-ROM device type and 2048-byte blocks, but every command
  only an optical drive has - the table of contents, the drive's profile, "a
  disc was inserted" - was refused. UEFI and Linux booted anyway; Windows,
  macOS and some firmware ask those first. Now the drive answers READ TOC,
  GET CONFIGURATION, GET EVENT STATUS NOTIFICATION, READ DISC INFORMATION,
  READ TRACK INFORMATION and MODE SENSE(10). An image above 900 MB presents
  itself as a DVD-ROM, a smaller one as a CD-ROM. Checked on a Linux target,
  which now sees a DVD drive with a disc in it. Not yet tried on Windows or on
  a BIOS boot.
- **Images over 4 GB on an exFAT card.** A full DVD installer is 5-8 GB. The
  device reads an upload's length as 32 bits, so the console now sends a big
  file in 2 GB parts and the device appends each one
  (`storage/upload?name=&offset=`). A part that does not line up with the file
  on the card is refused. The image list showed such a file with the wrong
  size, and the disc's end in minutes (MSF) wrapped past 255 minutes (about
  2.2 GB); both fixed. A 4.5 GB image went up in three parts and read back on
  the target with the same md5.

### Changed
- The Guition M3-Dev's capture ribbon goes into J3, not J2; the README and the
  site say so now (#61).

### Fixed
- **Recordings over 2 GB.** A recording can grow to 3.9 GB, but past 2 GB the
  recordings panel showed a negative size, and seeking in the player or a
  download with a range did not work. The file system's seek counted in a
  signed 32-bit number, so past 2 GB it went negative. The device now reads
  files and sizes in 64 bits.

## [0.54.3] - 2026-09-27

### Fixed
- **"No driver here knows it" no longer blames the firmware for a loose
  ribbon (#61).** With the C790 not answering, the Guition M3-Dev's own audio
  codec at 0x18 was the only chip on the capture bus, and the message sent
  people looking for a missing driver. The chips a board carries itself - audio
  codecs, touch controllers, an OLED - are now named as such, and the message
  says to check the ribbon.

## [0.54.2] - 2026-09-25

### Fixed
- **M5Stack Unit PoE-P4: the target no longer sees its monitor replugged
  every few minutes.** 0.54.1 reset the capture chip on the first missed
  frame. A single missed frame happens there several times an hour, and a
  rebuild of the camera receiver alone cures it - the reset only made the
  target's screen blink. The chip is now reset only when no frames have come
  for about 30 seconds.
- **M5Stack Unit PoE-P4: a locked PC can go to sleep again.** When its screen
  turned off, the capture chip lost the picture, and 10 seconds later the
  device reset the chip - which the PC saw as a monitor plugged in, so the
  screen woke up. This chip cannot tell a sleeping screen from a stuck one, so
  the device no longer resets it on its own when the picture goes.

### Added
- **"Reconnect HDMI" under "No signal".** It offers the source a fresh start:
  a hotplug cycle on a TC358743, a reset of the chip on the M5Stack. On the
  M5Stack it also counts down 30 seconds and presses itself, unless you say
  no - so a stuck chip still comes back when somebody is looking. The note
  under "No signal" there no longer claims the machine is off: that chip
  cannot see the source's power. `POST /api/v1/video/reconnect`.

## [0.54.1] - 2026-09-25

### Fixed
- **M5Stack Unit PoE-P4: the picture comes back when the capture chip keeps a
  mode but stops sending frames.** After about 17 hours the LT6911D still
  reported 1080p, but no frame arrived, and the recovery every 8 s never
  touched the chip, which has no hotplug line to cycle. Recovery now pulses
  its reset pin: on tries 1, 2, 4 and so on, then every 64th, because each
  reset looks like an unplugged monitor to the target. Boards with a
  TC358743 are unchanged.

## [0.54.0] - 2026-09-24

### Fixed
- **The picture comes back after a codec falls over, on the boards that reorder
  the captured bytes.** MJPEG needs a 4 MB buffer for that pass and took it on
  its first frame, a race it lost while H.264 held the PSRAM: the codec opened
  and produced nothing. MJPEG now takes it when it opens, before anything else.
- **Updates stick with H.264 and the dashcam running.** Before a restart the
  device waited 20 ms for the encoder, but a 1080p H.264 frame takes 43 ms or
  more, so the restart cut its memory writes in half, the new image hung, and
  the update rolled back. Now no new frame starts and the one in hand is waited
  for. funcev: 0 of 3 updates went through before, 6 of 6 after; M5Stack 2 of 4,
  then 6 of 6.
- **The dashcam gets its memory with H.264.** MJPEG keeping its buffers left
  the dashcam no room for its ring. It now asks for them, and sizes the ring to
  the largest free piece rather than the free total.
- **P4-ETH: switching to H.264 at 1080p no longer ends with no picture.** 0.53.0
  did this too. H.264 does not fit there next to the capture buffers, and giving
  MJPEG's buffers away lost them for good. Now MJPEG keeps them, and H.264 is
  marked unavailable until a restart, with the reason.
- **A refused codec switch no longer restarts the device.** It was retried twice
  a second until the task watchdog fired; now once every 10 s.
- **M5Stack: screenshots work while H.264 runs.** The byte-reordering buffer
  now lives in the codec region for good, so a screenshot no longer has to
  find 4 MB free at the moment it is taken.
- **Changing codec can no longer leave the device with no picture at all.** Each
  codec wants several megabytes of PSRAM in a few large pieces, and after the
  old one closed and the new one failed to open, the heap was not always the
  shape it had been: both refused, the status read "codec: none", and only a
  restart brought the picture back. Now MJPEG keeps its output buffers across a
  close, so it can always come back; H.264 takes those buffers back if it is
  short, and if everything fails the codec that was running is started again.
  The log says how much PSRAM was free and how big its largest piece was.
  On top of that, the codecs now share one PSRAM region taken at boot, sized
  for the bigger of the two, and never give it back to the heap. So a switch
  no longer depends on how the heap looks after hours of work. While H.264
  runs, the dashcam borrows the part of the region it does not use. M5Stack at
  1080p: 8 of 8 switches, where before the way back to MJPEG could fail.
  Handing that part back on a switch no longer counts as a codec short of
  memory, which kept the dashcam off for 30 s after it.
- **The console was pulling the picture twice.** The element that carries the
  multipart stream is hidden while the WebSocket has the picture, and hidden is
  not gone: with its address still set, the browser kept downloading it. So
  every frame left the device twice, on two connections, for as long as a
  console was open - which on a board streaming 1080p is about twice the
  bandwidth for nothing, and it is what the reconnect loops were made of.
- **Signing in somewhere else no longer throws out the console you are working
  in.** The device keeps a handful of sessions and, when they are all taken, one
  has to give way. It picked the one signed in longest ago - which is the person
  who has been working all day - so a phone, a second browser or a script could
  sign the operator out, and the console then showed "Stream interrupted,
  reconnecting" for ever. Now the one nobody has used for longest gives way, a
  session that is being used keeps itself alive instead of expiring twelve hours
  after the sign-in, and there are eight of them rather than four.

### Added
- **A microSD card formatted exFAT works, and so does one partitioned GPT.**
  Above 32 GB a card comes that way from the factory, and it had to be
  reformatted first - GPT it could not read at all. FAT32 and MBR cards are
  unaffected: the format is read off the card at mount. It costs 12 KB of flash.
  One file still stays under 4 GB on exFAT too: an image, and each part of a
  recording.
- **Alt+Tab, Ctrl+W and the Windows key can go to the target.** A browser keeps
  those for itself, so they never reached the far machine and Ctrl+W closed the
  console instead. In full screen, with control taken, the console now asks
  Chromium for them ("All keys", beside the full-screen button; Firefox and
  Safari have nothing like it and do not show it). Esc hands control back and
  the keys with it.
- **Closing the tab while you are driving asks first.** The browser's own "leave
  site?" question, and only with control taken - a tab left open to watch still
  closes without a word.

## [0.53.0] - 2026-09-22

### Added
- **The M5Stack Mini OLED Unit on the Unit PoE-P4.** The 0.42" 72x40 OLED plugs
  into the Grove port. That port is an I2C bus of its own (SDA 53, SCL 54), not
  the capture chip's bus the OLED used to need, so an OLED can now have a bus
  of its own: two new settings, OLED SDA and OLED SCL, and the M5Stack firmware
  sets them to the Grove pins. The panel is "SSD1315 72x40" in the list.
  Not tried on the unit yet.

### Fixed
- **The USB icon is green in every open console, not only in the one in
  control.** The device sent the target's USB state only to the console that
  held control, so a console that was just watching showed "no power on the
  target's port" until you pressed Take control.
- **M5Stack Unit PoE-P4: no more "1920x1080p15".** The refresh rate was worked
  out from a register the drivers call a pixel clock. On this board it is not
  one - it reads the same bytes whatever the source does - so a 60 Hz source
  showed as 15 Hz. The rate now shows as unknown.

## [0.52.3] - 2026-09-21

### Fixed
- **Install from Home Assistant works (#58).** Pressing Install gave
  "Failed to perform the action update/install. 'payload_install'". The update
  entity did not say what to send, and Home Assistant has no default for it.
- **The rest of the Home Assistant side, made sturdier:**
  - The update check no longer runs on the firmware's timer task. It is an
    HTTPS request with a 20-second timeout, and while it ran every timer in
    the device waited. It now has a short-lived task of its own.
  - Home Assistant shows install progress, and the release notes link.
    Versions go to it without the `v.` prefix, which its version parser
    does not understand.
  - A runbook with a long name made a discovery message bigger than its
    buffer. Now the buffer is bigger, and a message that still does not fit
    is dropped, not sent cut off.
  - A command sent with the retain flag by mistake is ignored. A retained
    `restart` would have restarted the device on every connect.
  - When Home Assistant restarts, the device publishes everything again, so a
    broker that does not keep retained messages loses nothing.
  - Entities for a feature that is off (the update check, power buttons,
    Wake-on-LAN, runbooks) are removed, not left behind as unavailable.
  - Screen and runbook text is cut to 255 characters, the most a Home
    Assistant state can hold.
  - Number sensors keep long-term statistics, and a temperature below zero
    keeps its minus sign.
  - A new diagnostic sensor shows the firmware version, also when the update
    check is off.
- **The browser no longer fills the MQTT user and password with the console's
  own login.** Settings fields now tell password managers they are not a
  login form, and a secret field stays read-only until you click into it.

## [0.52.2] - 2026-09-21

### Fixed
- **The capture bridge on the M5Stack Unit PoE-P4 can be recovered without
  pulling the power.** Switch an Ubuntu target to a text console and its
  LT6911D loses the mode and does not take it back - not in ten minutes, and
  not when the target returns to the desktop. The firmware has had an answer to
  this since the beginning, a fresh hotplug offered to a source that has gone
  quiet, but on this board it could never run: the check is a DDC5V line only
  the TC358743 reports. Now a bridge says whether its DDC5V means anything, one
  that cannot tell is tried anyway - three times and then it stops, so a
  sleeping machine is not poked all night - and a bridge with no hotplug line
  of its own is nudged by pulling its reset pin. Two things the board taught
  while this was written: the chip needs about two and a half seconds with its
  register bus to itself after a reset, or the mode reads stop it re-locking,
  and its reset pin is configured once now rather than on every pulse, which
  was earning a GPIO warning each time. The pixel clock is not a stand-in for
  DDC5V either - with the target's screen asleep it still reads the mode that
  was playing.
- **A console left open overnight signed back in under "The device is
  restarting."** The notice belonged to a session that had ended hours before;
  it was only hidden by the sign-in page, not dropped, and the timer that
  should have dropped it does not run in a tab the browser has frozen.
- **No keyboard or mouse for half a minute after signing in.** The console's
  control socket backs off when the device refuses it, which is what happens
  all the while nobody is signed in - and by morning that back-off is at its
  30-second ceiling. Signing in now reconnects at once instead of waiting the
  ceiling out. It is also why the stale notice sat there: nothing arrived to
  replace it.

## [0.52.1] - 2026-09-20

### Fixed
- **An update could come back as the old version.** With a picture coming in,
  the reboot at the end of an OTA left the new image unable to start: the boot
  hung before it could log a line, the RTC watchdog reset the board, and the
  bootloader read that as "the new image does not run" and went back to the
  previous one. The capture receiver is the reason - it writes every frame into
  PSRAM over AXI, a warm restart cuts that in half, and a half-finished AXI
  transaction cannot be cancelled. It is now stopped before any deliberate
  restart. Seen on a Function EV with a 1080p30 source: three updates in a row
  rolled back, and eighteen in a row went through with the stop in. This is what
  was behind the unexplained rollbacks of 2026-08-30 and 2026-09-16 as well.

## [0.52.0] - 2026-09-20

### Added
- **A keyboard on the page.** A button under the picture opens it; it is the way
  in when the real keyboard cannot be used - a phone, a combination the browser
  or the operating system swallows, a key the local layout does not have. Keys
  are sent as positions, so the target's own layout decides what appears.
  Modifiers latch: once for the next key, twice to lock. Ctrl+Alt+Del, Alt+F4,
  Alt+Tab and Ctrl+Alt+F2 have buttons of their own, and Caps, Num and Scroll
  light up from the target. Three layouts - every key, a compact one with the
  function row and the navigation block, and one of symbols with the number pad -
  two skins, and it can sit under the picture or float where it is dragged.
- **Everyone is told when the device is being updated.** The console that
  started it had its own splash; the others watched the picture stop for no
  reason. They now show the same progress, and the notice clears itself when the
  device comes back.
- **An eye in the password field**, and the browser is asked to remember the
  password properly - through the credential manager, with the fields named the
  way a password manager expects.

- **The status display can be turned upside down.** A panel does not always face
  the way its connector or its enclosure wants it to, and until now the picture
  went with the hardware. There is a switch for it under Screen; it applies at
  once, without a restart, on both the I2C OLEDs and the round SPI LCD.
  Contributed by @Crisspii in #56, who designed an enclosure that needed it.
- **A warning before the tailnet key runs out.** Tailscale gives a node key six
  months at most, and when it lapses the device is off the tailnet until someone
  authorises it again. The device learns the date from the control plane, shows
  it in Settings -> VPN, reports it as `ts.keyExpiry` in the system info, and
  sends a notification a set number of days before - fourteen by default.

### Changed
- **The boot log now says what is on the capture I2C bus.** When no driver
  recognises anything, the firmware scans the bus and prints every address that
  answers, and the console says "a chip answers on the capture bus (0x2b) but no
  driver here knows it" instead of sending someone to reseat a ribbon that is
  already seated. It also asks three times over two seconds, since a bridge with
  its own firmware can be slower than its reset line. New setting for a board
  whose reset is the other way round: the capture bridge's reset line can be
  active high - which is how M5Stack's Add-on Display In wires its LT6911D, and
  why that bridge looked absent until now.
- **The M5Stack Unit PoE-P4 shows a picture.** Its Add-on Display In carries an
  LT6911D rather than a TC358743, and it now works: 23 fps at 1280x720 over
  MJPEG, on a matchbox that takes power, network and video on two cables. The
  chip locks to HDMI, raises its lanes and measures the mode by itself, and the
  capture follows the machine at the other end the way it does on the other
  boards - change the resolution there and the picture comes back a second
  later. H.264 works on it too - 15 frames a second at 1280x720 and 6 at 1080p,
  each for about a third of MJPEG's bandwidth: the encoder on this silicon wants
  YUV420 with line prefixes and no hardware here converts YUV422 into it, so the
  firmware rearranges the bytes itself, in place of the pass MJPEG pays rather
  than on top of it. At 1080p the encoder needs a large contiguous block and does
  not always get one; the picture then stays MJPEG, which the console says. The
  rev 3.x Unit PoE-P4X has neither cost.

  Its microSD works too, at the full 40 MHz and with writes - so recording,
  screenshots, the timelapse and the dashcam are all available on it. They were
  refused before because "may write" was tied to the on-chip LDO that feeds the
  slot's IO rail on every other board; this slot is on the add-on and fed from
  there, so the two are separate settings now.

  One thing to set on the machine at the other end: the add-on's EDID makes a PC
  think it is driving a television, so graphics drivers send HDMI at 16-235 and
  the picture arrives flat. Set the driver's output range to Full.
- **Settings open in a window instead of the side panel.** There are over a
  hundred of them, and a 340 px column was a scroll with no shape. The window
  has the sections in a column on the left and the settings beside them, a box
  to search every setting by name or by what its help says, a filter for the
  ones that differ from the factory values, and a button per setting to put it
  back. Help hides behind an info button rather than sitting under every row.
  The sections, their titles and the headings inside them now come from the
  device: a new setting is still one table row and no console work.
- **The keyboard's extra keys moved beside it.** The arrows and editing keys sat
  under the keyboard, repeating keys the full rows already have and pushing the
  whole thing up over the picture. They stand beside the keys now - only what
  the rows do not carry - and on a phone a button in the header swaps between
  the keys and that block. The ready-made combinations fold away too.

### Fixed
- **A Linux console would not read as text.** Two things were wrong at once, and
  a real Ubuntu screen needed both. 67 rows of 16 pixels leave 8 over in a 1080p
  frame, and the reader assumed the console split them above and below; a
  framebuffer console draws from the very top, so every letter was cut four
  pixels off. Both positions are tried now. And the font was one the firmware did
  not have: a distribution loads its own over the one the kernel carries, drawn
  with a single pixel of stroke where the two tables held used two. Uni2-Fixed16
  is in the table now, which takes it from 282 bitmaps to 782 and reads a real
  console at 100%. The cells off that screen are kept as a test.
- **The browser filled the settings filter with a saved username.** The search
  box sits above password fields, which was enough for the browser to take it
  for a login form.

### Security
- **X-Frame-Options on every answer.** The console already refused to be put in
  a frame through its content policy; this says the same thing to a browser too
  old to read it.

## [0.51.2] - 2026-09-17

### Fixed
- **A clip had no subtitles until the moment the button was pressed.** The
  dashcam kept the past of the screen but not of the keyboard: cues were only
  built while a recording ran. They are built the whole time the dashcam is on
  now, and a clip's .srt starts where its picture starts.
- **Two subtitle symbols came out as empty boxes.** The return and erase signs
  (U+23CE, U+232B) are missing from common subtitle fonts; plain arrows are not.
- **The dashcam could cost the H.264 picture.** Its ring of frames sits in the
  middle of PSRAM, and when the target changed resolution the encoder could no
  longer find one long enough run for its reference frame: the device fell back
  to MJPEG for the rest of the run. Now a codec that cannot get memory asks the
  recorder for its ring first and tries again, and the recorder stays out of the
  way for half a minute afterwards. Seen on a Function EV: 7.7 MB of PSRAM free,
  longest run 4.6 MB, encoder needed more.

## [0.51.1] - 2026-09-17

### Fixed
- **An RSA key in a PKCS#8 file was refused** with "the private key does not
  match the certificate", though it matched: the library compares the public key
  each side carries, and for RSA it fills that in only for the older PKCS#1
  encoding - while a current openssl writes PKCS#8 ("BEGIN PRIVATE KEY"). The
  device now signs with the key and verifies with the certificate, which answers
  the real question whatever the file looked like, and stores the key in the
  encoding the TLS stack reads. ([#52](https://github.com/espkvm/espkvm/issues/52))
- **A stored certificate that the TLS stack cannot use no longer takes the
  console with it.** The pair is checked at start-up; a bad one is logged and the
  device serves its own certificate instead.
- A 4096-bit RSA pair does not fit in the device's settings storage, and the
  message says so now, instead of naming an NVS error code.

## [0.51.0] - 2026-09-17

### Added
- **Record the screen to the microSD card.** A record button under the picture
  writes what you see into VIDEO/ on the card, as H.264 in .ts files - the
  stream the viewers already get, so nothing is encoded twice. A .ts plays in
  VLC and most players, and one cut off by a pulled card, a full card or a crash
  plays up to where it stopped. On the Function EV with a video playing on the
  target, the picture stayed at 21-22 fps while recording and no frame was lost.
  Recording needs H.264 and a card the device can write.
- **Screenshots to the card.** The camera button saves a JPEG into SCREENSHOTS/.
  It works on H.264 too: the device encodes the frame it holds through the JPEG
  engine, which H.264 leaves idle.
- **A panel for both**, on the rail: download a recording, open a screenshot,
  delete either. Deleting and uploading wait while a recording runs, so the card
  only does one heavy thing at a time.
- Files are named by the date. A device without a clock takes the time from the
  browser the first time you record or take a screenshot.
- **A long recording is split into files**, 10 minutes each by default, and
  **stops by itself after an hour** unless told otherwise (both in Settings,
  Video). Each file plays on its own from 0:00.
- **Keystrokes as subtitles.** With it switched on, each recording gets a .srt of
  what was pressed and clicked: "Typed: root", "Ctrl+Alt+Delete", "Down x5",
  "Left click (812, 440)". Off by default. The "keys" mode hides typed characters
  as dots; "everything" writes them out, passwords included. Letters follow the
  target's keyboard layout setting.
- **Recording from runbooks and Home Assistant.** A runbook can `record`,
  `record 300`, `record stop` and `screenshot`, so a schedule can record too. Home
  Assistant gets a "Recording to microSD" switch and a "Screenshot to microSD"
  button.
- **A dashcam.** Switched on in Settings, Video, the device keeps the last stretch
  of the screen in memory, up to 5 MB of it. A quiet screen fits minutes; a
  video playing on the target at about 1 Mbit fits 40 seconds. When something
  happens it saves that past plus 30 seconds after into VIDEO/ on the card: half
  a minute of one flat colour (a stop screen, a blank output), a screen alert
  phrase appearing, or the power LED going off. A button under the picture, a
  runbook's API call and a Home Assistant button save a clip by hand. Another
  event while a clip is being saved makes it longer instead of starting a new
  one. The dashcam needs H.264, and it waits while an ordinary recording runs.
  A black screen does not count as an event: that is usually a display going to
  sleep. A coloured one, like a stop screen, does.
- **The dashcam can keep its past on the microSD card.** Set "where the past is
  kept" to microSD and the device writes the screen to VIDEO/.dashcam in
  15-second pieces, deleting the old ones. A clip joins the pieces it needs into
  one MP4. It reaches back as far as the setting says on any board: on the
  P4-ETH, which has about 3 MB of PSRAM to spare while H.264 runs, memory holds
  only a few seconds. An ordinary recording takes over the card while it runs,
  and the dashcam carries on after it. Tested on the P4-ETH: a 100-second clip
  with two events, no gaps, chapters in place.
- **A clip is an MP4 with chapters.** After it is written the device turns the
  .ts into an MP4 on the card, so it plays in the browser and in the phone's
  gallery. Each thing that happened is a chapter ("Before", "Target power went
  off"), which VLC and mpv show in their chapter menu. A 46-second 1080p clip
  took under 6 seconds to convert.
- **Timelapse.** One frame every few seconds or minutes, played back at 25 fps:
  an hour at one frame in 10 seconds plays in 14 seconds. Start it from the
  recordings panel, a runbook (`timelapse 10`, `timelapse 60 28800`) or
  `record/start?every=10`; the record button stops it. It keeps the stream's
  keyframes, which come about every two seconds, so nothing is encoded again,
  and it runs until stopped or the card is full. It becomes an MP4 when it ends,
  up to 256 MB. A minute at one frame in 2 s on a busy 1080p screen was 24 frames
  and 7 MB.
- **Play recordings in the console.** Play in the recordings panel streams the
  file from the card, with a seek bar and the keystroke subtitles on the
  picture, which a button turns off. An MP4 gets them as a track of the browser's
  player, under its CC button. Nothing is downloaded first. A .ts goes through a small demuxer in the
  console and the same H.264 decoder the live picture uses; an MP4 plays in the
  browser's own player. Downloads now answer HTTP ranges, which is what makes
  seeking work.
- **The clock is set from the network or the browser.** A new setting sets it
  over NTP, so recordings made by the dashcam get dates in their names instead of
  `up-001442`. Without it, the console gives the device the browser's time when
  you sign in.
- **The time zone can be set from the console.** Settings, System has a list of
  cities, ordered by UTC offset, with the browser's own zone at the top. The zone and the time server
  used to live in a settings section the console never showed, so they could
  only be set through the API. File names are in local time now too.
- **Search a recording for what was on the screen.** While recording, the device
  reads the screen as characters every three seconds and writes what it says
  into a .txt beside the video. The recordings panel searches those and plays
  from the moment the phrase was there. Only screens drawn as text can be read -
  a BIOS, an installer, a console - and it can be switched off in Settings,
  Video. `GET /api/v1/captures/search?q=` answers the same.
- **Home Assistant can start a timelapse**, with its own button and a number for
  the seconds between frames. The recording switch stops it.
- **Clips to Telegram.** With notifications on, a saved clip goes to Telegram as
  a video that plays right in the chat, up to Telegram's 50 MB. A bigger one
  sends a message with its file name instead. Webhooks get the message.

### Changed
- `GET /api/v1/video/frame.jpg`, Telegram photos and the Home Assistant snapshot
  now work while H.264 runs, instead of answering 409 or sending no picture.

### Fixed
- **Screenshots failed on the P4-ETH while H.264 ran**, and so did Telegram
  photos and the Home Assistant snapshot: the JPEG buffer was sized for the worst
  case, 2.6 MB, which that board does not have free. Smaller buffers are tried
  after it.
- **A keyframe came every nine seconds on the P4-ETH.** The keyframe interval
  was counted in frames for 30 fps, and that board encodes 1080p at about 7.
  Now a keyframe is sent at least every 2.5 seconds, so a viewer who joins, a
  timelapse and the dashcam get one soon.
- **MQTT stopped publishing for good** if a state message ever came out too long:
  the error path returned without releasing its lock.
- **The device could panic when a viewer left.** The video task and the web
  server wrote to the same TLS connection from two tasks. With dynamic TLS
  buffers one of them freed the output buffer while the other was copying into
  it. It happened most when a browser tab closed or reconnected. The August fix
  only covered our own sends; now every write to a connection takes the same
  lock, and TLS buffers are no longer freed and allocated around each write.
  Before the fix a test that opens and drops viewers crashed the device in 40
  seconds; after it, 279 viewers came and went in three minutes with nothing.
- **Internal memory ran short with the recorder in.** Its buffers were static,
  so they took internal RAM that TLS needs, and logins and Telegram sends failed
  with out-of-memory. They live in PSRAM now.
- **The device rebooted when the codec switch failed.** The capture loop gave up
  and the watchdog restarted the device; now it tries again.
- **A second viewer got 2 fps.** When two viewers waited for a frame, the first
  to wake took the other's wake-up too, and the other waited for its 500 ms
  timeout. Two browser tabs, or a recording and a tab, showed it.

## [0.50.0] - 2026-09-16

### Added
- **The microSD card can be swapped while the device runs.** The slot is watched
  every five seconds while the card is idle. Pull the card and the drive goes
  away from the target; put one in and it mounts and is offered again, no
  restart. A card pulled while the target is reading it is noticed too - the
  read that failed wakes the watcher, which is the only way to tell, because a
  card in use never sits idle for five seconds. On a board running WiFi the
  co-processor holds the other half of the same SD host, so a card put in after
  boot still waits for a restart there; losing one is noticed on every board.

### Fixed
- **The MJPEG picture was always one frame behind.** A browser paints a frame of
  a multipart stream when the boundary after it arrives, and that boundary was
  being left for the next frame. At 30 fps nobody sees it; on a still screen,
  where a frame is only sent when something changes, it meant the console showed
  the screen as it was *before* the last keystroke - a BIOS was almost unusable
  through it - until the 5-second keepalive caught up. Each frame now carries
  the boundary that ends it. Measured on hardware, same screen: the frame's
  closing boundary went from 5.0 s after the frame to 45 ms.
- **A card pulled out of the slot dragged the bus down to 2 MHz.** One yank
  arrives as a burst of failed reads, and each one stepped the clock down a
  rung: 40 MHz to the floor in ten milliseconds. A burst now costs one rung.
- **An empty slot logged an error about the medium it could not offer**, and,
  once the slot was watched, five lines of driver complaint every five seconds.
  An empty slot is a normal state and now says nothing.

## [0.49.1] - 2026-09-15

### Changed
- **Sign out is on the rail, under Settings.** It was only in Settings >
  Security. It asks before it signs out.
- **The target reads the virtual drive faster.** USB asks for 4 KB at a time,
  and a card command per 4 KB cost more than the data. A read that follows the
  last one now takes 256 KB from the card at once. The whole card reads at
  9 MB/s instead of 5.6 on both the Function EV and the P4-ETH, and an image
  file at 7.5 MB/s instead of 5.7.

- **Every board with a microSD slot turns on the slot's LDO.** The schematics of
  the NANO, NANO-WIFI6-DB, WIFI6, WIFI6-DEV-KIT, Module-DEV-KIT, WIFI6-POE-ETH,
  Guition M3-Dev, FireBeetle 2 and VIEWE P4-Pi all power the slot's pins from
  LDO 4, as on the Function EV and the P4-ETH. So they start at 40 MHz too, and
  the rev 1.3 ones can write the card. Not run on those boards yet; the card
  steps down by itself if one does not keep up. The M5Stack Unit PoE-P4 is left
  as it was.

### Fixed
- **Switching the medium did not reach the target.** The drive stayed the old
  size until it was re-plugged, so a new image or the whole card did not show
  up. The device now reports a medium change, and the target reads the new
  size and partitions within a couple of seconds.

## [0.49.0] - 2026-09-15

### Added
- **Find the Telegram chat instead of looking up its id.** Settings >
  Notifications has a Find chats button under the chat id. The device asks
  Telegram who wrote to the bot lately and lists those chats; a click fills in
  the id. Write to the bot or add it to a group first.
- **An old BIOS keyboard mode.** Settings > Input. Some pre-UEFI BIOSes do not
  see a keyboard that is one part of a bigger USB device, and ours is a
  keyboard, two mice and a drive. With this on, the target gets one boot
  keyboard at USB 1.1 speed, like a real one, and nothing else: no mouse, no
  media keys, no virtual media. Asked for in the comments on opennet; no such
  board here to try it on.
- **microSD speed is found per card.** The card starts at the fastest clock the
  board allows and steps down (40, 20, 10, 4, 2 MHz) when it fails to mount, a
  test read fails, or a transfer fails later. While the card is idle the device
  tries one step up again, waiting longer after each miss. The Media panel shows
  the bus speed; Settings > Storage > microSD speed caps it by hand.
- **The log and the console say when the input mode is too fast.** Two MIPI
  lanes carry about 1080p30. A source sending more gave no frames at all, and the
  log filled with timeouts and recoveries that could not help. The device now
  reads the refresh rate from the HDMI bridge (`input 1920x1080p30`), and when a
  mode is over the limit it says so once, in the log and over the picture.
- **Upload speed chart and a Cancel button.** The Media panel draws the upload's
  speed across its progress, like a file copy does, and can stop it. Reloading
  or closing the tab during an upload asks first.

### Fixed
- **Uploads are 50 times faster.** A card image came in at 64 KB/s whatever the
  link: the TCP receive queue held 6 packets, and a full queue dropped the rest
  until a timer ran. Now over Ethernet it is ~3.5 MB/s. The card is also
  written by a separate task, so the network does not wait for it. While an
  upload or an update runs, the video drops to 2 frames a second: the encoder
  and the Ethernet chip share a bus, and at full frame rate the upload fell to
  0.3 MB/s.
- **microSD at 40 MHz on the Function EV and the P4-ETH, and the P4-ETH can
  write it.** On both boards the slot's pins are powered by one of the chip's
  LDOs, which was never turned on. Without it the card only read at 4 MHz, and
  on the P4-ETH's rev 1.3 chip writes timed out, so the card was read-only. Now
  the card reads ~8.5 MB/s and writes ~4 MB/s on both, and the console uploads
  to it on the P4-ETH too (~1.5 MB/s over Ethernet, with the video paused).
- **microSD writes in WiFi mode, and no more hangs.** Now and then a write
  never finished, and the SD host controller then took no command on either
  slot - with the WiFi chip on the other slot, the device restarted. The SD
  driver from ESP-IDF 6.1 is now kept in the project with fixes: a failed
  transfer resets the controller, stops the card and is tried again, and a
  write with missing blocks no longer reports success. The card is writable in
  WiFi mode again.
- **A pre-3.0 board could restart with the console open.** The H.264 encoder
  task on those chips never gave its core away while frames kept coming, and
  the task watchdog restarts a core whose idle task has not run for 10 s. After
  a few such restarts the boot guard swapped to the other firmware slot. Seen on
  a P4-ETH.
- **An upload error said "the card may be full" when the network stalled.** It
  now says what happened.
- **Switching the network left the restart screen counting.** The device comes
  back on another address, which the page cannot follow. The screen now says so
  and offers a link by name and a Reload button.
- **A big upload over WiFi took the WiFi down.** Internal RAM ran out: TLS
  buffers lived there, and so did the WiFi chip's transfer buffers. Both now
  come from PSRAM. A 20 MB upload over WiFi goes through at ~130 KB/s, and
  internal RAM stays above 90 KB free.

## [0.48.0] - 2026-09-14

### Added
- **A Sign out button.** Settings > Security opens with who is signed in and a
  button to end the session. The device had the endpoint since the login
  arrived; the console never offered it.
- **The boot line says more after a crash.** It now carries the raw reset code
  from the ROM (the IDF folds every watchdog into one), and after a watchdog or
  panic a second line says how far the previous run got: into app_main, past
  the confirmation, or a full minute up. For the OTA rollback hunt; costs
  eight bytes of RTC memory.

### Fixed
- **H.264 could not be switched on after boot.** Its encoder needs one 135 KB
  block of internal RAM at 1080p. A board that booted on MJPEG had that block
  at start, but a minute of browser TLS cut it into pieces, and the switch
  failed. The encoder is now built early in boot, before the network starts,
  and kept whatever codec runs. Seen on a pre-3.0 P4-ETH.
- **That failure also filled the log.** On pre-3.0 chips the encoder is built
  on its own task, and the error never reached the capture loop. So there was
  no fall back to MJPEG, and the build was retried ten times a second.
- **Video died for good when the HDMI signal came back.** Every capture restart
  freed the CSI driver's 6 MB spare frame buffer and asked for it again. Once
  PSRAM had fragmented, that request failed and capture stayed dead until a
  reboot. The spare buffer is now allocated once at boot, beside the frame
  ring. Seen on a P4-ETH after its target's screen went to sleep.
- **The microSD card works in WiFi mode.** The card and the WiFi co-processor
  both used SDMMC slot 1, so WiFi mode left the card unmounted. The card now
  takes slot 0 when it is on the slot 0 pins, which is every board with WiFi,
  and the two run side by side. Found and first fixed for the NANO by
  @Crisspii in #46.

## [0.47.1] - 2026-09-13

### Fixed
- **A Telegram alert crashed the device.** The notification task had a 6 KB
  stack, and the first real send - a TLS handshake with api.telegram.org plus
  the message buffers - overflowed it. The device panicked, rebooted, and sent
  the same alert again. 0.47.0 was pulled for it. The task has 12 KB now and
  builds its messages in PSRAM, which also lets the log tail through whole
  instead of cut at 280 characters.

## [0.47.0] - 2026-09-13

### Added
- **Runbooks.** A macro that can wait. A runbook is the macro script plus two
  new lines - `wait Press F2` holds until a row of the screen says so, `gone
  Loading` until it stops saying so - and it runs on the device, so it carries
  on with the browser closed. Enter the setup, pick the boot device, answer an
  installer: the sort of thing that used to need somebody watching. It fails
  with the line it was on when a phrase does not turn up in time (`timeout 120`
  sets how long), and it can be stopped. Waits only ever see a text screen; on
  a picture they wait out their timeout. They live in the console's new
  Automation panel, and each one is a button in Home Assistant, with a sensor
  saying how the last run went. `GET /api/v1/runbooks/status`,
  `POST /api/v1/runbooks/run`, `POST /api/v1/runbooks/stop`.
- **DuckyScript runs too.** Paste a Hak5 DuckyScript into a macro or a runbook
  and it runs as-is - `REM`, `STRING`, `STRINGLN`, `DELAY`, `DEFAULT_DELAY`,
  `REPEAT`, and chords like `GUI r` or `CTRL ALT DELETE`. It is recognised by
  its upper-case verbs, so nothing has to be switched. Keyboard only, and its
  `STRING` is US ASCII like `type`.
- **A scheduler.** Cron lines that fire an action on a timetable - Wake-on-LAN
  in the morning, a runbook overnight, a reset on a schedule. Five-field cron in
  the Automation panel, one of Wake-on-LAN, the ATX buttons, a runbook or a
  device restart. It needs a wall clock, so it sets one over SNTP (a server on
  the local network works with no internet) and does nothing, and says so, until
  the clock is set. A time zone is a POSIX TZ string. `GET
  /api/v1/schedules/status`, `POST /api/v1/schedules/run`.
- **Push notifications.** When a watched phrase appears on the screen, or the
  screen goes blank, the device can send a message - to Telegram, with a
  screenshot attached where the codec is MJPEG, or to a webhook as JSON. It is
  off by default, on its own low-priority task; the TLS session and the copied
  screenshot both come from PSRAM, so the encoder's internal RAM is untouched.
  It can also attach the tail of the device log, so an alert carries the
  context that explains it. There is a Send-a-test button, and the panel shows
  whether the last one got through. `GET /api/v1/notify/status`, `POST
  /api/v1/notify/test`.

### Fixed
- **The VPN client's periodic lines are at debug level now.** 0.46.2 said so a
  release early: the firmware held the tags at warning, but the client itself
  still logged its heartbeat and its ticks at info. Both halves are in now.

## [0.46.2] - 2026-09-13

### Changed
- **Built for speed.** The firmware had been compiled at -Og - the debugger
  setting, and ESP-IDF's default - all along, capture path and TLS included. It
  is -O2 now.
- **Small allocations go to PSRAM.** The default sent anything up to 16 KB to
  internal RAM first, and 16 KB is exactly a TLS record buffer: every
  connection the browser opened carved 20 KB out of internal RAM and handed it
  back later, leaving holes. The H.264 encoder needs 155 KB of internal RAM in
  one piece, and a device with 314 KB free but no run longer than 132 could not
  rebuild it. The threshold is 4 KB now. Diagnostics reports both numbers -
  `internalFree` and `internalLargest` - because the second one is the one that
  mattered and nothing showed it.
- **A hung task reboots the device.** The task watchdog was enabled but watched
  only the idle tasks, and on firing it wrote a warning and carried on. The
  capture loop, the encoder and the USB worker are under it now, and a hang is
  a restart with a core dump rather than a black screen until somebody pulls
  the plug.

### Fixed
- **The device log is readable again with the VPN on.** The Tailscale client
  logged a line per relay heartbeat and per periodic tick, which filled the log
  ring in under a minute and pushed out anything worth reading - a boot that
  rolled back, a failed notification. Those lines are at debug level now, and
  the client's tags are held at warning unless the log setting is at debug.
- **The Home Assistant state message could be cut off.** Its buffer was sized
  for a short screen alert; the alert now names every matched phrase at once,
  and with a long one the JSON would have ended mid-field and been thrown away
  by the broker's consumer. The compiler found it the moment the build went to
  -O2. The buffer fits the worst case now, and a payload that still did not fit
  is not sent at all.
- **A failed H.264 start at boot no longer rewrites the codec setting.** It fell
  back to MJPEG and saved that as the preference, so H.264 was never tried
  again - the one place that did this, when the runtime fallback deliberately
  does not. It falls back for that boot only.

## [0.46.1] - 2026-09-12

### Fixed
- **The blocky picture on a still screen, at the source.** It was never our
  encoder settings and never a shortage of memory: the H.264 component's rate
  controller kept its running bit error in an int32 that nothing bounded. A
  still screen encodes far under its budget - 700 bits a frame against the
  133,000 it was allowed - so the counter marched toward INT32_MIN, reached it
  in about twelve minutes at 1080p and 4 Mbit/s, and wrapped. The controller
  then read an enormous overspend and raised the quantiser one step a frame to
  its ceiling; twelve minutes later it wrapped back and the picture healed
  itself. That is the whole fault, including why raising the bitrate made it
  come sooner rather than help. Espressif fixed it in esp_h264 1.4.0 (the
  counter is 64-bit and saturates at one second of budget), so this release
  requires that version.
- **The mouse jiggler did nothing at all.** It nudges one pixel and straight
  back, and the HID queue coalesces motion that is waiting to go out - so the
  two halves added up to zero and the target received a report with no movement
  in it, which its input layer drops. The counter went up, the screen it was
  meant to keep awake went dark anyway: 1245 nudges on a device here, and the
  target's HDMI asleep for half the night. The nudge now refuses to be folded
  into its neighbours and goes out as two real moves.
- **A console tab left open could erase the device's log.** Its session expires,
  the tab keeps reconnecting every 30 seconds as it is meant to, and every
  refused socket wrote two warnings. Overnight that was 875 refusals, enough to
  flush the 200-line ring in under an hour - so the log kept for a fault no
  longer had the fault in it. The refusal is logged once, then at most once a
  minute with a count of what it skipped.

## [0.46.0] - 2026-09-11

### Added
- **A build target for the VIEWE ESP32-P4-Pi.** A Raspberry-Pi-shaped carrier for
  VIEWE's own P4 module: 32 MB PSRAM, 16 MB flash, an ESP32-C6, IP101 Ethernet,
  microSD and the 15-pin Raspberry Pi camera connector, so a C790 ribbon fits.
  VIEWE publish both schematics, carrier and module, so this is the first board
  here whose C6 SDIO pins were read rather than inferred - and every pin it uses
  turns out to be a firmware default. Three USB ports: one Type-C for power,
  flashing and the log, the other Type-C is the OTG-HS that goes to the target,
  and the Type-A socket is a host port the KVM does not use. Its 40-pin header
  is the Waveshare PoE one pin for pin, which is now three boards with the same
  layout. Not run on hardware yet.
- **A build target for the Waveshare ESP32-P4-NANO-WIFI6-DB.** The NANO with a
  dual-band ESP32-C5 in place of the C6, so it can sit on a 5 GHz network - the
  first board here that can. Its pins come from Waveshare's own published table
  and are the ones already in use; the CSI connector is the 15-pin Raspberry Pi
  one, so a C790 ribbon fits. Two things are specific to it: the board carries
  an ESP32-P4NRW32**X**, which is rev 3.x silicon, so this image is a rev 3.x
  image with no pre-3.0 twin; and the co-processor being a C5 changes the
  esp-hosted profile to the one whose SDIO pins are CLK 18 / CMD 19 / D0-D3
  14-17. Not run on hardware yet.

### Changed
- **The log ring is readable again at INFO.** Every TLS connection wrote a
  "performing session handshake" line, and a few minutes of a browser sitting on
  the console pushed everything else out of the 200 lines Diagnostics keeps. The
  HTTPS server is held at WARN now unless the log level is set to DEBUG.

### Fixed
- **A still screen that broke into blocks, for minutes, with nothing wrong
  anywhere.** The encoder's rate controller can settle at its coarsest quantiser
  and stay there: measured on a device left running, the same unchanged screen
  encoded to 6.7 KB keyframes at QP 40 where a minute earlier it had been 148 KB
  at QP 13. The stream sat at 11 kbit/s of the 4000 it was allowed, so it was
  not short of bandwidth - raising the budget to 12 Mbit/s changed neither the
  bitrate nor the picture. Nothing that can be set on a running encoder moved
  it: not the bitrate, not a longer GOP, not a flood of keyframe requests, not
  parking it across a codec switch. What does clear it is building a new
  encoder, which until now happened only when the source changed resolution.
  The device watches its own keyframes now: three in a row at a fraction of
  their usual size mean the controller is stuck, and the encoder is rebuilt - a
  lost frame, at most once every two minutes. There is a switch for it in
  Settings -> Video, and the log says when it fires. The coarsest quantiser the
  encoder may use is also capped lower (45 -> 32), which bounds how ugly the
  picture can get before the rebuild.
- **A picture that comes back in blocks after the tab has been in the
  background.** Chrome throttles a hidden tab, the H.264 decoder falls behind,
  and the console was dropping delta frames into it to keep latency down. A
  dropped delta breaks the reference chain, so everything after it decodes into
  blocks - and the device cannot see it happen, because it sent those frames.
  The console stops decoding entirely while the tab is hidden, asks for a
  keyframe when it comes back, and does the same whenever it has to drop a
  delta. On the device, a second message from a viewer that is already watching
  is that request; an older console sends its subscribe byte again and gets the
  same repair.

## [0.45.0] - 2026-09-10

### Added
- **A build target for the Waveshare ESP32-P4-Module-DEV-KIT** (and the -A/-B/-C
  kits, which only differ by the screen in the box). It is the WIFI6-DEV-KIT on
  a module - P4, ESP32-C6 and 16 MB flash under one shield, 32 MB PSRAM - so
  every pin the KVM touches is one already in use: Ethernet as on the P4-ETH, the
  C6 on GPIO 14-19, the card slot's power gate on 45, capture I2C on 7/8. Its CSI
  connector is the 15-pin Raspberry Pi one, so a C790 ribbon fits as it comes.
  Read off the vendor schematic; nobody has run it on the hardware yet. Two
  things to know: a jumper switches the OTG-HS between one Type-A socket (what
  the KVM wants) and an internal hub, and that socket drives its own 5 V, so the
  lead to the target must be an A-to-A cable with the 5 V wire cut.
- **A build target for the DFRobot FireBeetle 2 ESP32-P4** (DFR1172, and the AI
  Kit DFR1237 - the same board with accessories). The smallest board that can do
  the job: 60 x 25 mm, 32 MB PSRAM, 16 MB flash, an ESP32-C6 for WiFi and a
  15-pin Raspberry Pi camera connector. No Ethernet, so it starts on the setup
  hotspot like the ESP32-P4-WIFI6. Pins read from the vendor schematic and
  confirmed against Espressif's Arduino variant for the board. Its two USB-C
  ports are not interchangeable: the one by the RST button is the P4's
  USB-serial-JTAG (power, flashing, log - and this build's console), the other is
  the OTG-HS that goes to the target.
- **A list of the P4 boards that have been looked at**, in `docs/PORTING.md`,
  including the ones that do not work and why - most fail on USB, not on memory.
  Plus what to check about a camera connector before buying: 15-pin/1.0 mm takes
  a C790 ribbon directly, 22-pin/0.5 mm needs an adapter.

### Changed
- Built with **ESP-IDF 6.1**, the release, rather than 6.1-rc1. The difference
  between the two tags is documentation and a version string.
- The co-processor component (`espressif/esp_hosted`) moves 3.0.6 -> 3.0.7,
  which fixes a leak of the GPIO interrupt handlers it allocates.

## [0.44.0] - 2026-09-10

### Added
- **A device with no password and no cable now puts out a hotspot.** Until now
  such a device had no way in at all: nothing to reach it at, and no hotspot,
  because the co-processor only runs once a WiFi mode is picked - in the console
  you cannot reach. On a board that has one, a new device waits twenty seconds
  for its network port and then opens ESP-KVM-xxxx with no password; the console
  is at 192.168.4.1. Open on purpose - the password it would invent goes to a
  serial console and a display, and the PoE board has neither - and safe because
  a session with the default password still in force reaches the auth endpoints
  and nothing else. It stops the moment a password is set, and Settings ->
  Network turns it off. Found by the first owner of a PoE board (#42), who
  reasonably expected an SSID.

### Fixed
- **A sign-in the browser throws away now says so.** If the device takes the
  password but the session cookie never comes back, the console used to show the
  same empty form again, which reads as a wrong password.
- **A build with core dumps turned off works again.** The web server asked for
  `esp_core_dump.h` whatever the configuration, and ESP-IDF only puts that
  header on the include path when core dumps are enabled - so anyone rebuilding
  in a tree with an sdkconfig from before 0.43.0 got "No such file or
  directory". Thanks to @petrn for the report (#43).
- **The console no longer knocks on the keyboard socket while nobody is signed
  in.** It opened with the page, so a device showing a login form refused the
  handshake every couple of seconds and logged each one. It waits for the
  session now.

## [0.43.0] - 2026-09-08

### Added
- **A panic now leaves a crash dump.** A device that reboots in the night says
  so in the log and nothing more: the backtrace went out of a serial port nobody
  has a cable for. The chip writes registers and task stacks to flash instead,
  and Diagnostics hands the file over - `GET /api/v1/system/coredump`, DELETE to
  throw it away. Two crashes are open right now with no evidence at all; this is
  what would have closed them. The partition sits in the 56 KB of free space
  below the first app slot, so nothing moves: a device adopts it with one cable
  flash of the table and keeps both slots, its storage and its rescue image. A
  device updated over the network keeps its old table and simply never has a
  dump - the panic handler notices and reboots as it did before.
- **The M5Stack Unit PoE-P4 is a build target**, and its rev 3.x twin the
  PoE-P4X. PoE, an ESP32-P4 and an IP101GRI on the same GPIOs as the P4-ETH, in
  something the size of a matchbox. Capture does not work yet: the add-on that
  gives it HDMI carries a Lontium LT6911D rather than a TC358743, and that
  driver is not written - so these images give the network, the console and
  updates, and no picture. Pins are read off M5Stack's schematics; nothing has
  been on hardware.
- **Releases carry an ELF for each board**, stripped of debug info: 3 MB rather
  than 18, every symbol kept. A crash dump from a device now decodes to function
  names against the release it came from, instead of needing that exact build
  rebuilt first. The full ELF, which also has the line numbers, goes up as a
  build artefact.

### Fixed
- **The Waveshare ESP32-P4-WIFI6-POE-ETH looked dead at boot.** Its console was
  pointed at the serial-JTAG, which that board brings out nowhere: the ROM
  messages arrived and then nothing, which reads exactly like a device stuck in
  the bootloader. Its Type-C is the UART port, and the console goes there now.
  Reported in #42, and the first thing anyone has told us about running this
  board.

### Changed
- **H.264 may use the bandwidth it has been given.** The encoder was held at a
  quality ceiling of QP 25 whatever the bitrate allowed, which on a still screen
  showed as visible blocks while the stream used a fortieth of its budget. 18
  now.

## [0.42.2] - 2026-09-03

### Added
- **A key press wakes a sleeping target.** The device always said it could wake
  a host and never asked. Now any input asks first - a key, a click, a mouse
  move. A host that did not arm remote wakeup refuses, and one that cut the
  port's power hears nothing; that is what Wake-on-LAN and the ATX button are
  for. The jiggler does not wake anything, nor does the key release at the end
  of a session.
- **Home Assistant gets the target's port power as its own entity.** "Target
  USB" only says whether the target enumerated us, which reads as a fault here
  when it is a port that went down with a sleeping machine.

### Changed
- The README says what to do about a source that ignores hotplug, with the
  Orange Pi 4 Pro as the case and a link to a fix for it.

## [0.42.1] - 2026-09-03

### Added
- **The console tells the two dead keyboards apart.** "USB is not active" was
  said both when the target had stopped listening and when its port had no power
  at all, and those want opposite things done: one is fixed by re-plugging, the
  other cannot be reached from this end however hard you press. The device knows
  which - TinyUSB tracks whether there is a live bus - and now says so, in the
  status pill and in what the popup offers. `GET /api/v1/system/usbprobe` grew
  `bus` and `mounted` beside the enumeration trace.
- **The log says when the bus goes quiet and when it comes back.** A target going
  to sleep left nothing in the log at all, so a keyboard that died over a lunch
  break began with no idea whether the host had ever said anything. The suspend
  and resume the host sends are written down now. Our own re-plug makes the bus
  go quiet too, and that one is deliberately not logged: a false "the host
  suspended the bus" in the middle of a repair is worse than no line.

### Changed
- **A re-plug asked for by hand is a second off the bus, not 100 ms.** A hub is
  allowed to ignore a port change shorter than 100 ms while it debounces, and the
  startup figure sits exactly on that limit. It stays there, where it is proved,
  because start-up has a deadline; a button press has none.
- **No more re-plugging into a port that has no power.** The retry after start-up
  used to spend all three of its attempts whatever the state of the bus, so a
  machine that was simply asleep burnt them in the first forty seconds and had
  none left when it woke. It now waits instead, keeps its tries, and writes one
  line saying why - rather than three that could not have worked.

### Fixed
- The wiring notes now say that on the p4-eth the target's 5 V comes back down
  the OTG lead, so pulling it at the target's end reboots the device - worth
  knowing before anyone is told to re-plug a cable to fix a keyboard. The notes
  also carry the case this release came from: a target that slept, woke with no
  power on its USB port, and could not be reached from this end at all.

## [0.42.0] - 2026-09-03

### Added
- **Two settings can no longer be put on one pin.** The ATX buttons and the
  round LCD are both wired by hand and both pick their GPIOs from the console,
  and until now nothing noticed when they named the same one - or when one of
  them named a pin the board's own hardware holds, like the capture bus or the
  Ethernet PHY. Neither shows up as an error: the panel simply stays dark, or
  the network stops. A settings write that would do it is now refused, naming
  the pin and what already has it. The pins are weighed as a set and after the
  request is imagined applied, so moving the display off a pin the buttons want
  still goes through in one go; a clash a device already has stored does not
  block unrelated changes; and a setting the console hides - the LCD's pins on a
  device running an I2C OLED - is left out of it. The rule has host tests
  (`tools/test.sh`).

- **A capture bridge driver announces itself.** The capture path used to call
  the TC358743 by name and know its I2C address, so a second bridge would have
  meant editing every one of those call sites. Now a driver registers a detect
  function, and the capture path asks for whatever answers on the bus, getting
  back a name to show and a table of operations to drive it with. Adding a
  bridge is adding a component. Nothing about the picture changes - one driver
  is registered and it is the same one - and the interface is honestly a guess
  until a second bridge exists to shape it. The idea comes from Espressif's
  esp_cam_sensor (#34), though not the mechanics: a constructor does for a
  handful of drivers what a linker section does for dozens, without a section to
  place by hand.

- **A source that is on but sending nothing now gets a fresh hotplug.** The
  bridge holds HPD low until this firmware has booted and started the capture,
  some fourteen seconds in - eight of them the password-reset window. A machine
  that boots faster looks at the input, finds no monitor, and configures no
  output; single-board computers in particular probe once at start and never
  look again, and raising HPD afterwards is not the edge they act on. The
  monitor now watches for that exact case and pretends to be unplugged and
  plugged back in - at ten seconds of silence, then twenty, then forty, and then
  it stops. It only does this while DDC5V says the source is powered and
  attached, so a target that is simply switched off is left in peace, and the
  count starts again when the picture returns or the input is unplugged. Found
  from a report of a C790 with an Orange Pi: DDC5V present, TMDS never.
- **The console says which silence it is.** "No signal" was followed by "it may
  be powered off, asleep, or its cable unplugged", which sends somebody whose
  machine is plainly running to check the wrong things. The device already knew
  better - DDC5V is in the status it serves, shown until now only as a raw hex
  byte - so the page now separates "nothing is plugged into the HDMI input" from
  "the cable is connected and the target has power, but it is not sending".

- **A dead keyboard can be re-plugged from the console.** A restart resets this
  side of the USB cable while the target's power never drops, and this OTG port
  does not always give the target a clean detach - so the target goes on holding
  a connection this side has forgotten, and every keystroke goes nowhere. The
  firmware already came back as a new device once at start-up to head that off.
  That is one attempt, and a target busy with its own boot can miss it, so it is
  now tried again while nothing has enumerated us: ten seconds after the last
  try, then twenty, then forty, and then it stops. A target in the middle of
  talking to us is left alone - the enumeration trace tells us it is - and one
  that is simply switched off is not poked forever. And clicking the USB dot in
  the status bar now offers "Re-plug USB", which does the same thing on demand:
  off the bus for 100 ms and back, without restarting either machine. Also on
  the API, as `POST /api/v1/hid/reattach`.

### Changed
- **The ATX buttons now start on pins that are free, per board.** They defaulted
  to "unassigned" with the wiring page suggesting GPIO 20, 21 and 22 - which is
  where the round LCD starts, so a device wearing both and set up by following
  that page ended up with three pins claimed twice. The power and reset buttons
  now default to 46 and 47, chosen clear of everything else on every board. The
  LED sense stays unassigned: its input is biased toward "off", so a pin nobody
  wired would report the target as powered down for ever, and a relay board
  cannot sense the LED at all - 48 is simply documented as free for it. The
  Guition, whose header brings out ten usable pins in all, gets its own set; its
  display defaults were wrong in the same way and are fixed, three of the five
  pins it offered not being on that board's connector at all, so a panel wired
  by them could never have worked. Nothing moves on a device that is already set
  up - a default is only consulted when nothing is stored - and nothing is driven
  until ATX control is switched on.
- The console leaves a hidden setting's pin out of the free-pin arithmetic, the
  way the device now does. The round LCD's five pins are stored even on a device
  running an I2C OLED, and they were being held back from the ATX pickers there
  for a panel that is not connected.
- **The C790's audio connector is written down**
  ([docs/HARDWARE-NOTES.md](docs/HARDWARE-NOTES.md)): the bridge hands HDMI
  audio out as I2S on a 5-pin header of its own, with a cable in the box, and
  the notes now carry its pinout and what wiring it would take. MCLK is not
  connected there, so the P4 would have to receive as the slave. Nothing
  captures audio yet.
- The ATX wiring page no longer claims GPIO 9-13 are held back for that audio.
  Those pins do not come out on any board this firmware builds for, so the
  sentence only sent people looking for a header that is not there.
- **A relay module is documented as an alternative for the power and reset
  buttons** ([docs/wiring.md](docs/wiring.md)). It needs no firmware change -
  the pins are driven the same way - but it cannot sense the power LED, so that
  wire stays an optocoupler or goes unused.
- A second printed case is listed - Fabrion365's on MakerWorld, adapted for this
  project at a user's request (discussion #18), next to Colin Hickey's.
- The Geekworm C792 is named in the hardware list. It is the same TC358743 with
  a splitter in front, so it should work; nobody here has one, and the note says
  so, along with what its EDID may do to a 1080p60 source.
- The device now says why a session check came back negative - "no cookie sent"
  against "cookie holds no session I know". A console that drops to the login
  screen is only repeating that answer, and until now there was no way to tell
  the two apart from a log (#31).
- A write refused for the wrong reason no longer says "authentication
  required". A POST or PUT must carry JSON or the console's own header - that is
  what keeps a form on another site from riding a cookie - and a request without
  either was answered as if the password were wrong, which sends whoever wrote
  the script hunting a login problem they do not have. It now says which rule
  turned it away, and a request that really did come from another site says that
  instead. Found calling `POST /api/v1/hid/reattach` with curl, which has no
  body to declare.
- A pin with two fixed uses is one row in the pins list, not two. On the p4-eth
  GPIO 35 is the BOOT button and Ethernet TXD1 both, and the console drew it
  twice.

## [0.41.3] - 2026-08-30

### Fixed
- **The pointer landed in the wrong place when the window was not the target's
  shape.** The console maps a click into the rectangle the picture actually
  occupies, so the black bars around it are skipped - but on the MJPEG stream it
  asked the `<img>` for its width, and on an image that property is the size it
  is drawn at, not the frame's. So the bars counted as picture, and the further
  from the centre you clicked the further off it landed: two cursors, drifting
  apart. A window at the target's own aspect has no bars, which is why resizing
  appeared to fix it. The H.264 path draws into a canvas, which reports the
  frame honestly, and was never affected. The arithmetic now has tests.
  Reported in #32.

## [0.41.2] - 2026-08-30

### Added
- **An SSD1315 entry for the status display, marked untested.** The SSD1315 is
  the SSD1306's near-twin and the existing entry should already drive it; the
  one command it has that the SSD1306 does not is `0xAD`, which says where the
  current reference comes from, and a module without an external resistor stays
  dark until it is told to use the internal one. Nobody here has one, so the
  choice says so until somebody reports back (discussion #15).

### Fixed
- **The keyboard stayed captured under the login window.** When a session ended
  while the console had control, the login screen appeared but every keystroke
  was still swallowed and sent to the target, so the password could not be
  typed into the form in front of you. Esc released it, which is no way to find
  out. It now releases itself. Reported in #31.
- **Signing in could leave the form sitting there.** After the password is
  accepted the console asks the device who it is now; a failure of that second
  request went unhandled and the form just stayed up, with a page reload the
  only way on. It says what went wrong instead. Also #31.
- **A display that was never switched on said "not probed yet".** The line stayed
  at its boot value forever, which reads like a panel that failed rather than one
  nobody asked for. It now says it is switched off, from the start. Found in a
  user's log in discussion #15.

## [0.41.1] - 2026-08-29

### Fixed
- **The device no longer reboots when an H.264 viewer disappears.** A tab closed
  at the wrong moment, or a link that drops, could take the whole device down.
  The video task was still writing a frame into a TLS session while the web
  server was freeing it, and the crash landed later, in whatever code asked for
  memory next. Sends and session teardown are now serialised. Twenty-four
  disconnects in a row, with heap poisoning on, no crash - the same build
  without the fix died on the second.
- **The SDIO pull-ups on the Waveshare ESP32-P4-WIFI6 now actually happen.**
  0.40.1 added the setting and the code behind it but never called it, so it
  changed nothing on the board it was written for. With the call in place the
  WiFi link stops stalling: @nwomn, who owns one, went through resets and
  reassociations, ten disconnect cycles and a hundred requests without an error,
  at the full 40 MHz. Found and fixed by @nwomn.

### Changed
- Our own components now build with unreachable code as an error. The pull-up
  bug was a function nobody called, and the only sign of it was a warning nobody
  read.

## [0.41.0] - 2026-08-28

### Added
- **Home Assistant gets a firmware update entity.** It shows what is installed
  and what the project has published, with a button that installs it. The device
  reads the manifest itself now, at most once every six hours - until now only
  the console did that, from the browser, which is no use to a dashboard. Only
  where the device is allowed to fetch (Settings -> the same switch that lets it
  install a published release); with that off there is no entity, because one
  that can never answer is worse than none.
- **A camera with a still of the target's screen.** A button takes one on
  demand, and there is a setting to take one by itself when the screen watch
  matches a phrase - so the notification that says `kernel panic` carries the
  screen along with it. Needs the MJPEG codec: while H.264 runs there is no
  still to take. Off by default, because a 1080p frame is a few hundred
  kilobytes over the broker.
- **The mouse jiggler as a switch, with its interval beside it.** The point is
  an automation: quiet during the day, awake overnight. Turning the switch back
  on restores the last interval you set.
- **Diagnostics worth having when something is wrong:** free internal memory and
  the largest unbroken block in it - the gap between those two is what decides
  whether the H.264 encoder can start - along with skipped frames, which
  firmware slot is running, and why the device last booted.

## [0.40.1] - 2026-08-28

### Fixed
- **The ESP32-P4-WIFI6 now gets the chip's own pull-ups on its link to the WiFi
  co-processor.** That board holds all six SDIO lines high through 51k, where
  Espressif ask for 10k, and one of those lines is how the co-processor says it
  has data. Miss that signal and the link sits there associated, addressed and
  silent, while ordinary commands still work - which is what the board's owner
  measured. esp-hosted never switches the internal pull-ups on and offers no
  setting for it, so the firmware now does, before the driver claims the pins:
  about 45k in parallel with the board's 51k, which lands near 24k.

  Whether that is enough is a question for the board rather than the firmware,
  and it is not settled yet - if it is not, the honest answer is a resistor. On
  by default for that board only, and free where a board is already wired right.

## [0.40.0] - 2026-08-28

### Added
- **The Waveshare ESP32-P4-WIFI6, contributed by [@nwomn](https://github.com/nwomn),**
  who has the board. Capture through the C790 and the USB keyboard and mouse are
  confirmed on hardware, and the header and pin reservations are checked against
  the real thing rather than guessed from a drawing. Published for both silicon
  revisions, and offered by the browser flasher.

  **Its WiFi is not dependable yet, and it is the only link this board has.** The
  co-processor associates and hands out an address, then the data path can stall:
  the SDIO interrupt from the C6 stops arriving while commands still work. It
  looks like a board-level problem rather than firmware - the SDIO lines there
  are pulled up through 51K where Espressif ask for 10K - but it is not settled,
  so treat the board as experimental.

- **A drag keeps its path.** Pointer reports were merged into the latest
  position, which is right while no button is down and wrong while one is: a
  drag reached the target as a press and a release with nothing in between, so
  drawing and drag-and-drop lost everything in the middle. Also from @nwomn
  (#28), validated by drawing on a tablet through the KVM.

## [0.39.0] - 2026-08-28

### Added
- **The Waveshare ESP32-P4-WIFI6-DEV-KIT is a build target.** It carries both
  links: 100M Ethernet on a PoE-capable magjack and an ESP32-C6 for WiFi 6. Its
  pins are the ones we already use - Ethernet as on the ESP32-P4-ETH, the C6 on
  GPIO 14-19, the card slot gated on GPIO 45 - so the overlay declares almost
  nothing. One thing to check before wiring: the USB OTG port is switched
  between HOST and DEVICE by a jumper, and the KVM needs DEVICE. Configured
  from the published schematic and not yet run on one, in both silicon
  revisions (`p4-wifi6-devkit`, `p4-wifi6-devkit-rev3`).

  Two more boards in that family were looked at and left out: the ESP32-P4-Pico
  and the ESP32-P4-Core-DEV-KIT have neither Ethernet nor a WiFi co-processor,
  and a KVM nobody can reach over the network is not much of a KVM.

### Fixed
- **The capture reset pin is not driven on this board.** GPIO 23 is the
  TC358743's RESETN on the ESP32-P4-ETH; here it is an ordinary expansion pin.
  GPIO 6 is also no longer offered as free - it goes to the WiFi
  co-processor's IO2.
- **A board with no wired port can be built at all.** The hostname setting and
  the recorder that puts the IPv4 address in the certificate both assumed
  Ethernet, though both also serve WiFi. Groundwork for boards with no wired
  port; nothing changes for the ones that have one.

## [0.38.1] - 2026-08-28

### Fixed
- **H.264 comes back after MJPEG.** Switching to MJPEG and back left the device
  on MJPEG, while the setting still said H.264. The encoder wants one unbroken
  block of internal memory for its reference frame, 135 KB at 1080p, and asks
  for internal only. That memory fragments as the device runs, so the block was
  there at boot and gone an hour later: 323 KB free on the bench, longest run
  132 KB. The encoder is now left alone when the codec changes, and rebuilt only
  when the resolution does.
- **Three string truncations, found by building with -O2.** The certificate
  authority is named after the hostname plus the last of the MAC, and the MAC is
  what keeps two devices apart; a long enough hostname would have pushed it out
  of the buffer. A path to an image on the card was cut instead of refused.
  Neither is reachable with the values the settings allow.

## [0.38.0] - 2026-08-27

### Added
- **A mouse jiggler.** A machine left alone locks its screen or goes to sleep,
  and then what you were watching is behind a password. Set an interval in
  Settings and the pointer is nudged one pixel and put straight back that often
  - nothing moves on screen, and the target counts it as somebody being there.
  It runs on the device rather than in the browser, because the case it exists
  for is a machine nobody is sitting in front of; a console-side one would go
  with the tab. It stands aside whenever you are using the mouse yourself, and
  does nothing when no target is attached. Off by default.

### Fixed
- **The update popup no longer sits lit up under the restart screen.** Starting
  an update raises a full-screen progress panel over a blurred page, and the
  popup the button was in stayed open behind it - with its own progress bar
  visibly filling through the frosting. It closes as the panel goes up.
- **That popup no longer scrolls sideways.** Long version names pushed the rows
  wider than the popup, and asking for a vertical scrollbar quietly gets you a
  horizontal one as well, so a stray bar slid about under the restart screen.
  The rows shrink and the names end in an ellipsis instead.
- **The mark on the sign-in card is centred.** It sat against the left edge with
  the fields, which was never the intention.

## [0.37.1] - 2026-08-26

### Fixed
- **Installing a published release no longer gives up when the link goes
  quiet.** A download that paused for a moment was read as a broken one and
  stopped with "the download broke off" - the read returns a negative number
  both for "nothing yet" and for "the connection is gone", and the two were
  being treated the same. A quiet spell is now waited out, as an upload from
  the browser already was. Nothing was ever at risk: the image is written to
  the spare slot, so a download that fails leaves the running firmware alone.
- **The list of releases is easier on GitHub's rate limit.** It is fetched by
  the browser, so the 60 unauthenticated calls an hour GitHub allows are counted
  against the address the console is opened from - your own, and never shared
  with other people running ESP-KVM. The list is held for ten minutes now, so a
  reload does not spend another, and if the limit is reached the console says so
  and when it clears rather than showing a bare 403.

## [0.37.0] - 2026-08-26

### Added
- **The screen as text, instead of the video.** A text screen is about two
  kilobytes; the video is megabits a second. Tick "text when the screen is text"
  in the video readout and the console shows the characters and stops the
  stream, so a machine stays workable over a phone tether or any link that will
  not carry a picture - the keyboard still reaches the target, which is most of
  what a BIOS asks for. It is a standing preference rather than a place to go
  back to: with it on, the view follows the target through a boot by itself -
  characters at the boot menu, the picture the moment a desktop paints,
  characters again at the next restart - and the encoder only starts when a
  reading fails. Both directions need two readings to agree, so a screen that
  sits near the edge of being readable does not flip back and forth. The
  readings arrive the way the picture does - pushed along the same socket as the
  device makes them, carrying only what changed - so walking a boot menu moves
  the highlight about as fast as the target repaints it.
  `GET /api/v1/screen/text` is unchanged, for scripts, Home Assistant and
  anything whose socket will not open.
- **The selected row comes through in text.** A menu says which line you are on
  by drawing it the other way round, and text alone loses exactly that. The
  scanner already had to try both polarities to read such a row, so it now keeps
  the answer: inverted cells are reported (`highlight` in
  `/api/v1/screen/text`) and drawn inverted in text mode. It is relative to the
  screen - an installer drawn black on white has nothing highlighted - and the
  blanks inside a highlighted label travel with it.
- **A screen that has gone to one flat colour is noticed.** Reading characters
  cannot cover a modern Windows stop screen: it is a graphical page in a
  proportional font, with no grid to cut. What it does have is a shape a few
  hundred samples can see - nearly all of it one colour, and it stays. The
  device reports how long the picture has been flat (`flatMs` in the video
  status, "One flat colour for" in the video readout) and publishes it to Home
  Assistant as a binary sensor with the seconds beside it. It catches a blanked
  output and a frozen desktop too; what it means is the operator's call.

- **Tests that run without a device, in one command.** `tools/test.sh` covers the
  C that reads a screen as characters and the C that decides a screen has gone
  flat, the console's own logic - the demo's machine, the settings file, the
  keyboard tables, the text layer - and the paste tables. The console side is
  new: node runs the TypeScript as it is, so it costs no dependency, and it is
  where a regression like "fast typing repeats a letter" gets caught before
  anybody else finds it. CI runs the same suites.

- **A viewing token, so a dashboard need not be given the keys.** Home
  Assistant's camera integrations can be handed a URL and little else, and this
  device authenticates with a session cookie - so putting a target's screen on a
  dashboard used to mean turning the login off. A token (Settings -> Security,
  off until you make one) opens the MJPEG stream, one frame and the capture's
  figures, and nothing that can press a key or cut the power. Only its hash is
  kept, so it is shown once.

- **Go back to any published release, not just the other slot.** The two slots
  already let the console boot back one step, which helps only while the image
  you want is still sitting in the other one. Settings now lists what the
  project has published and installs the one you pick - a version from weeks
  ago, or one you skipped. The device fetches the image itself, because the
  browser is not allowed to: the host serving them refuses cross-origin reads,
  whichever way the URL is reached. That means the device talking to GitHub, so
  it is **off until you turn it on** in Settings - a KVM often sits where
  nothing is meant to reach the internet, and ordinary updates do not need it.
  A release that lands badly is still reverted by the bootloader, exactly as an
  upload is.

### Changed
- **Security headers on every answer, and a stricter idea of who is asking.**
  A policy that allows this device and nothing else, no framing at all (a
  console inside somebody's invisible frame, with "force off" under the
  pointer), and no content-type guessing. A WebSocket upgrade from another
  origin is refused before the session is looked at, and a request that changes
  something has to carry either JSON or the console's own header - which a
  cross-site form cannot produce.
- **The hotspot no longer comes up open by default.** With no password set the
  device now makes one up the first time the hotspot starts and prints it in the
  log and on the display, rather than running an open network anybody in the
  building can join. An open hotspot is still available, as a deliberate choice
  (Settings -> Network).

### Fixed
- **A few things found by re-reading the new code rather than by running it.**
  The moment a screen went flat was kept as a 64-bit number written by the
  capture task and read by two others - two loads on this core, so a reader
  could catch half an update; it is 32 bits now. Text mode gave the picture back
  even to somebody who had paused it themselves, and left the last frame showing
  under the characters. The MQTT escaper passed control bytes through into what
  is meant to be JSON. A packed-YUV frame with an odd number of pixels would
  have been sampled one byte past its end. None of these had been seen in the
  wild; all of them were waiting.
- **Text mode reads the screen again while it is up.** Found on hardware: text
  mode stops the video, and with nobody watching the device only re-read the
  narrow modes on its own - so on a 1080p console the characters froze the
  moment the mode was entered. Walking a boot menu moved nothing. An operator
  asking for the text now counts as a reason to read, in any mode the reader
  understands, and four times a second while they are there. The console asks
  again right after a keypress instead of waiting for its poll.
- **The MJPEG codec could not be selected once H.264 was running.** The switch
  was only considered when somebody was watching, and the only ways to watch
  MJPEG - the stream, a single frame, anything the viewing token opens - refuse
  to serve while H.264 runs. So the setting saved, nothing changed, and the new
  viewing token had nothing to open. The codec is settled before that check now.
- **The security headers really are on every answer.** They were a call each
  handler had to remember, and the handlers that send their own body - system
  info, the video status, the auth replies - did not. They are set once, where
  routes are registered. "No such page" and "wrong method" come from inside the
  server and never pass a route of ours, so those carry them too now.
- **The MCP adapter can press the power button again.** Its power calls carry no
  body, so they carried no content type either, and the new "did the operator
  mean this" rule turned them away. It sends the console's own header on
  everything now.
- **Where to switch how the screen is shown.** H.264, MJPEG and text were kept
  in three places - two in Settings, one as a button under the picture. They now
  live in the video readout in the status bar, next to the figures they change:
  the codec as two buttons, and text as a tick under them, because which codec
  to send and whether to read the screen as characters are different questions.
  While the characters are up the readout says so rather than naming a codec
  that is deliberately not running.
- **A long screen-watch list no longer truncates the MQTT payload.** The state
  buffer was sized when an alert was one short phrase; the watch has been naming
  every phrase on screen since 0.34.0, and a full list would have been published
  as unparseable JSON. It is sized from the worst case now.
- **The phone's keyboard stops pushing the picture off the screen.** A virtual
  keyboard covers the window rather than shrinking it, so the console was laid
  out as if it were not there and the browser did the only thing left to it: it
  scrolled the page to reach the field, taking the status bar and the top of the
  screen with it. The keyboard is measured now and the console is sized to what
  is left, so the whole picture stays visible above it and nothing scrolls.
- **A capture board that is not plugged in is noticed.** The probe never spoke
  to the bridge - it only registered an address on the I2C bus - so with the
  ribbon off every later read came back with rubbish off the wire, the device
  started a capture on a chip that was not there and reported "no signal". It
  asks the bus for the address now and checks the answer really is a TC358743.
  When nothing answers the console says video is not available and repeats the
  device's own reason: check the ribbon. Input, media and power carry on as
  before - a missing capture board was never a reason to take the rest down.
  Found on a Waveshare board with the ribbon unplugged.
- **Fit stops blowing a small screen up.** Fit meant "fill the stage", so a
  640x480 BIOS on a big monitor was stretched over the whole window and every
  character came out soft. It now shrinks a picture that does not fit and leaves
  one that does at its own size. The old behaviour is still there as Stretch -
  the button walks Fit, Stretch, 1:1 - for anyone who wants the picture as large
  as the window whatever it measures. The pointer still lands where it is
  pointed in all three.
- **The target's keyboard and mouse work again straight after an update.** A
  restart resets our side of the USB link, but the target's power never drops
  and this port does not always give it a clean disconnect - so the target went
  on addressing a device that had forgotten who it was, and nothing reached it
  until the cable was unplugged and back in. The device now drops off the bus
  for a moment on every boot and comes back, which is all a replug ever did.
- **Control no longer gets stuck with a session that has gone.** Only one
  browser drives the target at a time, and releasing that claim could give up if
  it could not take a lock at once - leaving the claim held by a connection that
  no longer existed. Every other console was then quietly ignored, with no
  notice and nothing to press. It waits for the lock now, as the rest of the
  bookkeeping already did.
- **"Take control" is offered in text mode too.** Text mode stops the video, and
  the notice that says another session is driving was hidden along with the
  picture - so an operator reading the characters found the keyboard dead, with
  no explanation and no way to take over.
- **A picture that froze when H.264 was selected.** Switching codec ends the
  older multipart stream cleanly, and a cleanly ended one raises no error in the
  browser: the console kept showing the last frame it had and never moved to the
  channel that carries H.264. It now follows what the device reports it is
  encoding. The same mix-up could also leave the console retrying a stream the
  device answers with "not this codec" every couple of seconds, forever; with no
  transport that can carry the picture it now says so once.

## [0.36.0] - 2026-08-25

### Added
- **A panel can be pinned open beside the picture.** Panels float over the
  screen on purpose - a picture that resizes whenever one opens makes you find
  everything again - but on a wide screen there is room for both, and then
  covering the target is the worse of the two. The pin in the panel's header
  hands it a strip of the stage instead: the picture shrinks to what is left and
  everything drawn on it stays centred on the picture. Off by default,
  remembered per browser (it depends on the window, not the device), and not
  offered on a phone, where the panel is the whole width.
- **Arrow keys on a phone.** A soft keyboard has no arrows, which makes a BIOS
  menu or a boot list impossible to drive from a phone. Touch mode has a pad
  now: the four arrows in the shape they have on a keyboard, with Esc, Tab and
  Enter around them - missing for the same reason and wanted in the same places.
  Hold a key and it repeats.

### Changed
- **The video figures open where they are shown.** Ten numbers about the capture
  had a panel of their own, which slid over the very picture they describe. The
  headline ones have always been in the status bar; clicking them now opens the
  rest just below, and the rail is one button shorter. The bar itself is
  shorter too: "skipped" waits in the readout, and whether the device answers at
  all stands beside the figures rather than inside them - that is about the
  connection, not the picture.
- **Power is a menu on the rail, not a panel.** Three buttons and Wake-on-LAN do
  not need a panel sliding over the screen - and that panel covered the very
  thing you want to watch while a machine restarts. The button sits at the foot
  of the side rail with the target's power state on it, and the menu opens
  beside it.

### Fixed
- **A demo machine that is switched off looks switched off.** With no power there
  is no text screen, and the drawing fell through to the constellation - so a
  target that had just been shut down showed a running desktop, and Reset, which
  does nothing to a machine that is already off, looked broken. It is a black
  screen and "No signal" now, and the power button brings it back.
  Thanks @DaveDavenport.
- **The install stops crying "longer than usual" on a healthy update.** Writing
  and verifying an image takes past forty seconds on a real board, and the
  console expected twelve, so every update looked like it was going wrong; the
  wait for the device to come back was short by a few seconds too. Both now sit
  past what a healthy device takes. Reported by @petrn (#22).
- **The version badge stops competing with the install screen.** It filled as a
  ring and pulsed while the same install was described full-screen - two things
  telling one story, and the moving one made the version underneath hard to
  read. The badge carries the version; the screen carries the install.
  Reported by @petrn (#22).
- **Clicking the picture answers you.** Taking control is a button, deliberately
  - the first click would otherwise land on the target - but clicking the
  picture did nothing at all, which reads as a broken console. It waves the
  button at you now.
- **The demo points at the control it is asking for.** "Open Media" and "turn on
  Select" are obvious to anyone who knows the console and to nobody else, so the
  button in question glows while the demo waits for it, and the screens name
  where it is.
- **The demo types like a keyboard.** Two keys overlapping - which is what fast
  typing is - made it repeat the first one, because it read every report of what
  is held down as a fresh press. Only the device's own demo was affected: real
  hardware passes the report to the target, which decides what is new.
  Thanks @DaveDavenport.

## [0.35.0] - 2026-08-25

### Added
- **Every release carries symbol maps** - one `espkvm-<version>-symbols.zip`
  with a map per board. A panic prints an address and nothing else, and the
  build that could turn it into a function name is thrown away, so reading
  somebody's crash meant rebuilding that exact release first. Under a megabyte
  for all of them, and it turns an afternoon into a minute (#22).

### Changed
- **One list of checksums instead of one per board.** The per-board files were
  an accident of building the boards in parallel; the release page now carries a
  single `sha256sums.txt`, which is also what people actually check against.
- **The demo runs itself until you take over.** It asked for three decisions
  before anything happened - switch virtual media on, choose an image, press
  Reset - and a first-time visitor has no reason to know that. Now a machine with
  nothing in its drive says so, counts fifteen seconds down - long enough to look
  around and pick an image yourself - then loads one and boots it, and types the
  one command the screen asks for. The first thing you do stops the autopilot for
  good, and the screen tells you what the next step is at every point.
- **Handing control back does not take the pointer with it.** Pressing Esc on one
  of the demo's desktops made the drawn cursor vanish. It stays where it was left
  now, the way a real machine keeps its own pointer, while the screen behind it
  carries on moving; it appears only once you have actually driven, so nothing is
  parked in the middle of a screen nobody has touched.
- **The flock wanders off from a hand that has stopped.** The sheep gather round
  the pointer while it moves; after four still seconds they lose interest and go
  back to grazing, and they come back when it moves again.

### Fixed
- **Virtual media shows what is really in the drive.** The panel read the card
  once, when it was opened, so a medium that changed after that - a second
  operator, an upload finishing elsewhere - left it saying "ejected" while the
  target was plainly booting. It re-reads every few seconds while it is open, and
  a choice made here always beats a listing that was already on its way.
- **No scrollbar down the side of the console.** The page is deliberately one
  pixel taller than the window so Android's captive-portal browser stops
  swallowing downward drags. On a desktop there is no such gesture, and that
  pixel drew a scrollbar for nothing. It is given to touch screens only now.

## [0.34.0] - 2026-08-24

### Fixed
- **A refused firmware image no longer leaves the console behind a frozen
  screen.** The install takes over the whole screen, and when the image was
  rejected - the wrong file, or a download that arrived damaged - the message
  appeared underneath it while the overlay stayed up saying what it had been
  doing a moment ago. Only a reload got you out. The screen comes down with the
  failure now.

### Changed
- **The screen watch names every phrase it finds, and has room for more of
  them.** It reported only the first match, so a machine showing both a panic and
  a failed disk told you about one of them; now the alert lists all of them, in
  the order they were typed. The list of phrases also grew from 127 characters to
  255, which was the real limit - three or four phrases and it was full.

## [0.33.1] - 2026-08-24

### Fixed
- **One slow client can no longer take the whole console down.** The web server is
  a single task, and every accepted connection carried a thirty-second receive
  timeout, so a client that opened a connection and never finished its request
  stopped the device answering anybody for that long - measured at 30.9 seconds on
  the bench. The device still pinged and still served a page now and then, which
  is what made it look like a network fault rather than a stuck server. The wait is
  five seconds now; a firmware or image upload is unaffected, because every place
  the server reads a body already waits out a silence rather than treating it as a
  broken connection. Reported by @petrn (#25).

## [0.33.0] - 2026-08-24

### Added
- **The NANO and the Guition boards get their rev 3.x images.** The ESP32-P4 ships
  in two silicon families, an image built for one will not start on the other, and
  the same product code arrives as either - so a NANO bought in August came up as
  v3.1 and esptool refused every release we had. Both boards now build and publish
  a `-rev3` image beside the plain one, each with its own update slug so a device
  is never offered the other family's build. The browser flasher offers the choice,
  and [docs/FLASHING.md](docs/FLASHING.md) says how to ask a board which chip it
  has. Reported by @levrskn (#24).

## [0.32.0] - 2026-08-23

### Added
- **Settings can be saved to a file and loaded back.** Two buttons at the foot of
  Settings. The file is plain JSON with the firmware version and the board in its
  header, which covers both of the real cases: keeping a copy of a working
  configuration before restoring defaults, and bringing a second device up like
  the first. Passwords and keys are not in it - the device never serves them, so
  they cannot be - and loading a file leaves the device's own identity alone:
  hostname and static addresses stay, because two devices answering to one name
  make two certificates a browser trusts neither of. What a file would change is
  shown before anything is written, and what was skipped is said afterwards
  rather than a bare "done".

## [0.31.1] - 2026-08-23

### Fixed
- **The console no longer looks frozen while the device writes an update.** The
  middle step - the device writing and verifying the image on its own - has no
  bytes to report and only a clock to go by, and nothing was ticking it. So the
  animation and the "of about 12s" line sat still for the whole write and only
  came alive at the reboot. The watch runs its own clock now. Reported by
  @petrn (#23).

## [0.31.0] - 2026-08-23

### Added
- **The hotspot shows a join code you can point a phone at.** While the rescue
  hotspot is up, the panel takes a turn showing the network and its password as
  a QR code, then a turn showing them as text. It only appears where the glass
  can hold a code big enough to read: 128x64 shows it at two pixels a module,
  64x48 and 72x40 at one, and the shorter panels keep the text they had.

### Fixed
- **An update takes on the first try again.** The chip could fault part-way
  through its own restart, and the boot that followed read the new firmware wrong
  and got nowhere. A few seconds later the watchdog reset the board properly, the
  old version came back, and from outside it looked like the update had reverted
  itself. The fault is in ESP-IDF's restart path on chips with external RAM - it
  never needed an update to happen, a plain restart could do it too - and it is
  gone in ESP-IDF 6.1, which the releases are built with now. Updating to this
  version is enough - no cable needed - but the restart that installs it is still
  done by the old firmware, so if the device comes back on the old version, run
  the update once more. From this version on it stops happening. Found from a
  serial log by @petrn (#22).
- **An upload no longer gives up when the device is busy.** Over HTTPS a quiet
  stretch on the socket arrives as a raw mbedTLS "nothing yet", not the timeout
  the code watched for, so it was read as a broken connection and the whole
  upload was thrown away. Live video can starve a socket for half a minute, so
  updating with the console open could die at a few percent. A silence is now a
  silence - in the firmware update, the image and rescue uploads, and every body
  the server reads.
- **The console scrolls again inside the phone's hotspot sheet.** Android shows a
  captive portal in a WebView wrapped in its own pull-to-refresh, and that decides
  whether to take a downward drag by reading the document's scroll position - not
  the panel the finger is on. A page sitting at the very top always looked
  refreshable, so a drag up inside Settings reloaded instead of scrolling, and
  nothing the page did could argue: the decision is made outside the WebView. The
  page now rests one pixel down, which is invisible and answers the question the
  other way.
- **The hotspot page says what that sheet is.** It is not a browser and handles
  the console poorly, so the page now points at the address to open properly -
  and warns that the phone must be told to stay on a network with no internet.
- **The touch controls no longer paint over the settings.** They carried a
  stacking order and the panel did not.
- **The diagnostics and target-OS popups stay on screen on a phone.** They opened
  beside their button, which is right when the rail is a column down the side and
  wrong once it is a row across the top - they went up and to the right, off the
  glass both ways. On a narrow screen they are centred instead.

## [0.30.0] - 2026-08-22

### Added
- **The status OLED no longer has to be 128x64.** SSD1306 panels also work as
  128x32, 96x16, 72x40, 64x48 and 64x32, and SH1106 as 128x32, 96x16 and 64x48.
  The display setting lists panels by controller and size together, so pick the
  line that matches yours: the same chip drives every size and reports none of
  it, so this is the one thing that cannot be detected. Devices already set up
  keep the panel they had.

  A shorter panel simply shows fewer lines, and it drops the least useful ones
  first - the address stays, the uptime goes. A line too long for the glass now
  steps along a character at a time rather than being cut off, and the screen
  waits for it before moving on - a clipped address looks like a real one, which
  is worse than a slow one.

- **The status screens got a proper header.** The screen's name and icon now sit
  in a reversed bar, with a dot for each of the three screens so it is clear
  which one is showing and how many there are. On the health screen the readings
  line up in a column beside their bars.

## [0.29.0] - 2026-08-22

### Added
- **You can watch an update happen now.** It takes over the console: the steps,
  the one it is on, and a fan of rays filling as it goes. Writing the image and
  restarting have nothing to measure, so those fill against how long they
  usually take and then turn into a sweep instead of sitting full. Restarting
  from Settings and switching the network show the same.
- **And the console says which version came back.** A restart ends the session,
  so an update that rolled back used to look exactly like one that worked. Now
  it says "came back on 0.28.0, not 0.29.0". Asked for by @petrn (#23).

## [0.28.1] - 2026-08-22

### Fixed
- **An update that starts and then dies no longer leaves a device you cannot
  reach.** The bootloader can undo an update that never comes up, but that
  protection ends the moment the new image confirms itself - which it does as
  soon as the console answers, because from there it can be re-flashed. Anything
  the firmware starts after that point was outside it, so a crash in, say, the
  MQTT or capture path left a confirmed image failing over and over, and
  power-cycling changed nothing: every boot failed the same way.

  The device now counts crashes that never got anywhere. Four in a row without
  once staying up for a minute, and it starts the other slot instead. Presses of
  the reset button and power cuts do not count - only real crashes - so nobody
  gets a downgrade for being impatient. Reported by @petrn (#22).
- **The MQTT state payload no longer sits on the timer task's stack.** It grew to
  576 bytes in 0.28.0 when the screen alert joined it, on a task that runs on
  about 3.5 KB. It lives in one static buffer now.

## [0.28.0] - 2026-08-21

### Added
- **The device can watch the screen when nobody is.** Give it a phrase or two -
  `no boot device`, `kernel panic` - and it reads the screen once a second with
  the console closed, raising an alert the moment one of them appears and
  clearing it when it goes. Both edges go in the log, and Home Assistant gets a
  "Screen alert" sensor with the matched text, published within two seconds
  rather than at the next state interval. Off by default; while it is off, and
  whenever the target is not showing text, it costs nothing - the resolution is
  checked before the frame is touched at all.
- **EDID profiles that cap the target's resolution.** Alongside the full mode
  list there are now `720p` and `1024x768`, which advertise everything up to that
  and prefer it, in both halves of the EDID - the extension block is the usual
  place a source looks for 1080p, so a cap that only trimmed the first block
  would not be a cap at all. A smaller picture encodes faster and costs less bandwidth, which
  on pre-3.0 silicon is the difference between 7 fps and something a person can
  work in. Text modes stay on offer in every profile, so a BIOS still arrives as
  text. This replaces the `custom` choice, which never did anything but fall back
  to the full list - a device set to it comes up on `720p`, so check the setting
  if you had picked it.
- **The Pins tab shows the board's expansion header, not just a list of GPIOs.**
  A GPIO number says nothing about where to put a wire, and half of the chip's
  pins never reach a connector, so the tab now draws the header as it is printed
  on the board - two columns, pin numbers down the middle where the board has
  them - with what holds each pin. The old list of every usable GPIO is still
  there, one click away, because it answers the other question: what is free.

  Pinouts are in for the Waveshare ESP32-P4-ETH, PoE and NANO boards, the
  Espressif Function EV and the Guition ESP32-P4-M3-Dev. They come from the
  vendors' own diagrams, and the tab says so - check yours against the silkscreen
  before wiring anything.

- **The screen can be read back as text when the target is showing text.** A BIOS
  setup, a UEFI boot menu, memtest, a Linux console: press Select and sweep the
  mouse over the picture as if it were a page, or press Copy and take the whole
  screen. Until now the only way to get a serial number or an error code off a
  BIOS screen was to type it out by hand.

  It is not OCR, and that is the point. A text screen is drawn by a character
  generator - a fixed grid, a fixed bitmap per character - so each cell is looked
  up in a table of the shapes the font has. Either a cell matches and the
  character is certain, or it comes back as `�` and you can see exactly which
  characters were not read; there is no "recognised with errors" to catch you out
  later. It costs one pass over the frame, only in character modes, and only once
  the picture has stopped moving - a firmware that paints its setup as a picture
  has no grid and is left alone.

  Three fonts are known: the 8x16 one a legacy BIOS draws with, the 8x16 one a
  Linux console draws with - which is a different font, differing in five
  printable characters including f and v - and the 8x19 one a UEFI console draws
  with. So a 720x400 setup screen reads, and so do a 1024x768 UEFI boot menu and
  a Linux virtual console, with the text area centred the way a firmware console
  centres it.

  Narrow screens are read as they come. A wide one - a UEFI console filling a
  1080p display is 240 characters across - is read only while you are asking for
  it, because looking at every settled 1080p frame on the off chance it is text
  costs four times as much and would nearly always find a desktop. Press Select
  or Copy and it reads that screen too; the first reading takes about a second,
  because a picture has to hold still before it can be read.
- **Three more layouts for pasting text: Czech, Ukrainian and Lithuanian.**
  Pasting types text out as keystrokes, so it has to know where each character
  sits on the target's keyboard. A character the chosen layout cannot type is
  reported rather than guessed at. The tables are now checked on every push -
  against each other, and against the X keyboard database.

## [0.27.1] - 2026-08-20

### Fixed
- **The PoE and rev 3.x images now get their manifests.** They have been built and
  attached to every release since 0.26.0, but the step that publishes the manifests
  still carried the list of four boards it was written with. So those three images
  shipped with an update check pointing at a URL that was not there, and the
  browser flasher had nothing to offer them from. Nothing in the firmware itself
  changed in this release.

## [0.27.0] - 2026-08-19

### Fixed
- **The status display now comes up in the first seconds of a boot.** An I2C OLED
  shares the capture chip's bus, and that bus was only made once capture started -
  so the panel stayed dark for the first fourteen seconds. That covered the whole
  password-reset window, which is exactly when someone is standing at the device
  looking at it.

### Added
- **The hotspot's password on a mono OLED.** A 128x64 panel is too small for a
  scannable join code, so it prints the network's key as text instead - enough to
  type it into a phone. The round LCD keeps showing the QR code.

## [0.26.0] - 2026-08-19

### Added
- **The round LCD's pins now default per board.** Which GPIOs are free is not the
  same on every board, so the console offered five numbers that were right for
  one of them and wrong for the rest. The NANO's come from @DaveDavenport, who
  worked out which pins that board can actually spare. A device that has already
  been set up keeps its own values - a default is only read when nothing is
  stored.
- **Builds for rev 3.x silicon, beside the existing ones.** The two ESP32-P4
  revisions need different images and neither will start on the other's chip -
  the header carries a minimum *and* a maximum. Espressif is still ramping rev
  3.x up, and a product code does not say which chip is in the box, so both are
  published: `p4-eth` and `p4-eth-rev3`, `p4-poe` and `p4-poe-rev3`. Check the
  boot log, which prints `Chip rev:`. On rev 3.x you also get the faster capture
  path and a writable microSD.
- **A build for the Waveshare ESP32-P4-WIFI6-POE-ETH.** The first supported board
  that takes PoE, so a KVM in a rack needs one cable instead of two. Its Ethernet,
  microSD, WiFi co-processor and button turned out to sit on the same GPIOs as the
  boards already supported, so the overlay is mostly inherited. Configured from the
  published schematic and not yet run on one - the same footing the NANO and
  Guition targets started from.

## [0.25.1] - 2026-08-18

### Added
- **Every boot now says how the last one ended** - power on, a reset by hand, a
  panic, a watchdog, or a brownout - and which slot is running and whether it is
  confirmed yet. When an update is rolled back there is otherwise nothing left
  to say why: by the time anyone looks, the old firmware is running again. These
  are four very different faults and the chip already knows which one it was.

## [0.25.0] - 2026-08-18

### Fixed
- **Keyboard and mouse worked again after the log endpoint took their route.**
  The web server has a fixed table of routes; registration past the end fails
  silently, and the input WebSocket is registered last, so adding one endpoint
  cost the whole control channel while every other page still answered. The
  table is bigger now, and a route that does not fit says so in the log instead
  of vanishing.

### Added
- **The device keeps its own log, and the console can hand it to you.** A ring in
  RTC memory that a restart does not clear - so the log of a boot that failed is
  still readable after the device has come back up on something that works, which
  is the one case a serial cable used to be the only answer to. Diagnostics &rarr;
  Download the log. It holds no passwords or keys; it does name your network,
  addresses and MAC, and the console says so where you download it.
- **The update shows itself on the status screen.** Percentage while the image
  arrives, then verifying, then restarting - and what went wrong if it did.
  An update is the longest thing the device does with nothing to show for it,
  and the console has just been asked to give up its video connection for the
  duration, so whoever is standing at the box has somewhere to look. Works on
  the mono OLED as well as the round LCD: by then the panel is attached.

## [0.24.0] - 2026-08-18

### Added
- **Scan to join the rescue hotspot.** When the device is running its own hotspot,
  the round LCD gives its whole face to a QR code: point a phone at it and it
  joins, no squinting at a passphrase. The hotspot exists precisely because the
  device is otherwise unreachable, so the panel is the only thing that can hand
  you the credentials. The small mono OLEDs sit this one out - a code that size
  is not something a phone can focus on.
- **The certificate now names the address DHCP gave the device.** Typing the IP
  used to land on an untrusted page, because only a static address was ever put
  in the certificate. You could click through for the console, but not for the
  video: a browser refuses a `wss://` stream to a mismatched certificate and
  offers no way to accept it. The address is recorded when it arrives and named
  from the next restart, exactly as the IPv6 addresses already were.
- **The board button now tells you it heard you.** Holding it to reset a forgotten
  password used to be a gesture into the void: no light, no message, no way to
  tell whether the button was even wired. The panel now fills a ring around its
  rim as you hold, empties it if you let go early, and says what it cleared when
  it fires.

### Fixed
- **A WiFi network that disappears no longer locks you out for good.** Choosing
  WiFi holds the wired port down, so if the network moved or changed its password
  the device became unreachable by every route at once - and the one setting that
  would fix it lived in the console you could no longer open. The button reset
  always claimed to put you back on Ethernet; it never actually did. Now it does,
  on that very boot.
- **Hold the button AFTER the reset, not through it.** Every instruction we
  shipped said to hold it down through a power-on. On boards where that button is
  also the chip's download strap - the ESP32-P4 Function EV among them - that
  drops the chip into firmware-download mode instead: nothing boots, and the board
  looks dead. The window is polled seconds into start-up, so holding it through
  the reset never helped anyway.
- **The round LCD no longer starves the rest of the device.** Its framebuffer
  lives in PSRAM but was not cache-aligned, so the SPI driver could not send it
  where it lay and copied all 115 KB into internal memory for every frame - of
  which this chip has about half a megabyte in total, shared with TLS, USB, the
  network stack, the SD card and the video encoder. The panel logged a failure a
  second, and the H.264 encoder could not get its reference frame: no picture in
  the browser, from a display setting. The buffer is aligned now and goes out in
  bands, so a single frame can never take the device down with it.
- **A picture instead of a black rectangle when H.264 cannot start.** The
  hardware encoder needs one contiguous multi-megabyte block for its reference
  frame, and after a long uptime PSRAM can be half free and still not have one.
  The encoder is rebuilt whenever the input resolution changes, so that is when
  it bites. It used to retry on every captured frame - thirty times a second,
  forever, logging the same error each time - while the viewer watched nothing.
  Now it says so once, with the actual memory figures - internal and PSRAM,
  free and largest block, because "out of memory" without them sends you
  measuring the wrong heap - and carries on in MJPEG until the next restart.
  Your codec setting is left alone.
- **The video stream steps aside during a firmware update.** The device serves
  the console, the stream and the upload from one small pool of connections, and
  writes flash with the encoder running. Reported as updates that fail and then
  work on the second or third try (#19 petrn).
- The round LCD no longer runs long text off its edges. A 15-character IP address
  wants more room than a 240-pixel circle has, and the overflow was silently
  clipped rather than shrunk.
- The console no longer offers pins that are already spoken for: the six SDIO
  lines to the WiFi co-processor, its reset line, and the pin that carries SD
  power on the Function EV. Picking one of those used to kill WiFi with no
  explanation - and it was the display driver's own default data/command pin,
  which is why a panel wired there stayed dark.

## [0.23.0] - 2026-08-17

### Added
- **IPv6.** The device now answers on IPv6 as well as IPv4, on Ethernet and on
  WiFi. Nothing to fill in - the address comes from the router - and
  `espkvm.local` resolves to it too. The certificate learns the address and names
  it from the next restart, so `https://[address]/` is trusted the same way the
  v4 one is. There is a switch in Settings &rarr; Network if you would rather the
  KVM stayed off IPv6.
- **Tap the network icon** in the footer to see everything about the connection
  in one place: what it is connected by, the name it answers to, its IPv4 address,
  every IPv6 address with what each one is good for, and the MAC you would put in
  a DHCP reservation. Each one is a link and has a Copy button, because nobody
  retypes an IPv6 address.
- **A network with no IPv4 at all now works.** Tailscale and WireGuard used to
  wait for an IPv4 address that was never coming; over WiFi, `espkvm.local` was
  never announced for the same reason; and the little status screen showed a
  blank address. All three now take the IPv6 route. Wake-on-LAN genuinely cannot
  work there - a magic packet is an IPv4 broadcast, and IPv6 has none - so it
  says so and hides the button instead of failing quietly.
- The device now appears in network discovery under **its own hostname** rather
  than as one of several identical "ESP-KVM" entries. Only the name you chose is
  published: no firmware version, no board model. mDNS is shouted at every device
  on the link, and a version number there is a free list of which bugs apply.

## [0.22.1] - 2026-08-13

### Fixed
- **Video no longer falls apart while you watch.** The frame pump polled with a
  5 ms delay that, at this firmware's 100 Hz tick, rounds down to zero - a busy
  loop that starved the chip whenever a viewer was connected: stutter, artefacts,
  and eventually a stream only a reboot would bring back. The pump now sleeps on
  the frame signal itself, which also shaves a little latency.
- **A heavy screen change repairs itself at once.** Opening a window is a burst of
  large frames; when the sender falls behind and skips one, the H.264 picture
  shatters until the next keyframe. The device now notices the skip and asks for a
  keyframe immediately - a blink instead of a two-second smear.
- A viewer that stops reading - a closed laptop lid, a dead link - is skipped and
  then dropped, instead of slowing the stream for everyone else.
- The update popup showed its percentage in three places at once; now it's one bar
  and one line. It also says which slot the update will install to (the inactive
  one - the running slot is untouched until the new image verifies).

## [0.22.0] - 2026-08-12

### Added
- **See both firmware slots - and boot the other one.** The firmware panel now
  lists both app slots with the version on each, which one is active, and whether
  it's confirmed or still unconfirmed. A "Boot this" button switches onto the other
  slot and restarts - handy for dropping back to a known-good image without
  reflashing. A slot with no valid image is refused, so a click can't strand the
  device.

### Fixed
- **The Function EV board boots cleanly again** (and the NANO/Guition targets).
  esp-hosted 3.0 moved its boot-time init, so the guard that keeps the WiFi
  co-processor off the shared SD bus in Ethernet mode quietly stopped working: the
  onboard C6 grabbed the bus, the microSD fought it for the card, and the board took
  ~30s to start while logging an endless SDIO error. It's back to a few seconds.
  Boards with no co-processor (the Waveshare P4-ETH) were never affected.
- **A glitchy H.264 picture repairs itself.** The browser's video decoder can
  silently stall - the image fills with artefacts and even a keyframe won't clear
  it. The console now spots that no decoded frames are coming out, rebuilds the
  decoder and resyncs on the next keyframe instead of needing a reload; if it still
  can't recover, it falls back to the MJPEG stream.
- A just-installed over-the-air image is now confirmed before the optional network
  features (MQTT, WireGuard, Tailscale) start, so a hiccup bringing one of those up
  can't leave a perfectly reachable update unconfirmed and bound for rollback.

## [0.21.2] - 2026-08-12

### Fixed
- **Pressing `f` no longer flips the browser to fullscreen while you're typing.**
  The console's `f`-for-fullscreen shortcut fired even inside a text field, so
  typing an `f` into the login password - right after an OTA update or a reboot,
  before you're logged in - toggled fullscreen instead of entering the character.
  Most visible in Safari, where it looked like an unasked-for Cmd-F. The shortcut
  now stays out of the way while a field is focused or a modifier is held. Thanks
  to the reporter in [#16](https://github.com/orgs/espkvm/discussions/16).

## [0.21.1] - 2026-08-11

### Fixed
- **An update now tells you where it is.** The bar used to fill up and then sit
  there while the device quietly wrote and verified the image, so a working update
  looked like a hung one ([#13](https://github.com/orgs/espkvm/discussions/13)). It
  now names each step - downloading, uploading, writing, restarting - waits for the
  device to answer again, and reloads the console onto the new firmware by itself.
  If it doesn't come back, it says so instead of leaving you guessing.
- An update that fails now leaves the reason on screen in the firmware panel, rather
  than in a toast that fades while you're watching the progress ring. A file that
  clearly isn't a firmware image is turned away before the upload starts.
- A dropped connection on the last byte of an upload is no longer reported as a
  failure - that's what the device restarting looks like from the browser, so the
  console waits it out and checks.

## [0.21.0] - 2026-08-11

### Added
- **A little status display.** Solder on a small OLED (SSD1306 or SH1106,
  auto-detected on the capture I2C bus) or a round GC9A01 colour LCD, switch it on in
  Settings, and the device shows its IP, link, capture status and health right on the
  panel - a boot logo first, then cycling pages with icons and temperature/RAM bars.
  Off by default, and it stays out of the encoder's way so streaming is unaffected.
- **Assign GPIO pins from the console.** Pin settings are now drop-downs of the free
  GPIOs (plus "None"), and a new Pins tab shows the whole map at a glance - what the
  board reserves, what you've assigned, and what's free. The ATX pins live on that map
  now too, so they can't quietly collide with anything else.

### Fixed
- Scrolling the settings page no longer changes a setting when the pointer happens to
  land on a drop-down - which could silently flip the display type and blank the screen.
- The round LCD now shows the Tailscale address as well, matching the OLED.

## [0.20.0] - 2026-08-10

### Added
- **Virtual media now serves the right kind of drive on its own.** An `.iso` is
  presented to the target as a CD-ROM, so installers boot and mount as an optical
  drive; anything else comes up as a removable disk. Nothing to set - though you can
  still force CD-ROM or disk if a file is misnamed. Switching between the two re-plugs
  the USB drive for you.
- **Hand the whole microSD card to the target.** A new "Whole microSD card" item in
  the Media panel exposes the entire card as a USB drive - every file, not one image.
  On rev-3.x boards it's read-write, so the target can copy files onto it; while the
  card is handed over the console steps off it so there's a single owner, and re-reads
  it when you switch the medium back (a reformat by the target is fine).
- **Uploads show throughput and a time estimate**, so a slow multi-gigabyte write to
  the card visibly moves instead of looking hung.

### Changed
- **The Media panel is now the one place to pick and turn on virtual media** - it has
  its own on/off switch (with the restart reminder), the rescue slot shows its size,
  and the duplicate "Mounted image" field is gone from Settings.

## [0.19.2] - 2026-08-09

### Fixed
- ATX power control could never be turned on: the settings that configure it (the
  button GPIOs and the enable toggle) were hidden until ATX was already working -
  which needed those very settings. A deadlock. The Power settings are now always
  visible, so you can wire up the optocouplers, set the GPIOs, and enable it. Thanks
  to marcopompili for the report.
- OTA updates sometimes finished but didn't reboot on their own (issue #13, thanks
  petrn). The restart was fired inline from the request handler right after the
  upload, where a blocked response send or an already-closed connection could swallow
  it; it now runs on a short timer and reliably reboots once the new image is written.

## [0.19.1] - 2026-08-07

### Changed
- microSD is now **writable on rev-3.x boards** - you can upload and delete images
  from the console. The write path was there all along but gated off, because the
  older ESP32-P4 stepping times out on SD writes; rev 3.x handles it fine (verified
  on hardware). Pre-3.0 boards stay read-only.

### Fixed
- A Linux machine that re-reads a USB string descriptor (an ASUS NUC on Ubuntu
  re-reading its serial number) was being detected as a Mac. The OS guess now also
  checks that the langid is requested last - the macOS tell - so Linux reads as Linux.

## [0.19.0] - 2026-08-07

### Added
- **Headscale (self-hosted Tailscale).** Point the device's native Tailscale client
  at your own Headscale or Ionscale control server instead of Tailscale's cloud -
  just a control-server URL (and optional port) in the VPN settings.

### Changed
- The target now sees the "monitor" as **ESP-KVM** instead of the capture chip's
  Toshiba name.

### Fixed
- The Waveshare ESP32-P4-NANO and Guition ESP32-P4-M3-Dev now build for pre-3.0
  silicon by default. The units a contributor actually tested turned out to be
  pre-3.0, and the 0.18.0 images (built for rev 3.x) wouldn't boot on them. Capture,
  USB and Ethernet are confirmed working on both; rev-3.x owners can flip a config
  for the faster H.264 path.
- The PWA install splash was blurry on Android - the icons were marked maskable
  without any padding, so it zoomed into the logo. They're plain "any" now, so it
  renders crisp.

## [0.18.0] - 2026-08-06

### Added
- Draft build targets for two more boards: the Waveshare ESP32-P4-NANO and the
  Guition ESP32-P4-M3-Dev. Both are configured from their datasheets and haven't
  been run on hardware yet - in theory they should work, but a few pins still need
  confirming. They show up in the browser flasher labelled "configured, untested".

### Changed
- Clearer firmware download names: the Waveshare ESP32-P4-ETH build is now
  `p4-eth` (was `waveshare`), since more boards are on the way. Devices already in
  the field keep updating as before.

## [0.17.0] - 2026-08-06

### Added
- **WiFi**, on boards with an ESP32-C6 (like the ESP32-P4 Function EV). The P4 has
  no radio of its own, so it talks WiFi through the C6; the Waveshare board is
  unaffected. Pick your link in the new "Connection" setting - Ethernet, WiFi, or the
  device's own access point - and swap it any time from the network pill in the status
  bar. One P4 caveat: the microSD and the C6 share a bus, so virtual media only works
  in Ethernet mode.
- **Rescue hotspot for WiFi.** If the device can't reach its network, it can also put
  up its own hotspot (`ESP-KVM-xxxx`) so you can still get to it and fix things -
  meanwhile it keeps trying the network and reconnects on its own once it's back.
  Turn it on with the new "If WiFi can't connect" setting.
- **Captive portal on the hotspot.** Join the device's access point and your phone
  opens the console on its own, like hotel WiFi. Over the hotspot the console runs on
  plain HTTP (a captive browser won't accept the self-signed cert), so H.264 is off
  there but MJPEG and settings work; Ethernet and WiFi keep full HTTPS.

### Changed
- **1080p H.264 is faster on rev 3.0+ silicon** - roughly 15 -> 22 fps. A third
  capture buffer keeps the encoder fed instead of waiting on the camera, and dropping
  some buffers the direct path never used freed the memory for it.
- **rev 3.0+ now captures YUV422 straight into both encoders**, skipping the
  colour-convert pass. Frees ~4 MB of PSRAM and puts H.264 and JPEG on one format; the
  older rev <3.0 path is untouched.

### Fixed
- **Two devices on the default hostname could clash on certificates** - their
  self-issued CAs had the same name, so trusting one made the browser reject the other
  (`ERR_CERT_AUTHORITY_INVALID`). Each CA now carries a per-device suffix and is
  re-issued automatically, nothing lost.
- **The rescue hotspot was hard to join while WiFi was hunting for its network** - the
  constant channel scanning kept knocking the hotspot off its channel. It now paces the
  retries so the hotspot stays put.
- **A fresh clone didn't build** - `sdkconfig.defaults` didn't set the chip, so the
  build fell back to `esp32` and failed on a missing toolchain. It now targets
  `esp32p4` out of the box.

## [0.16.5] - 2026-08-05

### Fixed
- At high resolution the free-running CSI capture DMA could overwrite the frame a
  codec was still reading and tear it: an encode takes longer than one capture
  period, so the two-buffer ping-pong came back around mid-read. The capture now
  holds the frame being encoded out of the DMA rotation (and drops intermediate
  frames to the driver's backup buffer) so the buffer under the encoder is never
  written.

## [0.16.4] - 2026-08-05

### Added
- Native Tailscale now chooses its home DERP relay region by latency instead of
  always relaying through the built-in default (Dallas). On first connect the
  device probes every region in the tailnet's DERP map and relays through the
  nearest, falling back to Frankfurt and then the default if none answer - a large
  latency win for tailnets reached from outside North America (microlink #19).
- The WireGuard settings show the device's own public key with a Copy button when
  WireGuard is selected, so it can be registered as a peer on the hub without
  digging it out of the raw `/api/v1/system/info`.

### Changed
- The pointer-mode setting dropped its "auto" choice, which did nothing (it
  behaved as "absolute"); it is now just Absolute / Relative.
- A tagged release's GitHub notes are taken from the matching CHANGELOG section
  rather than auto-generated commit summaries.

### Fixed
- WireGuard did not come up after a cold boot when its endpoint was a DNS name:
  the address was resolved before the network was ready and never retried. It now
  waits for the link (as Tailscale already did) and retries, so the tunnel
  establishes on its own.
- A key or mouse button could stay stuck down on the target when a USB report was
  dropped while the endpoint was momentarily busy (e.g. the host stalled the
  transfer): reports are retried, the release-all safety path can no longer be the
  one dropped, and a stale completion no longer makes the next report skip its wait.
- In an installed PWA the bottom action bar (and the top status bar) could sit
  under the phone's home indicator or notch and look missing - most visibly in
  dark theme. The bars now respect the safe-area insets.
- Repeated H.264 keyframe requests (one per reconnecting viewer) walked the GOP
  length down toward 2 over time, turning almost every frame into a keyframe and
  wrecking the bitrate. The keyframe toggle now stays around the configured GOP.
- Switching the video codec while a slow client still held a frame could leave
  later frames tagged with no payload type, so clients mis-decoded them until the
  next switch.
- The "Power LED active-high" setting never persisted: its NVS key exceeded the
  15-character limit, so it reverted to the default every reboot and could leave
  the reported power state inverted.
- The thermal guard could jump straight to stopping video if the warn and stop
  thresholds were set out of order; the warn threshold is now kept below stop.
- A virtual-media image of 2 GB or larger uploaded as an empty "success" (the
  length was truncated to a signed 32-bit int); large uploads now transfer fully.
- Hardened the control WebSocket against a zero-length frame and tightened a few
  internal bounds and authentication checks.

## [0.16.3] - 2026-08-04

### Fixed
- The USB status indicator (and the REST "no USB target attached" checks) could
  report no target after a warm reboot with the cable still attached, even though
  the keyboard and mouse worked: the readiness flag came from the USB mount event,
  which is missed if the target enumerated the device before the HID task
  registered its handler. Readiness now reads TinyUSB's own mount state directly,
  so the indicator matches reality without needing a replug.

## [0.16.2] - 2026-08-04

### Changed
- The VPN settings tab is a single Off / WireGuard / Tailscale selector that shows
  only the chosen backend's fields (and nothing when off), instead of listing every
  WireGuard and Tailscale field at once and making the operator scroll past the one
  they are not using. The tab is now just "VPN".

## [0.16.1] - 2026-08-04

### Fixed
- Enabling a VPN (Tailscale or WireGuard) before it was fully configured locked
  its own settings: an incomplete tunnel reported its capability as unavailable,
  which disabled the very fields - including the Tailscale auth-key box and the
  enable toggle - needed to finish setting it up, with no way back without a
  reflash. The VPN capabilities now track only whether the subsystem is present
  (always, here), so their settings stay editable; connection state is reported
  through the status API and the VPN pill instead. Secret settings (VPN keys) are
  also marked as such in the settings schema, so the console renders them as
  write-only password fields that keep the stored value when left blank.

## [0.16.0] - 2026-08-04

### Added
- Native Tailscale, as a second VPN backend alongside classic WireGuard (enable
  one in Settings -> VPN). The device joins a tailnet directly - no gateway, VPS
  or port-forward - and is reachable at its 100.x address (or MagicDNS name) from
  anywhere, with NAT traversal (DERP/DISCO) handled for it. Built on a native
  ts2021 client (microlink) ported to ESP-IDF 6. Set a Tailscale auth key and,
  optionally, a tailnet hostname; `/api/v1/system/info` reports the tailnet state
  (enabled/up/address/peers). The console's TLS certificate is re-issued to name
  the tailnet address and MagicDNS name, so it is valid when reached over
  Tailscale. Runs on its own worker task; bring-up never blocks boot.
- Installable PWA console. The web console ships a manifest, service worker and
  icons, so a phone can install it to the home screen and run it full-screen and
  standalone - which also removes the mobile browser chrome that shrank the
  usable area. Touch control is a proper relative trackpad with acceleration.
- The generated device CA is now named after the device (e.g. "espkvm ESP-KVM
  CA") so it is identifiable in a phone's trusted-credentials list, and `/cert.pem`
  is served as a CA certificate (`application/x-x509-ca-cert`, `.crt`). The
  console is served `no-cache` with a version ETag so a firmware update always
  delivers the matching console, and an open tab is offered a reload when the
  device is updated under it.

### Changed
- The classic WireGuard client now runs on the same bundled WireGuard stack as
  Tailscale, instead of a second, separate one. This removes a symbol clash so
  both can live in one firmware and be chosen at runtime; the WireGuard feature
  and its settings are unchanged for the operator.

### Fixed
- The self-signed certificate's serial number could be an invalid ASN.1 integer
  (a redundant leading zero) roughly one time in 256, producing a certificate
  some clients rejected outright. The serial is now always a valid positive
  integer.

## [0.15.0] - 2026-08-03

### Added
- Bring-your-own TLS certificate. An operator can install their own certificate
  and key (e.g. from an internal CA or a real public one) so the browser trusts
  the device without importing the device CA. `PUT /api/v1/tls/cert` takes one
  PEM blob (the certificate chain, leaf first, then the private key - exactly
  `cat fullchain.pem privkey.pem`); `DELETE` reverts to the self-signed identity;
  `GET /api/v1/tls` reports which is in use. The pair is validated (both parse
  and the key matches the certificate) before it is stored, and a bad upload can
  never strand the console: if the TLS stack rejects the certificate at start-up,
  the server falls back to the self-signed one.

## [0.14.0] - 2026-07-31

### Added
- WireGuard VPN client (Settings -> VPN), off by default. The device reaches a
  peer over the existing Ethernet link as a split tunnel - only its own tunnel
  address rides WireGuard, so the console stays reachable on the LAN too. It
  generates its own X25519 key on first use (via PSA) and reports the public key
  to add to the peer; the operator supplies the peer key, endpoint and tunnel
  address. Optional SNTP keeps the handshake timestamp valid across reboots.
  `/api/v1/system/info` reports the tunnel state (enabled/up/address/publicKey).
  Bring-up runs on its own worker task, so a slow or failing connect never blocks
  boot or the web server.
- Agent / computer-use REST endpoints, so an AI agent (or any script) can drive
  the target without the binary WebSocket protocol: `GET /api/v1/video/frame.jpg`
  for a single JPEG still, and `POST /api/v1/hid/move`, `/hid/click`, `/hid/key`
  and `/hid/type` for pointer and keyboard input. Coordinates are the raw HID
  range 0..32767; the caller maps screen pixels using the resolution from
  `/api/v1/video/status`.
- An "Agent REST API" setting (Security), off by default, that gates all of the
  above. These endpoints grant the same keyboard/mouse/screen control the console
  already has, over a simpler interface, so they exist only when the operator
  turns them on; each still needs a session and a USB target.

### Fixed
- The HID endpoints never block the web server. Each only enqueues to the HID
  worker and returns; the worker paces the reports over USB. Blocking the single
  server task (an earlier draft used a per-character/hold delay) stalls every
  other request and backs up the TLS handshake pool, which could hang the whole
  web server under load - the same reason the upload and MJPEG-stream paths run
  on their own tasks.
- Settings that do not depend on hardware - the whole MQTT/Home Assistant
  section and the agent-API toggle - are no longer hidden on a device without
  video capture. They had defaulted to requiring the video capability; they now
  declare themselves always applicable.

## [0.13.1] - 2026-07-31

### Added
- A connection indicator for the MQTT bridge in the status bar, next to the
  HDMI/USB/SD/Ethernet icons. It appears only when MQTT is enabled, and hovering
  it shows whether the device is connected to the broker or still connecting.
  The firmware reports this in `/api/v1/system/info` as `mqtt.enabled` and
  `mqtt.connected`.

### Fixed
- MQTT: turning the bridge off (or changing its settings) now publishes
  "offline" before disconnecting, so Home Assistant no longer shows the device
  stuck "online". A clean disconnect does not trigger the last will, so the
  retained availability had to be updated explicitly; an unexpected drop still
  falls back to the last will.

## [0.13.0] - 2026-07-31

### Added
- Home Assistant integration over MQTT. Turn it on and the device is
  auto-discovered as one Home Assistant device: sensors for temperature,
  viewers, video mode/frame rate/codec/bitrate, uptime, free PSRAM, HDMI
  signal, target USB and target power, plus buttons for power, reset,
  force-off, Wake-on-LAN and restart. Availability is tracked with a last-will
  topic. TLS is supported (verify against the built-in CA bundle, or skip it
  for a self-signed broker). Off by default and gated by the `mqtt_*` settings,
  so a device that never enables it only pays the linked code. esp-mqtt is a
  managed dependency now that it has left the IDF core in v6.

### Security
- The must-change-password state is enforced on the device, not only in the
  console: while the default password is still in force a session may reach
  only the auth endpoints (including the video/input WebSocket upgrade), so the
  device cannot be driven over the wire before a real password is set.

### Fixed
- ATX: a settings change can no longer reset a GPIO out from under a button
  press in progress (a dedicated operation lock serialises the two).
- The failed-login counter is now read and updated under the lock.
- Virtual media: the rescue-write flag is flipped only under the media lock on
  every path.

## [0.12.0] - 2026-07-29

### Added
- A "Restart device" button in Settings, so a reboot no longer needs a
  power-cycle or a curl call.

### Changed
- The H.264 path runs its colour conversion (PPA) and the encode on two tasks
  over two YUV buffers, so one frame encodes while the next converts. Measured
  on hardware this is a smaller win than hoped (~6.7 -> ~7.3 fps at 1080p): the
  bottleneck is PSRAM bandwidth, not serialised compute - the conversion moves
  ~9 MB per frame and running it alongside the encode makes the two contend for
  the memory bus. The real fix (feeding the encoder YUV directly, without the
  conversion pass) is a larger change tracked separately.

## [0.11.0] - 2026-07-29

### Fixed
- The WebSocket endpoints (`/video`, `/ws`) returned 404, so nothing that rides
  them worked in a browser - no H.264 video, no keyboard or mouse - even though
  every REST route was fine. The HTTP server's handler table was one size too
  small for the number of routes (the ATX endpoints added in 0.9.0 pushed it
  over), and `/video` and `/ws` are registered last, so they were the ones
  silently dropped. Raised the limit with headroom. This is why H.264 and input
  had stopped working in the browser since 0.9.0.
- The "JPEG quality" setting no longer shows "not probed yet" when the device
  booted on the H.264 codec: MJPEG is now probed at start-up like H.264, rather
  than only when its encoder first opens.

### Added
- The device is its own certificate authority. It generates a small root CA once
  (kept in NVS) and signs its server certificate with it; the CA is offered at
  `/cert.pem` (also on the plain port-80 server) and from Settings -> Security.
  Importing the CA into a client's trust store is what a self-signed *leaf* could
  never do - it makes the device genuinely trusted, which clears the browser
  warning and, being a secure context, enables the WebSocket channel and the
  H.264 decoder. The leaf is re-issued under the same CA when the hostname or a
  static IP changes, so the one-time import survives.

### Changed
- `/api/v1/video/status` reports the PPA colour-conversion time (`ppaUs`)
  separately from the encode time (`encodeUs`) on the H.264 path. Measured on
  hardware this showed the conversion, not the encoder, is the bottleneck
  (~104 ms vs ~45 ms at 1080p) - the encoder-load figure now covers both stages.

## [0.10.1] - 2026-07-29

### Changed
- The self-signed certificate now lists a configured static IP as a
  subjectAltName, so reaching the device by address no longer adds a name
  mismatch on top of the self-signed warning. The certificate is keyed by
  hostname and static IP together and regenerates when either changes; a device
  on DHCP is unaffected (its address can move, so it stays hostname-only) and
  keeps its certificate across the upgrade.

## [0.10.0] - 2026-07-28

### Added
- Touch mode: on a phone or tablet the screen becomes a trackpad instead of
  fighting the desktop pointer mapping. One finger moves the pointer, tap is a
  left click, two-finger tap a right click, two-finger drag scrolls, and a long
  press then drag holds the button. An on-screen keyboard types through the
  target's own layout, like paste does. Auto-detected from a coarse pointer,
  with a manual "Touch" toggle in the action bar.
- A clear "another session is in control" state. The device now grants control
  to the first client and holds it there, rather than letting each new frame
  silently steal the session; a second viewer sees a banner with a "Take
  control" button instead of dead input, and taking control demotes the previous
  holder to a viewer rather than disconnecting it.

### Changed
- The control WebSocket protocol gained a "take control" client message (0x07)
  and a "control state" device message (0x83); the console polls it so a viewer
  notices when the session is freed or taken over.

### Changed
- Numeric settings (GPIO pins, pulse lengths, thresholds) are now plain number
  fields instead of sliders - easier to set a value precisely.
- The Power panel hides its controls entirely when ATX control is switched off,
  rather than showing a disabled hint.
- The ATX wiring guide (`docs/wiring.md`) is board-agnostic, with a wiring
  diagram (`docs/atx-wiring.svg`) and only illustrative header examples.

## [0.9.0] - 2026-07-28

### Added
- ATX power control: the device can "press" the target's front-panel power and
  reset buttons through optocouplers, and read its power LED back the same way.
  A PC817 two-channel module on each side does the whole job with no custom
  board - see `docs/wiring.md`. New endpoints `POST /api/v1/power/click`,
  `/api/v1/power/hold` (a five-second hard off) and `/api/v1/power/reset`; the
  target's power state (when a LED is wired) appears in `/api/v1/system/info`
  under `atx`. A Power panel in the console drives it, with confirmation on the
  destructive actions.
- The GPIO pins, pulse lengths and drive/sense polarity are all runtime
  settings (Settings -> Power), so the same firmware runs on a board with no ATX
  wiring - it simply reports the capability unavailable with the reason - and a
  wrong guess about a module's trigger polarity is a checkbox, not a reflash.

Not yet verified against a real optocoupler: the firmware path (capability,
endpoints, GPIO pulsing, LED sense) is exercised on hardware, but the button
polarity and LED sensing are confirmed only once a module is wired.

## [0.8.0] - 2026-07-28

### Added
- Connection icons in the action bar - HDMI in, USB to the target, microSD and
  Ethernet - each lit by its live state (green when active, amber when the HDMI
  cable is up but there is no picture, dim when nothing is connected), so a
  glance shows what is plugged in.
- Ethernet link state (up/down and negotiated speed) is reported in
  `/api/v1/system/info` under `net`, and drives the Ethernet connection icon.

### Changed
- The status bar adapts to phones: on a narrow screen the secondary video stats
  (codec/fps, skipped frames, bitrate, encoder load) are hidden, keeping the
  online state, resolution, the version widget and the theme toggle; the full
  figures remain in the Video panel.
- The web console now lives in its own repository
  ([espkvm/console](https://github.com/espkvm/console)) and is pulled in as a
  git submodule at `web/`. Clone with `--recursive`; see the README.

## [0.7.0] - 2026-07-26

### Added
- Build target for the Espressif ESP32-P4 Function EV Board (chip rev v3.2,
  16 MB flash) as an sdkconfig overlay: `boards/funcev_p4.defaults` +
  `partitions_funcev.csv`, documented in `boards/README.md`. Chip rev <3.0 and
  >=3.0 are mutually exclusive targets, so boards are built separately; the
  default `idf.py build` still targets the Waveshare ESP32-P4-ETH (rev v1.3).
- CI now builds every board and publishes each one's release assets (the `.bin`
  filenames carry the board name) and its own Pages manifests under
  `firmware/<board>/` and `flash/<board>/`. The root `firmware/` and `flash/`
  still mirror the Waveshare board, so devices and flasher pages that predate the
  per-board layout keep working unchanged.
- The default update-manifest URL is now a build option (`KVM_UPDATE_URL`) so a
  board's build points its update check at its own manifest. The Waveshare
  default is unchanged (the root manifest); the Function EV build points at
  `firmware/funcev/`, so it never tries to install a rev <3.0 image.

### Fixed
- The "always powered" microSD configuration (`KVM_SD_PWR_GPIO=-1`, documented
  in PORTING.md) did not actually compile - a constant negative shift tripped
  `-Werror=shift-count-negative`. That path now builds.

## [0.6.0] - 2026-07-25

### Changed
- The firmware version is now a widget in the status bar: an outlined badge that
  shows a dot when a newer build is published and whose outline fills as a
  progress ring while an update installs. Clicking it opens the firmware panel -
  the update check, installing a release or a hand-picked `.bin` - which moved
  out of Settings so the whole flow is one click from the version.
- Live diagnostics (chip temperature, memory, uptime, ESP-IDF version) moved from
  the Settings System tab into a rail button above Settings: the button shows the
  temperature and a coarse uptime (55min, 1h, 3d, 2w, 5m, 1y), the rest opens in
  a popup beside the rail.
- The guessed target OS moved out of the action bar into its own rail button
  above Settings (next to the diagnostics one); clicking it shows the raw USB
  fingerprint. The popups open toward the stage on whichever side the rail is on.
- The image list, active-medium choice and uploads moved out of Settings into the
  Virtual media panel itself; Settings keeps only the media settings.
- The button rail and its panels now open on the same side, and a `Panel side`
  setting (Settings -> System) flips the whole unit left or right.

## [0.5.1] - 2026-07-25

### Added
- A progress bar while an image uploads - a firmware update, a microSD image or
  the on-flash rescue image - so it is clear the upload is moving, not stalled.

## [0.5.0] - 2026-07-25

### Added
- **User macros.** Save named key macros - a short script of key chords, typed
  text and delays (`key ctrl+alt+f2`, `type root`, `delay 500`) - in the Input
  panel and replay them with one click, for a fixed sequence like stepping
  through a BIOS or an unattended install. Stored on the device, so they follow
  it rather than the browser.

### Changed
- The virtual-media tab now points at netboot.xyz for where to get a rescue
  image, since the browser cannot fetch one cross-origin to install in one click.

## [0.4.0] - 2026-07-25

### Added
- **Wake-on-LAN.** The Power panel gains a Wake button that sends a magic packet
  to the target's MAC (set under Settings -> Power), to power on a machine that
  keeps standby power and has WoL enabled - no ATX wiring needed.
- **Static network addressing.** The Network tab's static IP, mask, gateway and
  DNS now take effect; DHCP stays the default. A malformed address falls back to
  DHCP at boot so a typo cannot strand the device, and the board button now
  reverts to DHCP alongside clearing the password, so a valid-but-wrong address
  is recoverable the same way a forgotten password is. (The hostname / mDNS name
  already applied.)

## [0.3.0] - 2026-07-24

### Fixed
- A network firmware update could come up reachable and then be rolled back on
  the next reset. The image was confirmed only after every peripheral had
  started, so a USB or capture block left in a bad state by the warm restart
  kept the confirmation from being reached. The image is now confirmed the
  moment the network and web server answer - the point at which the device is
  re-flashable - and the warm-reset-prone peripherals start afterwards, where a
  failure degrades the device instead of reverting it. Boot phases are logged
  so a future failure is diagnosable from the serial console.

### Added
- **Target-OS awareness.** The device guesses whether the target is Windows,
  macOS, Linux or Android from how it enumerates USB, and shows the guess (with
  the raw fingerprint) next to the USB status and in Settings. A `Target OS`
  setting overrides it when the guess is wrong or unknown.
- The console tailors input to that OS: it labels the Meta key Win, Cmd or
  Super, and offers OS-specific key combinations - the Linux magic-SysRq
  sequences **REISUB** (safely reboot a hung machine) and **REISUO** (safely
  power it off), and **Ctrl+Alt+F1-F6** to switch virtual terminals.
- **Built-in rescue media.** A small bootable image - iPXE, memtest, a DOS
  floppy - can be kept in a 4 MB flash partition and served to the target over
  the same USB drive as the card's files, with no microSD needed. Unlike the
  card, it can be written from the console, because this board's flash writes are
  reliable where its SD writes are not. Upload it in the virtual-media tab and
  pick it as the active medium; the card and the rescue image coexist.

### Changed
- The set of USB functions is now built into the descriptor at start-up instead
  of being fixed, so mass storage can be left out to free its endpoints (room a
  future USB-network interface can use). **Expose virtual media** now controls
  whether the USB drive is present at all and takes effect after a restart; with
  it off the device is a plain keyboard and mouse. Swapping the image once the
  drive exists still takes effect immediately.
- The partition table gains a 4 MB `rescue` partition, appended so every existing
  offset is unchanged. A device adopts it with a one-time full flash over cable
  (the browser flasher works); it cannot be taken on by an over-the-network
  update.

### Security
- Bumped the build-time dev dependencies (Vite, esbuild) to clear advisories.
  These are toolchain packages that never ship in the firmware, and the issues
  were dev-server-only, so there is no effect on the device - the update just
  keeps the tree clean.

## [0.2.1] - 2026-07-24

### Changed
- Board wiring is now configured in `menuconfig` instead of a hardcoded header:
  the microSD pins and slot power-gate, the TC358743 I2C pins and reference
  clock, and the BOOT button GPIO joined the Ethernet pins that were already
  there, all with the Waveshare ESP32-P4-ETH values as defaults. Porting to
  another ESP32-P4 board is now a menuconfig edit; set the button or SD
  power-gate GPIO to `-1` on a board that lacks them. No behaviour change on
  the reference board - the binary is identical. See `docs/PORTING.md`.

### Added
- `docs/PORTING.md` - adapting the firmware to another ESP32-P4 board.
- `AGENTS.md` - repo conventions, build/flash commands and hard-won gotchas,
  for contributors and AI coding agents.

## [0.2.0] - 2026-07-24

### Added
- **Virtual media.** A disk image on the microSD card is presented to the target
  as a read-only USB drive it can boot from - a rescue system, an installer, a
  live image. The console gains a media tab to list the images on the card and
  choose which one the target sees.

### Notes / known limits
- The card is served **read-only**: this board cannot write the microSD
  reliably, so images are prepared in an external card reader. Upload and delete
  in the console are disabled with the device's own explanation.
- The card must be **FAT32**; a single image is capped at 4 GB (a FAT32 limit).
- microSD reads run at 4 MHz (~1.5 MB/s). Higher clocks fail every multi-block
  read on this board - a known ESP32-P4 SD limitation - so booting a heavy
  graphical image is slow (minutes); a minimal rescue image boots in about a
  minute. The card mount is retried at boot; a marginal card may need reseating.

## [0.1.2] - 2026-07-23

Foundation releases. The core KVM: HDMI capture with automatic mode following,
MJPEG and hardware H.264 streaming, an absolute and relative USB pointer and a
full keyboard, the Vue web console, HTTPS with a self-signed certificate,
login with a physical password reset, thermal protection, and firmware update
over the network with rollback. microSD is mounted and reported (the base the
virtual-media feature above builds on).

## [0.1.1] - 2026-07-23

First tagged build: the release pipeline (GitHub Actions), the update manifest
on GitHub Pages, and the browser flasher.
