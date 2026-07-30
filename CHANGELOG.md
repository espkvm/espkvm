# Changelog

All notable changes to ESP-KVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning while it is pre-1.0 (a new feature bumps the minor, a fix
bumps the patch).

## [Unreleased]

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
