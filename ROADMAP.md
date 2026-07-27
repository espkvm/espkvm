# Roadmap

Where ESP-KVM is going. This is a guide, not a promise, and the order can change.
Help toward any of it is welcome - see [CONTRIBUTING](https://github.com/espkvm/.github/blob/main/CONTRIBUTING.md),
and use [Discussions](https://github.com/espkvm/espkvm/discussions) for ideas.

## Done

- HDMI capture that follows the target's resolution automatically
- MJPEG and hardware H.264 streaming
- Absolute and relative USB pointer, full keyboard, media keys, text paste
- User-defined key macros
- Virtual media: boot the target from a microSD image or an on-flash rescue image
- Target-OS detection, with OS-specific shortcuts
- Wake-on-LAN
- DHCP or static networking, mDNS
- HTTPS with a self-issued certificate, login, and a physical password reset
- Thermal protection
- Over-the-network updates with automatic rollback
- A second board build target (Espressif ESP32-P4 Function EV Board)

## Planned

- **ATX power control** - power and reset through GPIO, with power/HDD LED sensing
- **HDMI audio** - capture the I2S audio from the bridge and stream it to the browser
- **WiFi** on boards that carry a wireless co-processor (e.g. the ESP32-C6)
- **Faster H.264 on rev 3.x silicon** - feed the encoder directly, without the
  colour-conversion detour that limits frame rate on the older revision
- **More supported boards**

## Under consideration

- A screen recorder (save a clip of the target's output)
- Serial console passthrough
- Bring-your-own TLS certificate
- A USB network interface to the target

Have a use case that is not here? Open a discussion.
