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
- Install any published release, including going back to an earlier one - the device fetches the image itself, so the history is not limited to the two slots on the board
- A second board build target (Espressif ESP32-P4 Function EV Board)
- WiFi on boards with an ESP32-C6 (station or the device's own access point) - one link at a time, with a rescue hotspot and captive portal so a device whose network is out of range is still set up from a phone
- Faster H.264 on rev 3.x silicon - native YUV422 capture fed straight to the encoder; 1080p went from ~15 to ~22 fps
- IPv6 alongside IPv4, autoconfigured from the router's advertisements, with the certificate naming the addresses
- A log kept on the device, surviving a restart and downloadable from the console - so a report of something that failed at boot can carry the evidence
- A status screen on the device itself (I2C OLED or round SPI LCD) - link, address, capture and health without a browser; on the round LCD also a scannable code for joining the rescue hotspot, and a ring that fills while the recovery button is held

## Planned

- **HDMI audio** - capture the I2S audio from the bridge and stream it to the browser
- **More supported boards**
- **A mouse jiggler** - a small move now and then so the target does not lock or sleep while you are watching it
- **Tell the console how the target's monitors are arranged** - an absolute pointer addresses the target's whole desktop, so a second display makes the pointer travel further than the mouse and half the picture aim at a screen you cannot see. Give the desktop's size and where the captured screen sits inside it, and the console maps into that part instead - absolute pointing that lands where it is aimed on a multi-monitor target, without falling back to relative
- **Install your own EDID** - the built-in profiles cover the common cases; this takes a binary from a monitor that works, for the target that only likes its own
- **GPIO relays and lamps** - the pins already drive the ATX buttons; this opens the rest of them, as buttons and indicators of your own in the console
- **Metrics for Prometheus** - the figures the console shows, in the format a monitoring system already reads

## Under consideration

- A screen recorder (save a clip of the target's output)
- Serial console passthrough - a terminal in the console, on a free UART or as a USB serial port the target sees
- A USB network interface to the target
- A VNC server - reach the target from any VNC client, no browser
- Serving a USB drive plugged into the device as the target's virtual media
- Yggdrasil - a decentralised overlay as an alternative to the WireGuard/Tailscale backends

Have a use case that is not here? Open a discussion.
