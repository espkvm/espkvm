# AGENTS.md

Guidance for AI coding agents (and new contributors) working in this repo. Keep
it accurate; update it when the build, layout, or conventions change.

## What this is

ESP-KVM is an IP-KVM firmware for the **ESP32-P4** with a **TC358743** HDMI-to-CSI
capture board: it captures the target's HDMI, presents itself over USB as a
keyboard, mouse and (read-only) disk, and serves the whole thing to a browser
over HTTPS. It is a heavily-rewritten fork of jrowny/p4kvm; only the TC358743
driver and the CSI-bridge programming are inherited. ESP-IDF 6.0.1.

## Setup, build, flash

```sh
. tools/env.sh                    # ESP-IDF 6.0.1 env for esp32p4 (installs via tools/install-idf.sh)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py menuconfig                 # settings live under the "ESP-KVM" menu
```

- The board is on **/dev/ttyACM0** (USB-Serial-JTAG). It is an ESP32-P4 rev
  **v1.3** (pre-3.0 - matters for H.264, see below).
- `sdkconfig` is gitignored; defaults come from `sdkconfig.defaults` + Kconfig.
- Board pins are Kconfig options (menuconfig -> ESP-KVM -> Board pins / Network /
  Video capture) with Waveshare ESP32-P4-ETH defaults. See `docs/PORTING.md`.

## Web console

The console is Vue 3 + Vite in `web/`, built to a single gzipped file that the
firmware embeds. **After changing anything under `web/`, rebuild it and then
rebuild the firmware** so the new bundle is embedded:

```sh
cd web
npm run typecheck
npm run build        # writes components/kvm_web/assets/index.html.gz
npm run dev          # local dev against a real device
npm run dev:mock     # local dev with a mock backend (no device)
```

## Layout

```
components/
  tc358743/       HDMI bridge driver (inherited; hard-won register sequence)
  video_pipeline/ CSI capture + MJPEG/H.264 encoders behind one frame store
  kvm_hid/        composite USB HID (kbd/abs+rel mouse/consumer) + MSC
  kvm_storage/    microSD mount + virtual-media (serves an image read-only)
  kvm_config/     NVS settings, capability registry, thermal guard
  kvm_web/        HTTPS server, REST + WebSocket, auth, self-signed TLS, OTA
  kvm_net/        Ethernet + mDNS
  kvm_board/      pin map (thin: names the Kconfig CONFIG_ values)
  esp_tinyusb/    VENDORED fork of esp_tinyusb (see gotchas)
web/              Vue 3 console
main/             app_main and start-up order
docs/             HARDWARE-NOTES.md (measured facts), PORTING.md
```

## Conventions

- **Comments say why, not what**, and match the density of the code around them.
  This codebase leans on prose to record hard-won reasoning; keep that.
- **No hard-to-type Unicode in source** (no em dashes, smart quotes, etc.). In
  HTML use entities (`&mdash;`, `&rarr;`). Plain hyphens in C/TS comments.
- New files carry the Apache-2.0 SPDX header used across the tree.
- **Don't rip out unfinished features - mark them disabled with a reason.** The
  capability registry (`kvm_config`) reports every feature as compiled (Kconfig)
  + available (probe) + enabled (setting); the UI shows a disabled control with
  the device's own reason rather than a control that fails. `net_static` and the
  disabled SD upload/delete are the examples to follow.
- Keep commits small and messages short and imperative, matching the log.
- Versions come from git tags (`v.0.X.Y`); a tag triggers the CI release. Record
  user-facing changes in `CHANGELOG.md` under `[Unreleased]`.

## Hard-won gotchas (don't relearn these)

- **esp_tinyusb is vendored** at `components/esp_tinyusb`, not pulled from the
  registry. It is a fork with its storage-backed MSC removed so ESP-KVM can
  supply its own `tud_msc_*_cb` serving a file; `CFG_TUD_MSC` is forced on in
  its `tusb_config.h`. Do not re-add the managed `espressif/esp_tinyusb`
  dependency, and do not expect its MSC storage APIs to exist.
- **mbedTLS 4 / PSA crypto.** IDF 6 dropped the legacy pk / ctr_drbg API. Keys
  come from `psa_generate_key` + `mbedtls_pk_copy_from_psa`; the X.509 writer
  takes no RNG callback. PBKDF2 and HMAC are hand-rolled over PSA (not public in
  mbedTLS 4).
- **microSD is marginal on the reference board.** Reads run at **4 MHz**
  (`host.max_freq_khz` in `kvm_storage.c`) because every multi-block read fails
  at 20 MHz; writes are disabled entirely (`kvm_storage_writable()` returns
  false) and the card is prepared in an external reader. Mount uses a retry
  loop; it never blocks start-up. A known ESP32-P4 SD limitation - see
  `HARDWARE-NOTES.md`.
- **H.264 needs HTTPS in the browser** (WebCodecs is a secure-context API) and,
  on this chip revision, an ISP colour-convert detour the encoder forces
  (rev < 3.0). It is a bandwidth win, not a frame-rate win.
- **OTA can roll back.** A network-flashed image occasionally comes up without
  the web server and the next reset reverts to the previous slot; USB-flashing
  the same image is reliable. Watch for it with a serial monitor attached.
- Don't ever reformat a card that won't mount - it may be the operator's.

## Verifying a change

- `idf.py build` clean, no new warnings; boot log has no `E (` from init.
- Test on real hardware where it matters (video, HID, TLS, virtual media). The
  device is attached to this machine; `curl -k https://espkvm.local/` reaches it.
- For the console, `npm run typecheck` and a look in a browser.
