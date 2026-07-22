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

static const char *const s_codec_choices[] = {"mjpeg", "h264"};
static const char *const s_edid_choices[] = {"full", "1080p30", "custom"};
static const char *const s_mouse_choices[] = {"absolute", "relative", "auto"};
static const char *const s_engage_choices[] = {"click", "hover"};
/* Only layouts with a verified character table; see web/src/layouts.js. */
static const char *const s_layout_choices[] = {"en_us", "ru_ru"};
static const char *const s_media_choices[] = {"cdrom", "removable", "whole_sd"};
static const char *const s_log_choices[] = {"error", "warn", "info", "debug"};

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
        .min = 0, .max = 1, .def = 0, .choices = s_codec_choices,
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
        .help = "What the capture card claims to be. \"full\" advertises the common "
                "modes from 640x480 up; switch to \"1080p30\" if a source refuses to "
                "output a picture.",
        .min = 0, .max = 2, .def = 0, .choices = s_edid_choices,
        .requires_cap = KVM_CAP_VIDEO, .flags = KVM_SF_REBOOT,
    },

    /* ---- input ---------------------------------------------------------- */
    {
        .key = "mouse_mode", .section = "input", .type = KVM_VT_ENUM,
        .title = "Pointer mode",
        .help = "Absolute puts the target's cursor exactly where you click and is "
                "the right choice almost always. Relative is for software that "
                "captures the pointer, such as games and 3D viewers.",
        .min = 0, .max = 2, .def = 0, .choices = s_mouse_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "ptr_engage", .section = "input", .type = KVM_VT_ENUM,
        .title = "Start controlling on",
        .help = "\"click\" waits for a click on the video before input reaches the "
                "target, and stops again on Esc or a click outside; the engaging click "
                "is still delivered. \"hover\" tracks the pointer as soon as it is over "
                "the video, the way a remote desktop behaves. Keyboard input always "
                "requires a click first.",
        .min = 0, .max = 1, .def = 0, .choices = s_engage_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "mouse_sens", .section = "input", .type = KVM_VT_INT,
        .title = "Relative sensitivity (%)",
        .help = "Scales pointer movement in relative mode only.",
        .min = 10, .max = 400, .def = 100, .requires_cap = KVM_CAP_HID,
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
        .min = 0, .max = 1, .def = 0, .choices = s_layout_choices, .requires_cap = KVM_CAP_HID,
    },
    {
        .key = "type_delay", .section = "input", .type = KVM_VT_INT,
        .title = "Paste keystroke delay (ms)",
        .help = "Raise it if the target drops characters while text is being pasted.",
        .min = 1, .max = 200, .def = 8, .requires_cap = KVM_CAP_HID,
    },

    /* ---- storage -------------------------------------------------------- */
    {
        .key = "msc_enable", .section = "storage", .type = KVM_VT_BOOL,
        .title = "Expose virtual media",
        .help = "Present a disk image to the target as a USB drive. Turn off to keep "
                "the device a plain keyboard and mouse.",
        .def = 0, .requires_cap = KVM_CAP_MSC,
    },
    {
        .key = "msc_mode", .section = "storage", .type = KVM_VT_ENUM,
        .title = "Media type",
        .help = "CD-ROM for bootable ISO images, removable for writable disk images, "
                "or hand the whole microSD card to the target as a flash drive.",
        .min = 0, .max = 2, .def = 0, .choices = s_media_choices, .requires_cap = KVM_CAP_MSC,
    },
    {
        .key = "msc_image", .section = "storage", .type = KVM_VT_STR,
        .title = "Mounted image",
        .help = "File on the microSD card currently offered to the target.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_MSC,
    },

    /* ---- power ---------------------------------------------------------- */
    {
        .key = "atx_enable", .section = "power", .type = KVM_VT_BOOL,
        .title = "Enable ATX control",
        .help = "Requires optocouplers wired to the target's front-panel header.",
        .def = 0, .requires_cap = KVM_CAP_ATX,
    },
    {
        .key = "atx_pwr_gpio", .section = "power", .type = KVM_VT_INT,
        .title = "Power button GPIO",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_ATX, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "atx_rst_gpio", .section = "power", .type = KVM_VT_INT,
        .title = "Reset button GPIO",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_ATX, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "atx_led_gpio", .section = "power", .type = KVM_VT_INT,
        .title = "Power LED sense GPIO",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_ATX, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "atx_long_ms", .section = "power", .type = KVM_VT_INT,
        .title = "Force-off hold (ms)",
        .help = "How long the power button is held for a hard power off.",
        .min = 1000, .max = 15000, .def = 5000, .requires_cap = KVM_CAP_ATX,
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
        .def_str = "https://espkvm.github.io/espkvm/firmware/manifest.json",
        .max_len = 200, .requires_cap = KVM_CAP_OTA,
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
        .min = 0, .max = 3, .def = 2, .choices = s_log_choices, .requires_cap = -1,
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
