/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The single source of truth for every user-visible setting. Adding a row here
 * is enough: it is persisted, validated, exposed over REST and rendered by the
 * settings panel without further code.
 *
 * NVS keys are limited to 15 characters.
 */
#include "kvm_settings.h"

#include "sdkconfig.h"

/* Derive an ENUM's .max from its choices array so the two can't drift
 * (the schema iterates choices[0..max]; a short array reads OOB). */
#define ENUM_MAX(arr) ((int)(sizeof(arr) / sizeof((arr)[0]) - 1))

static const char *const s_codec_choices[] = {"mjpeg", "h264"};
static const char *const s_edid_choices[] = {"full", "1080p30", "720p", "1024x768"};
static const char *const s_mouse_choices[] = {"absolute", "relative"};
static const char *const s_engage_choices[] = {"click", "hover"};
/* Must match the layout ids in web/src/layouts.ts. New ones go on the end:
   the value is stored as an index, so reordering would move everyone's setting. */
static const char *const s_layout_choices[] = {"en_us", "ru_ru", "cs_cz", "uk_ua", "lt_lt"};
static const char *const s_media_choices[] = {"auto", "cdrom", "disk"};
static const char *const s_log_choices[] = {"error", "warn", "info", "debug"};
static const char *const s_side_choices[] = {"left", "right"};
/* One line per panel the firmware can drive, controller and size together.
   Order must match k_panels[] in components/kvm_display/kvm_panels.c: the first
   three keep the values devices already have stored. */
static const char *const s_display_choices[] = {
    "SSD1306 128x64", "SH1106 128x64", "GC9A01 240x240",
    "SSD1306 128x32", "SSD1306 96x16", "SSD1306 72x40",
    "SSD1306 64x48",  "SSD1306 64x32", "SH1106 128x32",
    "SH1106 96x16",   "SH1106 64x48",
};
/* "auto" follows the OS guessed from USB enumeration; the rest force it. */
static const char *const s_targetos_choices[] = {"auto", "windows", "macos", "linux", "android"};
static const char *const s_netmode_choices[] = {"ethernet", "wifi", "ap"};
static const char *const s_fallback_choices[] = {"keep_trying", "hotspot"};

/* clang-format off */
static const kvm_setting_t s_settings[] = {
    /* ---- video ---------------------------------------------------------- */
    {
        .key = "vid_codec", .section = "video", .type = KVM_VT_ENUM,
        .title = "Stream codec",
        .help = "H.264 costs a fraction of the bandwidth of MJPEG on a screen that "
                "barely changes, and about a third of the frame rate. Browsers decode "
                "it through WebCodecs, which they offer on HTTPS pages only, so it is "
                "unusable over plain HTTP until TLS is in place.",
        .min = 0, .max = ENUM_MAX(s_codec_choices), .def = 0, .choices = s_codec_choices,
        .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "jpg_quality", .section = "video", .type = KVM_VT_INT,
        .title = "JPEG quality",
        .help = "Higher is sharper and larger. Only affects the MJPEG codec.",
        .min = 1, .max = 100, .def = CONFIG_KVM_JPEG_QUALITY, .requires_cap = KVM_CAP_MJPEG,
    },
    {
        .key = "h264_kbps", .section = "video", .type = KVM_VT_INT,
        .title = "H.264 bitrate (kbit/s)",
        .help = "Target bitrate for the hardware encoder.",
        .min = 500, .max = 20000, .def = 4000, .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "vid_fps_max", .section = "video", .type = KVM_VT_INT,
        .title = "Frame rate limit",
        .help = "Upper bound on encoded frames per second. Lower it to save bandwidth "
                "on a slow link.",
        .min = 1, .max = 60, .def = 30, .requires_cap = KVM_CAP_VIDEO,
    },
    {
        .key = "vid_adapt", .section = "video", .type = KVM_VT_BOOL,
        .title = "Skip unchanged frames",
        .help = "Stop sending while the target's screen is static. Drops the bitrate to "
                "nearly zero on an idle desktop. MJPEG only: H.264 already codes an "
                "unchanged screen as a frame of a few hundred bytes.",
        .def = 1, .requires_cap = KVM_CAP_VIDEO,
    },
    {
        .key = "edid_prof", .section = "video", .type = KVM_VT_ENUM,
        .title = "EDID profile",
        .help = "What this device claims to be, as a monitor - the target picks its "
                "output mode from what is offered here. \"full\" advertises the common "
                "modes from 640x480 up to 1920x1080@30. \"720p\" and \"1024x768\" stop "
                "lower, which is often what you want: a smaller picture encodes faster "
                "and costs less bandwidth. \"1080p30\" offers that one mode alone, for a "
                "source that refuses a list. Text modes stay on offer either way, so a "
                "BIOS still comes through as text.",
        .min = 0, .max = ENUM_MAX(s_edid_choices), .def = 0, .choices = s_edid_choices,
        .requires_cap = KVM_CAP_VIDEO, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "scr_watch", .section = "video", .type = KVM_VT_BOOL,
        .title = "Watch the screen for words",
        .help = "Read a text screen even when nobody is looking, and raise an alert "
                "when it says one of the phrases below. This is the case a KVM is "
                "bought for - a machine that fell over at three in the morning - so it "
                "keeps working with the console closed. Costs one pass over the frame "
                "a second, and only while the target is showing text.",
        .def = 0, .requires_cap = KVM_CAP_VIDEO,
    },
    {
        .key = "scr_match", .section = "video", .type = KVM_VT_STR,
        .title = "Phrases to watch for",
        .help = "Comma-separated, matched without regard to case, anywhere on a line - "
                "for example: no boot device, kernel panic, press F1 to continue. An "
                "alert is raised the moment one appears and cleared when it goes, and "
                "every phrase on screen is named, not only the first; the log records "
                "both, and Home Assistant gets a sensor if MQTT is on.",
        .def_str = "", .max_len = 255, .requires_cap = KVM_CAP_VIDEO,
    },

    /* ---- input ---------------------------------------------------------- */
    {
        .key = "target_os", .section = "input", .type = KVM_VT_ENUM,
        .title = "Target OS",
        .help = "Which machine's conventions the console follows - the label on the "
                "Meta key, and which OS-specific key combinations it offers. \"auto\" "
                "trusts the guess made from how the target enumerates USB, shown next "
                "to the USB status; set it by hand if that guess is wrong or unknown.",
        .min = 0, .max = ENUM_MAX(s_targetos_choices), .def = 0, .choices = s_targetos_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "mouse_mode", .section = "input", .type = KVM_VT_ENUM,
        .title = "Pointer mode",
        .help = "Absolute puts the target's cursor exactly where you click and is "
                "the right choice almost always. Relative is for software that "
                "captures the pointer, such as games and 3D viewers.",
        .min = 0, .max = ENUM_MAX(s_mouse_choices), .def = 0, .choices = s_mouse_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "ptr_engage", .section = "input", .type = KVM_VT_ENUM,
        .title = "Start controlling on",
        .help = "\"click\" waits for a click on the video before input reaches the "
                "target, and stops again on Esc or a click outside; the engaging click "
                "is still delivered. \"hover\" tracks the pointer as soon as it is over "
                "the video, the way a remote desktop behaves. Keyboard input always "
                "requires a click first.",
        .min = 0, .max = ENUM_MAX(s_engage_choices), .def = 0, .choices = s_engage_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "mouse_sens", .section = "input", .type = KVM_VT_INT,
        .title = "Relative sensitivity (%)",
        .help = "Scales pointer movement in relative mode only.",
        .min = 10, .max = 400, .def = 100, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "jiggle_s", .section = "input", .type = KVM_VT_INT,
        .title = "Mouse jiggler (seconds)",
        .help = "0 turns it off. Otherwise the pointer is nudged one pixel and put straight "
                "back this often, so the target does not lock or fall asleep while you are "
                "watching it. It stands aside whenever you are using the mouse yourself, and "
                "does nothing when no target is attached.",
        .min = 0, .max = 3600, .def = 0, .requires_cap = KVM_CAP_HID,
    },

    {
        .key = "scroll_inv", .section = "input", .type = KVM_VT_BOOL,
        .title = "Invert scroll wheel",
        .def = 0, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "kbd_layout", .section = "input", .type = KVM_VT_ENUM,
        .title = "Target keyboard layout",
        .help = "Used when pasting text, so the characters sent match what the target "
                "actually types. A KVM sends key positions, not characters.",
        .min = 0, .max = ENUM_MAX(s_layout_choices), .def = 0, .choices = s_layout_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "type_delay", .section = "input", .type = KVM_VT_INT,
        .title = "Paste keystroke delay (ms)",
        .help = "Raise it if the target drops characters while text is being pasted.",
        .min = 1, .max = 200, .def = 8, .requires_cap = KVM_CAP_HID,
    },
    {
        /* User-defined key macros, as a JSON array the console reads and writes.
         * The section is deliberately one the settings panel does not render, so
         * this stays a store rather than a text field a person edits by hand. */
        .key = "macros_json", .section = "macros", .type = KVM_VT_STR,
        .title = "User key macros",
        .help = "Saved macros, edited from the Input panel.",
        .def_str = "[]", .max_len = 2000, .requires_cap = KVM_CAP_HID,
    },

    /* ---- storage -------------------------------------------------------- */
    {
        .key = "msc_enable", .section = "storage", .type = KVM_VT_BOOL,
        .title = "Expose virtual media",
        .help = "Present a USB drive to the target that it can boot from. Off keeps the "
                "device a plain keyboard and mouse, which also frees the USB endpoints the "
                "drive would use. Adding or removing the drive re-enumerates the device to "
                "the target, so it takes effect after a restart; swapping the image once the "
                "drive exists does not.",
        .def = 0, .requires_cap = KVM_CAP_MSC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "msc_mode", .section = "storage", .type = KVM_VT_ENUM,
        .title = "Media type",
        .help = "How an image is presented to the target. \"auto\" serves a .iso as a "
                "CD-ROM (so it boots and mounts as an optical drive) and any other file "
                "as a removable disk - the right choice almost always. Force \"cdrom\" or "
                "\"disk\" only if a file is misnamed. Handing over the whole card is picked "
                "in the Media panel, not here. Switching the type re-attaches the USB drive.",
        .min = 0, .max = ENUM_MAX(s_media_choices), .def = 0, .choices = s_media_choices, .requires_cap = KVM_CAP_MSC,
    },
    {
        /* The active medium, chosen from the Media panel (a file name, or "@rescue"
         * / "@wholesd"). Kept out of the settings panel - section "storage_hidden"
         * is not one the panel renders - so the Media panel is the single place it
         * is picked, rather than showing the same choice twice. */
        .key = "msc_image", .section = "storage_hidden", .type = KVM_VT_STR,
        .title = "Mounted image",
        .help = "File on the microSD card currently offered to the target.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_MSC,
    },

    /* ---- power ---------------------------------------------------------- */
    {
        .key = "pwr_wol_mac", .section = "power", .type = KVM_VT_STR,
        .title = "Target MAC for Wake-on-LAN",
        .help = "The target's Ethernet MAC, e.g. AA:BB:CC:DD:EE:FF. The Wake button sends "
                "it a magic packet. The target must have Wake-on-LAN enabled in its BIOS and "
                "keep standby power.",
        .def_str = "", .max_len = 17, .requires_cap = KVM_CAP_WOL,
    },
    {
        .key = "atx_enable", .section = "power", .type = KVM_VT_BOOL,
        .title = "Enable ATX control",
        .help = "Requires optocouplers wired to the target's front-panel header.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "atx_pwr_gpio", .section = "power", .type = KVM_VT_INT,
        .title = "Power button GPIO",
        .help = "Drives the optocoupler across the target's power button. A free "
                "pin on the Waveshare board; 20 is a safe default. -1 to disable.",
        .min = -1, .max = 54, .def = -1, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
    },
    {
        .key = "atx_rst_gpio", .section = "power", .type = KVM_VT_INT,
        .title = "Reset button GPIO",
        .help = "Drives the optocoupler across the target's reset button. 21 is a "
                "safe default. -1 to disable.",
        .min = -1, .max = 54, .def = -1, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
    },
    {
        .key = "atx_led_gpio", .section = "power", .type = KVM_VT_INT,
        .title = "Power LED sense GPIO",
        .help = "Reads the target's power LED through an optocoupler. Wire it to the "
                "power LED, not the HDD LED, which only blinks on disk activity. 22 "
                "is a safe default. -1 if you are not sensing the LED.",
        .min = -1, .max = 54, .def = -1, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
    },
    {
        .key = "atx_short_ms", .section = "power", .type = KVM_VT_INT,
        .title = "Short press (ms)",
        .help = "How long a normal power or reset press is held.",
        .min = 50, .max = 2000, .def = 200, .requires_cap = -1,
    },
    {
        .key = "atx_long_ms", .section = "power", .type = KVM_VT_INT,
        .title = "Force-off hold (ms)",
        .help = "How long the power button is held for a hard power off.",
        .min = 1000, .max = 15000, .def = 5000, .requires_cap = -1,
    },
    {
        .key = "atx_active_high", .section = "power", .type = KVM_VT_BOOL,
        .title = "Buttons active-high",
        .help = "On to press the button when the GPIO drives high (high-level-trigger "
                "optocoupler modules). Turn off if your module presses on a low.",
        .def = 1, .requires_cap = -1,
    },
    {
        /* Key kept <= 15 chars: NVS rejects longer keys, and "atx_led_active_high"
         * (19) silently failed to persist, reverting to the default every boot. */
        .key = "atx_led_ah", .section = "power", .type = KVM_VT_BOOL,
        .title = "Power LED active-high",
        .help = "On if the LED sense reads high when the target is powered. Flip it if "
                "the reported power state is inverted.",
        .def = 1, .requires_cap = -1,
    },

    /* ---- audio ---------------------------------------------------------- */
    {
        .key = "aud_enable", .section = "audio", .type = KVM_VT_BOOL,
        .title = "Stream HDMI audio",
        .help = "Only works once BCLK, LRCK and DATA are wired from the capture board "
                "to the ESP32-P4.",
        .def = 0, .requires_cap = KVM_CAP_AUDIO, .flags = KVM_SF_REBOOT,
    },

    /* ---- network -------------------------------------------------------- */
    {
        .key = "net_hostname", .section = "network", .type = KVM_VT_STR,
        .title = "Hostname",
        .help = "Also the mDNS name: <hostname>.local",
        .def_str = CONFIG_KVM_MDNS_HOSTNAME, .max_len = 31, .requires_cap = -1,
        .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_dhcp", .section = "network", .type = KVM_VT_BOOL,
        .title = "Obtain address automatically",
        .def = 1, .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_ip", .section = "network", .type = KVM_VT_STR,
        .title = "Static address", .def_str = "", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_mask", .section = "network", .type = KVM_VT_STR,
        .title = "Netmask", .def_str = "255.255.255.0", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_gw", .section = "network", .type = KVM_VT_STR,
        .title = "Gateway", .def_str = "", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_dns", .section = "network", .type = KVM_VT_STR,
        .title = "DNS server", .def_str = "", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_ipv6", .section = "network", .type = KVM_VT_BOOL,
        .title = "IPv6",
        .help = "Also answer on an IPv6 address, alongside IPv4. There is nothing to "
                "configure: the address comes from the router's advertisements. Turn it "
                "off to keep the device off IPv6 entirely.",
        .def = 1, .requires_cap = -1, .flags = KVM_SF_REBOOT,
    },

    /* ---- wifi (boards with an ESP32-C6 co-processor only) ---------------- */
    {
        .key = "net_mode", .section = "network", .type = KVM_VT_ENUM,
        .title = "Connection",
        .help = "The device uses one link at a time. \"ethernet\": the wired port. "
                "\"wifi\": join the network below (Ethernet is left down). \"ap\": the "
                "device makes its own WiFi hotspot for setup where there is no "
                "network to join. If WiFi is unreachable: reset the board, then hold "
                "the button for two seconds - that returns it to Ethernet, and "
                "clears the password too. Hold it AFTER the reset, not through it.",
        .min = 0, .max = ENUM_MAX(s_netmode_choices), .def = 0, .choices = s_netmode_choices,
        .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "wifi_ssid", .section = "network", .type = KVM_VT_STR,
        .title = "WiFi network (SSID)",
        .help = "The name of the network to join in \"wifi\" mode.",
        .def_str = "", .max_len = 32, .requires_cap = KVM_CAP_WIFI,
        .flags = KVM_SF_REBOOT,
    },
    {
        .key = "wifi_pass", .section = "network", .type = KVM_VT_STR,
        .title = "WiFi password",
        .help = "Left blank for an open network. Stored write-only.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_WIFI,
        .flags = KVM_SF_SECRET | KVM_SF_REBOOT,
    },
    {
        .key = "ap_open", .section = "network", .type = KVM_VT_BOOL,
        .title = "Open hotspot (no password)",
        .help = "Run the device's hotspot with no password at all. Off, and with "
                "no password set, the device makes one up on first use and prints "
                "it in the log and on the display - an open network anybody in the "
                "building can join is a poor way to reach a server. Turn this on "
                "only where that is what you want.",
        .def = 0, .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "ap_pass", .section = "network", .type = KVM_VT_STR,
        .title = "Hotspot password",
        .help = "Password for the device's own hotspot (\"ap\" mode, or the rescue "
                "hotspot below). At least 8 characters. Left blank, the device "
                "makes one up the first time the hotspot comes up and prints it "
                "in the log and on the display; for a hotspot with no password at "
                "all, use the setting above. The network name is ESP-KVM-xxxx "
                "(the device's MAC). Stored write-only.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_WIFI,
        .flags = KVM_SF_SECRET | KVM_SF_REBOOT,
    },
    /* KVM_SF_SECRET means write-only over the API - and never in a log line.
     * The console hands the device log to anyone signed in, and people paste it
     * into public bug reports; a passphrase that reaches ESP_LOG is published.
     * See components/kvm_log. */
    {
        .key = "net_fallback", .section = "network", .type = KVM_VT_ENUM,
        .title = "If WiFi can't connect",
        .help = "keep_trying: keep retrying the network - it reconnects on its own "
                "when the network comes back (best for a device you cannot reach "
                "physically). hotspot: ALSO run a rescue hotspot (ESP-KVM-xxxx) the "
                "whole time WiFi is trying, so you can always reach the device "
                "on-site to fix its settings - set a Hotspot password first. Only "
                "applies in WiFi (station) mode.",
        .min = 0, .max = ENUM_MAX(s_fallback_choices), .def = 0, .choices = s_fallback_choices,
        .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT,
    },

    /* ---- vpn / wireguard ------------------------------------------------- */
    {
        .key = "wg_enable", .section = "vpn", .type = KVM_VT_BOOL,
        .title = "Enable WireGuard",
        .help = "Bring up a classic WireGuard tunnel to a hub so the device is "
                "reachable over the VPN. Off by default. Split-tunnel: only the "
                "tunnel subnet goes through WireGuard, so the console stays "
                "reachable on the LAN too. An alternative to Tailscale below, not "
                "a companion - enable one.",
        .def = 0, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_address", .section = "vpn", .type = KVM_VT_STR,
        .title = "Tunnel address",
        .help = "The device's own IP on the WireGuard network, e.g. 10.9.0.2.",
        .def_str = "", .max_len = 31, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_private_key", .section = "vpn", .type = KVM_VT_STR,
        .title = "Private key",
        .help = "Base64 WireGuard private key. Leave empty and the device "
                "generates one on first connect; its public key is shown below.",
        .def_str = "", .max_len = 47, .flags = KVM_SF_SECRET, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_peer_key", .section = "vpn", .type = KVM_VT_STR,
        .title = "Peer public key",
        .help = "Base64 public key of the WireGuard peer (the hub/server).",
        .def_str = "", .max_len = 47, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_endpoint", .section = "vpn", .type = KVM_VT_STR,
        .title = "Peer endpoint",
        .help = "host:port of the peer, e.g. vpn.example.com:51820.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_keepalive", .section = "vpn", .type = KVM_VT_INT,
        .title = "Persistent keepalive (s)",
        .help = "Keeps a NAT/firewall mapping open. 25 is typical; 0 disables it.",
        .min = 0, .max = 65535, .def = 25, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_sntp", .section = "vpn", .type = KVM_VT_BOOL,
        .title = "Sync time over SNTP",
        .help = "WireGuard handshakes carry a timestamp; without a real clock a "
                "reboot can make the peer reject them. Turn this on if the device "
                "can reach an NTP server. Off by default (isolated networks).",
        .def = 0, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_sntp_srv", .section = "vpn", .type = KVM_VT_STR,
        .title = "NTP server",
        .help = "Used only when SNTP is on.",
        .def_str = "pool.ntp.org", .max_len = 47, .requires_cap = KVM_CAP_WG,
    },

    /* ---- vpn / tailscale ------------------------------------------------- */
    {
        .key = "ts_enable", .section = "vpn", .type = KVM_VT_BOOL,
        .title = "Enable Tailscale",
        .help = "Join a Tailscale network natively - the device gets a 100.x "
                "address reachable from anywhere on your tailnet, with NAT "
                "traversal handled for you and no separate gateway. Off by "
                "default. An alternative to WireGuard above, not a companion; "
                "enabling both is unusual.",
        .def = 0, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_auth_key", .section = "vpn", .type = KVM_VT_STR,
        .title = "Auth key",
        .help = "A Tailscale auth key (tskey-auth-...) that authorises this device "
                "to join. Generate one in the Tailscale admin console; a reusable "
                "key survives re-registration across reboots.",
        .def_str = "", .max_len = 63, .flags = KVM_SF_SECRET, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_hostname", .section = "vpn", .type = KVM_VT_STR,
        .title = "Tailnet hostname",
        .help = "The name this device takes on the tailnet. Empty uses the mDNS "
                "hostname from the Network section.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_control_url", .section = "vpn", .type = KVM_VT_STR,
        .title = "Control server",
        .help = "Coordination server for a self-hosted control plane (Headscale, "
                "Ionscale). Empty uses Tailscale's own (controlplane.tailscale.com). "
                "Host only, no scheme - e.g. headscale.example.com.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_control_port", .section = "vpn", .type = KVM_VT_INT,
        .title = "Control server port",
        .help = "Port for the control server. 0 = default (443 with TLS, else 80). "
                "Set only if your Headscale listens on a non-standard port.",
        .def = 0, .min = 0, .max = 65535, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_ctrl_tls", .section = "vpn", .type = KVM_VT_BOOL,
        .title = "Control plane over TLS",
        .help = "Reach the coordination server over HTTPS. Required for the hosted "
                "Tailscale service (the default). Turn off only for a self-hosted "
                "Headscale served over plain HTTP.",
        .def = 1, .requires_cap = KVM_CAP_TS,
    },

    /* ---- mqtt / home assistant ------------------------------------------ */
    {
        .key = "mqtt_enable", .section = "mqtt", .type = KVM_VT_BOOL,
        .title = "Publish to MQTT",
        .help = "Report status to an MQTT broker and appear in Home Assistant "
                "(auto-discovered). Off by default; costs nothing when off.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "mqtt_host", .section = "mqtt", .type = KVM_VT_STR,
        .title = "Broker host",
        .help = "Hostname or IP of the MQTT broker, e.g. the Home Assistant host.",
        .def_str = "", .max_len = 63, .requires_cap = -1,
    },
    {
        .key = "mqtt_port", .section = "mqtt", .type = KVM_VT_INT,
        .title = "Broker port",
        .help = "1883 for plain MQTT, 8883 for MQTT over TLS.",
        .min = 1, .max = 65535, .def = 1883, .requires_cap = -1,
    },
    {
        .key = "mqtt_tls", .section = "mqtt", .type = KVM_VT_BOOL,
        .title = "Use TLS",
        .help = "Connect with mqtts. Set the port to 8883 as well.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "mqtt_verify", .section = "mqtt", .type = KVM_VT_BOOL,
        .title = "Verify broker certificate",
        .help = "With TLS on, check the broker's certificate against the built-in "
                "CA bundle (public CAs, e.g. Let's Encrypt). Turn off for a broker "
                "with a self-signed certificate.",
        .def = 1, .requires_cap = -1,
    },
    {
        .key = "mqtt_user", .section = "mqtt", .type = KVM_VT_STR,
        .title = "Username",
        .help = "Leave empty for an anonymous broker.",
        .def_str = "", .max_len = 47, .requires_cap = -1,
    },
    {
        .key = "mqtt_pass", .section = "mqtt", .type = KVM_VT_STR,
        .title = "Password",
        .help = "Stored on the device; never sent back to the console.",
        .def_str = "", .max_len = 63, .flags = KVM_SF_SECRET, .requires_cap = -1,
    },
    {
        .key = "mqtt_base", .section = "mqtt", .type = KVM_VT_STR,
        .title = "Base topic",
        .help = "Topic prefix; the device id is appended, e.g. espkvm/a1b2c3.",
        .def_str = "espkvm", .max_len = 31, .requires_cap = -1,
    },
    {
        .key = "mqtt_disco", .section = "mqtt", .type = KVM_VT_STR,
        .title = "Discovery prefix",
        .help = "Home Assistant MQTT discovery prefix. Default suits a stock HA.",
        .def_str = "homeassistant", .max_len = 31, .requires_cap = -1,
    },
    {
        .key = "mqtt_interval", .section = "mqtt", .type = KVM_VT_INT,
        .title = "Publish interval (s)",
        .help = "How often telemetry is published.",
        .min = 5, .max = 3600, .def = 30, .requires_cap = -1,
    },

    /* ---- security ------------------------------------------------------- */
    {
        .key = "sec_https", .section = "security", .type = KVM_VT_BOOL,
        .title = "Serve over HTTPS",
        .help = "Uses a self-signed certificate generated on first boot. Disable only "
                "on a trusted network or behind a VPN.",
        .def = 1, .requires_cap = KVM_CAP_HTTPS, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "sec_auth", .section = "security", .type = KVM_VT_BOOL,
        .title = "Require login",
        .def = 1, .requires_cap = KVM_CAP_HTTPS, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "sec_user", .section = "security", .type = KVM_VT_STR,
        .title = "Username", .def_str = "admin", .max_len = 31, .requires_cap = KVM_CAP_HTTPS,
    },
    {
        .key = "agent_api", .section = "security", .type = KVM_VT_BOOL,
        .title = "Agent REST API",
        .help = "Enables the plain-REST snapshot and keyboard/mouse endpoints "
                "(/api/v1/video/frame.jpg, /api/v1/hid/*) used to drive the target "
                "from an AI agent or a script. Off by default: it grants the same "
                "control the console already has, over a simpler interface, so turn "
                "it on only when you mean to hand that control to a program.",
        .def = 0, .requires_cap = -1,
    },

    /* ---- system --------------------------------------------------------- */
    {
        .key = "upd_check", .section = "system", .type = KVM_VT_BOOL,
        .title = "Offer firmware updates",
        .help = "The browser asks the address below whether a newer build exists and offers to "
                "install it. The device never reaches out on its own - a KVM that phones home "
                "is not what belongs in an isolated network.",
        .def = 0, .requires_cap = KVM_CAP_OTA,
    },
    {
        .key = "upd_url", .section = "system", .type = KVM_VT_STR,
        .title = "Update manifest",
        .help = "URL of a manifest.json describing the newest build. The project's own builds "
                "are published at the default address; point it at your fork, or at a file "
                "server inside your network, and it will use that instead. Whatever it points "
                "at is what gets written to the device, so point it somewhere you trust.",
        .def_str = CONFIG_KVM_UPDATE_URL,
        .max_len = 200, .requires_cap = KVM_CAP_OTA,
    },

    {
        .key = "fw_fetch", .section = "system", .type = KVM_VT_BOOL,
        .title = "Let the device fetch releases itself",
        .help = "Off by default, and deliberately so: a KVM often sits where nothing is "
                "supposed to reach the internet, and it does not talk to GitHub unless it is "
                "told to. Turn it on and the console can offer any published release - which "
                "is how you go back to an earlier one, since the browser is not allowed to "
                "download the image itself. Ordinary updates do not need this; the browser "
                "fetches those and hands them over.",
        .def = 0, .requires_cap = KVM_CAP_OTA,
    },

    {
        .key = "therm_guard", .section = "system", .type = KVM_VT_BOOL,
        .title = "Thermal protection",
        .help = "Cap the frame rate when the chip gets warm and stop encoding if it gets hot. "
                "Keyboard, mouse and the web interface keep running either way - a KVM that "
                "stops accepting keystrokes because it is warm has failed at its job.",
        .def = 1, .requires_cap = -1,
    },
    {
        .key = "therm_warn", .section = "system", .type = KVM_VT_INT,
        .title = "Warm threshold (C)",
        .help = "Above this the frame rate is halved. Measured on this board: 1080p MJPEG at "
                "full rate settles around 46 C in open air, so the default leaves plenty of "
                "room before anything is given up.",
        .min = 35, .max = 100, .def = 70, .requires_cap = -1,
    },
    {
        .key = "therm_stop", .section = "system", .type = KVM_VT_INT,
        .title = "Hot threshold (C)",
        .help = "Above this encoding stops until the chip cools. The ESP32 family is rated to "
                "85 C ambient and the die runs hotter than the air around it.",
        .min = 40, .max = 110, .def = 85, .requires_cap = -1,
    },

    {
        .key = "log_level", .section = "system", .type = KVM_VT_ENUM,
        .title = "Log verbosity",
        .min = 0, .max = ENUM_MAX(s_log_choices), .def = 2, .choices = s_log_choices, .requires_cap = -1,
    },
    {
        .key = "ui_side", .section = "system", .type = KVM_VT_ENUM,
        .title = "Panel side",
        .help = "Which side of the screen the button rail and its panels sit on.",
        .min = 0, .max = ENUM_MAX(s_side_choices), .def = 0, .choices = s_side_choices, .requires_cap = -1,
    },

    /* ---- status display ------------------------------------------------- */
    {
        .key = "disp_enable", .section = "display", .type = KVM_VT_BOOL,
        .title = "Status display",
        .help = "Drive a small display that shows the IP, link, capture status and "
                "health. An I2C OLED (SSD1306/SH1106) shares the capture chip's I2C "
                "and is auto-detected with no extra pins; a round SPI LCD (GC9A01) "
                "uses the GPIOs set below, after you pick it as the display type. "
                "Off by default; does nothing when no panel is connected.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "disp_type", .section = "display", .type = KVM_VT_ENUM,
        .title = "Panel",
        .help = "Which panel is wired, controller and size together. SSD1306 and SH1106 "
                "are I2C OLEDs - pick SH1106 if the image is shifted by two pixels or "
                "wraps. GC9A01 is a round colour SPI LCD, e.g. the Waveshare 1.28\" "
                "module. None of this can be detected: one chip drives every size and "
                "reports none of it. A wrong size shows the picture squeezed into part "
                "of the glass, or every other row missing. Shorter panels show fewer "
                "status lines.",
        .min = 0, .max = ENUM_MAX(s_display_choices), .def = 0, .choices = s_display_choices, .requires_cap = -1,
    },
    /* GC9A01 SPI pins (the I2C OLEDs need none - they share the capture bus).
     * Pins, so the console offers only free GPIOs; a restart re-attaches on the
     * new wiring. Defaults are a sane free set on the P4; set them to your board. */
    {
        .key = "disp_sclk", .section = "display", .type = KVM_VT_INT, .title = "LCD SCLK / CLK",
        .help = "SPI clock GPIO for the GC9A01. Ignored by the I2C OLEDs.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_SCLK_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "disp_type", .visible_val = 2, /* GC9A01 only */
    },
    {
        .key = "disp_mosi", .section = "display", .type = KVM_VT_INT, .title = "LCD MOSI / DIN",
        .help = "SPI data GPIO for the GC9A01.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_MOSI_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "disp_type", .visible_val = 2, /* GC9A01 only */
    },
    {
        .key = "disp_cs", .section = "display", .type = KVM_VT_INT, .title = "LCD CS",
        .help = "Chip-select GPIO for the GC9A01.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_CS_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "disp_type", .visible_val = 2, /* GC9A01 only */
    },
    {
        .key = "disp_dc", .section = "display", .type = KVM_VT_INT, .title = "LCD DC",
        .help = "Data/command GPIO for the GC9A01.",
        /* Not 45: on the Function EV board that pin carries SD_PWRn unless a
         * resistor is moved, so a panel wired there never sees clean levels and
         * stays dark. 26 is free on every board we support. */
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_DC_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "disp_type", .visible_val = 2, /* GC9A01 only */
    },
    {
        .key = "disp_rst", .section = "display", .type = KVM_VT_INT, .title = "LCD RST",
        .help = "Reset GPIO for the GC9A01. -1 (None) if RST is tied to 3V3.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_RST_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "disp_type", .visible_val = 2, /* GC9A01 only */
    },
    {
        .key = "disp_bl", .section = "display", .type = KVM_VT_INT, .title = "LCD backlight",
        .help = "Backlight GPIO for the GC9A01. -1 (None) if BL is tied to 3V3 (always on).",
        .min = -1, .max = 54, .def = -1, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "disp_type", .visible_val = 2, /* GC9A01 only */
    },
};
/* clang-format on */

const kvm_setting_t *kvm_settings_table(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_settings) / sizeof(s_settings[0]);
    }
    return s_settings;
}
