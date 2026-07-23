# Changelog

All notable changes to ESP-KVM are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning while it is pre-1.0 (a new feature bumps the minor, a fix
bumps the patch).

## [Unreleased]

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
