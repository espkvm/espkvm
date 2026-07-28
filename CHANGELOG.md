# Changelog

All notable changes to ESP-KVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning while it is pre-1.0 (a new feature bumps the minor, a fix
bumps the patch).

## [Unreleased]

## [0.9.1] - 2026-07-28

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
