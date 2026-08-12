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
- ATX power control - power and reset buttons, and power-LED sensing, through optocouplers
- DHCP or static networking, mDNS
- Home Assistant integration over MQTT - auto-discovered sensors and power/reset/Wake-on-LAN buttons, with optional TLS
- VPN: classic WireGuard or native Tailscale (pick one, one shared WireGuard stack) - WireGuard is a split-tunnel client with on-device key generation; Tailscale joins a tailnet natively, reachable at its 100.x address (or MagicDNS name) from anywhere with no gateway or port-forward, and the console's TLS certificate is valid over the tailnet
- Installable PWA console - runs full-screen/standalone from a phone home screen; trackpad touch control
- HTTPS with a self-issued certificate, login, and a physical password reset
- Bring-your-own TLS certificate - install your own certificate and key in place of the self-signed one
- Thermal protection
- Over-the-network updates with automatic rollback
- A second board build target (Espressif ESP32-P4 Function EV Board)
- WiFi on boards with an ESP32-C6 (station or the device's own access point) - one link at a time, with a rescue hotspot and captive portal so a device whose network is out of range is still set up from a phone
- Faster H.264 on rev 3.x silicon - native YUV422 capture fed straight to the encoder; 1080p went from ~15 to ~22 fps

## Planned

- **HDMI audio** - capture the I2S audio from the bridge and stream it to the browser
- **IPv6**
- **More supported boards**

## Under consideration

- A screen recorder (save a clip of the target's output)
- Serial console passthrough
- A USB network interface to the target
- Yggdrasil - a decentralised overlay as an alternative to the WireGuard/Tailscale backends

Have a use case that is not here? Open a discussion.
