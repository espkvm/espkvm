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

/*
 * The pin defaults are Kconfig values that only exist while their feature is
 * built in, but the rows below are not compiled out with it - the setting still
 * has to be listed so the console can say the feature is off. Unassigned is the
 * right answer when there is nothing to assign to.
 */
#ifndef CONFIG_KVM_ATX_PWR_GPIO
#define CONFIG_KVM_ATX_PWR_GPIO -1
#endif
#ifndef CONFIG_KVM_ATX_RST_GPIO
#define CONFIG_KVM_ATX_RST_GPIO -1
#endif
#ifndef CONFIG_KVM_ATX_LED_GPIO
#define CONFIG_KVM_ATX_LED_GPIO -1
#endif
#ifndef CONFIG_KVM_DISP_BL_GPIO
#define CONFIG_KVM_DISP_BL_GPIO -1
#endif
#ifndef CONFIG_KVM_DISP_DEFAULT_TYPE
#define CONFIG_KVM_DISP_DEFAULT_TYPE 0
#endif
#ifndef CONFIG_KVM_DISP_SCLK_GPIO
#define CONFIG_KVM_DISP_SCLK_GPIO -1
#endif
#ifndef CONFIG_KVM_DISP_MOSI_GPIO
#define CONFIG_KVM_DISP_MOSI_GPIO -1
#endif
#ifndef CONFIG_KVM_DISP_CS_GPIO
#define CONFIG_KVM_DISP_CS_GPIO -1
#endif
#ifndef CONFIG_KVM_DISP_DC_GPIO
#define CONFIG_KVM_DISP_DC_GPIO -1
#endif
#ifndef CONFIG_KVM_DISP_RST_GPIO
#define CONFIG_KVM_DISP_RST_GPIO -1
#endif

static const char *const s_codec_choices[] = {"mjpeg", "h264"};
/* Kinds of notification, bit 0 first: the order of kvm_notify_ev_t. */
static const char *const s_notify_ev[] = {"watch", "flat", "area", "clip", "netlog", "tskey"};
static const char *const s_notify_ev_labels[] = {
    "A watched phrase on the screen", "Blank screen", "Screen area", "Dashcam clip",
    "Target log alert", "Tailscale key running out"};
#define NOTIFY_EV_ALL ((1 << 6) - 1)
#define NOTIFY_EV_DEF (NOTIFY_EV_ALL & ~(1 << 1)) /* all but the blank screen */
/* Must match kvm_upload_kind_t: the bit for each kind is 1 << its value. */
static const char *const s_up_ev[] = {"recording", "clip", "dashcam", "screenshot", "timelapse"};
static const char *const s_up_ev_labels[] = {
    "Recording", "Clip saved by hand", "Dashcam clip", "Screenshot", "Timelapse"};
#define UP_EV_ALL ((1 << 5) - 1)
static const char *const s_rec_subs_choices[] = {"off", "keys", "everything"};
static const char *const s_edid_choices[] = {"full", "1080p30", "720p", "1024x768", "480p", "1080p50"};
static const char *const s_mouse_choices[] = {"absolute", "relative"};
static const char *const s_engage_choices[] = {"click", "hover"};
static const char *const s_cursor_choices[] = {"arrow", "dot", "hidden"};
/* Must match the layout ids in web/src/layouts.ts. New ones go on the end:
   the value is stored as an index, so reordering would move everyone's setting. */
static const char *const s_layout_choices[] = {"en_us", "ru_ru", "cs_cz", "uk_ua", "lt_lt"};
static const char *const s_media_choices[] = {"auto", "cdrom", "disk"};
/* Index 1 is what dashcam.c calls on_card(). */
static const char *const s_rtc_chip_choices[] = {"Auto", "Off", "DS3231", "PCF8563 / BM8563",
                                                 "PCF85063", "PCF8523"};
static const char *const s_rtc_bus_choices[] = {"The capture board's I2C", "Its own pins"};
static const char *const s_dashcam_store_choices[] = {"memory", "microSD"};
/* Index 1.. must match k_sd_steps_khz[] in kvm_storage.c. */
static const char *const s_sd_speed_choices[] = {"auto", "40 MHz", "20 MHz", "10 MHz", "4 MHz", "2 MHz"};
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
    "SSD1315 128x64 (untested)",
    "SSD1315 72x40 (M5Stack Mini OLED)",
    "SH1107 128x64 (M5Stack Unit OLED)",
    "SSD1309 128x64 (M5Stack Unit Glass2, untested)",
    "ST7789 240x240 (JetKVM Mini)",
};
/* The disp_type values that are SPI LCDs: GC9A01 (2) and ST7789 (15). Their
   pins show for both; the OLED's own I2C pins for everything else. */
#define DISP_SPI_TYPES ((1 << 2) | (1 << 15))
/* A built-in panel (the JetKVM Mini): its type and pins are not the
   operator's to change, so they never show, and the colour-panel settings
   (brightness, the picture page) hang on the display being on rather than on
   a type that is hidden. */
#if CONFIG_KVM_DISP_FIXED
#define DISP_TYPE_SHOW 2           /* disp_enable never equals 2 */
#define DISP_PIN_SHOW 0            /* in no type */
#define DISP_OLED_SHOW 0x7fffffff  /* notIn every type */
#define DISP_COLOUR_KEY "disp_enable"
#define DISP_COLOUR_VAL 1
#define DISP_COLOUR_FLAGS 0
#else
#define DISP_TYPE_SHOW 1
#define DISP_PIN_SHOW DISP_SPI_TYPES
#define DISP_OLED_SHOW DISP_SPI_TYPES
#define DISP_COLOUR_KEY "disp_type"
#define DISP_COLOUR_VAL DISP_SPI_TYPES
#define DISP_COLOUR_FLAGS KVM_SF_SHOW_IN
#endif
#ifndef CONFIG_KVM_BTN_COUNT
#define CONFIG_KVM_BTN_COUNT 4
#endif
#ifndef CONFIG_KVM_BTN_GPIO_DEFAULT
#define CONFIG_KVM_BTN_GPIO_DEFAULT -1
#define CONFIG_KVM_BTN_PRESS_DEFAULT 0
#define CONFIG_KVM_BTN_HOLD_DEFAULT 0
#endif
#ifndef CONFIG_KVM_DISP_BRIGHT_DEFAULT
#define CONFIG_KVM_DISP_BRIGHT_DEFAULT 100
#endif
#if CONFIG_KVM_DISP_DEFAULT_ON
#define DISP_DEFAULT_ON 1
#else
#define DISP_DEFAULT_ON 0
#endif
/* "auto" follows the OS guessed from USB enumeration; the rest force it. */
static const char *const s_targetos_choices[] = {"auto", "windows", "macos", "linux", "android"};
/* Must match k_bauds in kvm_serial.c. */
static const char *const s_baud_choices[] = {"9600",   "19200",  "38400",  "57600",
                                             "115200", "230400", "460800", "921600"};
/* Must match USB_SERIAL_AGENT in kvm_agent.c and the 1 in kvm_serial.c. */
static const char *const s_usb_serial_choices[] = {"off", "console", "agent"};
static const char *const s_pad_choices[] = {"off", "switch", "switch_alone", "xinput",
                                            "xinput_alone", "xbox"};
/* What the button on the box does. Order must match k_actions[] in
   components/kvm_sched/button.c. */
static const char *const s_btn_action_choices[] = {"nothing", "power", "power off (hold)", "reset",
                                                   "wake (WoL)", "runbook", "save clip", "screenshot",
                                                   "hotspot on/off", "next network mode",
                                                   "Ethernet / WiFi", "next display page",
                                                   "display on/off"};
/* Read by the console only; how long its vibration tick lasts on a phone. */
static const char *const s_haptic_choices[] = {"light", "medium", "strong"};
static const char *const s_touch_kbd_choices[] = {"console", "phone"};
static const char *const s_netmode_choices[] = {"ethernet", "wifi", "ap", "auto"};
static const char *const s_fallback_choices[] = {"keep_trying", "hotspot"};

/* What the console shows for a choice, where the name the API takes would read
   badly. Same length and order as the choices beside them. */
static const char *const s_codec_labels[] = {"MJPEG", "H.264"};
static const char *const s_layout_labels[] = {"English (US)", "Russian", "Czech", "Ukrainian", "Lithuanian"};
static const char *const s_media_labels[] = {"Auto", "CD-ROM", "Disk"};
static const char *const s_targetos_labels[] = {"Detect", "Windows", "macOS", "Linux", "Android"};
static const char *const s_pad_labels[] = {"Off", "Nintendo Switch", "Nintendo Switch, pad only",
                                           "Xbox 360 (XInput)", "Xbox 360, pad only", "Original Xbox"};
static const char *const s_netmode_labels[] = {"Ethernet", "WiFi", "Own hotspot", "Ethernet, WiFi as backup"};
static const char *const s_fallback_labels[] = {"Keep trying", "Also start a hotspot"};
static const char *const s_log_labels[] = {"Errors", "Warnings", "Info", "Debug"};
static const char *const s_usb_serial_labels[] = {"Off", "Serial console", "Agent"};

/* clang-format off */
static const kvm_setting_t s_settings[] = {
    /* ---- video ---------------------------------------------------------- */
    {
        .key = "vid_codec", .section = "video", .group = "Picture", .type = KVM_VT_ENUM,
        .title = "Stream codec",
        .help = "H.264 costs a fraction of the bandwidth of MJPEG on a screen that "
                "barely changes, and about a third of the frame rate. Browsers decode "
                "it through WebCodecs, which they offer on HTTPS pages only, so it is "
                "unusable over plain HTTP until TLS is in place.",
        .min = 0, .max = ENUM_MAX(s_codec_choices), .def = 0, .choices = s_codec_choices,
        .requires_cap = KVM_CAP_H264,
        .flags = KVM_SF_PROFILE,
        .labels = s_codec_labels,
    },
    {
        .key = "jpg_quality", .section = "video", .group = "Picture", .type = KVM_VT_INT,
        .title = "JPEG quality",
        .help = "Higher is sharper and larger. Only affects the MJPEG codec.",
        .min = 1, .max = 100, .def = CONFIG_KVM_JPEG_QUALITY, .requires_cap = KVM_CAP_MJPEG,
        .flags = KVM_SF_PROFILE,
    },
    {
        .key = "h264_kbps", .section = "video", .group = "Picture", .type = KVM_VT_INT,
        .title = "H.264 bitrate",
        .help = "Target bitrate for the hardware encoder.",
        .min = 500, .max = 20000, .def = 4000, .requires_cap = KVM_CAP_H264,
        .flags = KVM_SF_PROFILE,
        .unit = "kbit/s",
    },
    {
        .key = "webrtc_enable", .section = "video", .group = "Picture", .type = KVM_VT_BOOL,
        .title = "Allow WebRTC",
        .help = "Let the console take H.264 over WebRTC: UDP, so a lost packet costs one frame "
                "instead of holding up the ones behind it. Only a signed-in console can start "
                "it, and the video is encrypted (DTLS-SRTP). Off sends all video over the "
                "WebSocket.",
        .def = 1, .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "vid_fps_max", .section = "video", .group = "Picture", .type = KVM_VT_INT,
        .title = "Frame rate limit",
        .help = "Upper bound on encoded frames per second. Lower it to save bandwidth "
                "on a slow link.",
        .min = 1, .max = 60, .def = 60, .requires_cap = KVM_CAP_VIDEO,
        .flags = KVM_SF_PROFILE,
        .unit = "fps",
    },
    {
        .key = "vid_adapt", .section = "video", .group = "Picture", .type = KVM_VT_BOOL,
        .title = "Skip unchanged frames",
        .help = "Stop sending while the target's screen is static. Drops the bitrate to "
                "nearly zero on an idle desktop. MJPEG only: H.264 already codes an "
                "unchanged screen as a frame of a few hundred bytes.",
        .def = 1, .requires_cap = KVM_CAP_VIDEO,
    },
    {
        .key = "h264_guard", .section = "video", .group = "Picture", .type = KVM_VT_BOOL,
        .title = "Rebuild a stuck H.264 encoder",
        .help = "The encoder's rate controller can settle at its coarsest setting and "
                "stay there: the picture goes to visible blocks on a screen that is not "
                "moving, and nothing short of building a new encoder brings it back. "
                "With this on, keyframes that collapse to a fraction of their usual size "
                "are taken as that fault and the encoder is rebuilt - a lost frame, once "
                "every two minutes at most. Turn it off if a screen of yours is being "
                "rebuilt for no reason; the log says when it happens.",
        .def = 1, .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "edid_prof", .section = "video", .group = "HDMI input", .type = KVM_VT_ENUM,
        .title = "EDID profile",
        .help = "What this device claims to be, as a monitor - the target picks its "
                "output mode from what is offered here. \"Full\" advertises the common "
                "modes from 640x480 up to 1920x1080@30. \"720p\" and \"1024x768\" stop "
                "lower, which is often what you want: a smaller picture encodes faster "
                "and costs less bandwidth. \"1080p30\" offers that one mode alone, for a "
                "source that refuses a list. \"480p\" offers 720x480 and nothing bigger, "
                "for old consoles and TV boxes. \"1080p50\", the default, is \"Full\" with "
                "1920x1080@50 first: the target sends a frame every 20 ms instead of 33, "
                "so the pointer shows up sooner, for a frame or two a second less. Only "
                "boards with a rev 3.x chip and a TC358743 take 50 Hz; others get "
                "\"Full\". Text modes stay on offer either way, so a BIOS still comes "
                "through as text.",
        .min = 0, .max = ENUM_MAX(s_edid_choices), .def = 5, .choices = s_edid_choices,
        .requires_cap = KVM_CAP_EDID, .flags = KVM_SF_REBOOT | KVM_SF_PROFILE,
    },
    {
        .key = "rec_split_min", .section = "video", .group = "Recording", .type = KVM_VT_INT,
        .title = "New recording file every",
        .help = "A long recording is written as several files, each playable on its own, "
                "so one is quick to download and a damaged card loses one piece rather "
                "than the whole. 0 starts a new file only at 3.9 GB, the largest file the device writes.",
        .min = 0, .max = 240, .def = 10, .requires_cap = KVM_CAP_H264,
        .unit = "min",
    },
    {
        .key = "rec_max_min", .section = "video", .group = "Recording", .type = KVM_VT_INT,
        .title = "Stop a recording after",
        .help = "So a recording left running does not fill the card. 0 records until it is "
                "stopped or the card is full. A runbook's \"record <seconds>\" and the "
                "API's ?seconds= set their own length instead.",
        .min = 0, .max = 1440, .def = 60, .requires_cap = KVM_CAP_H264,
        .unit = "min",
    },
    {
        .key = "dashcam", .section = "video", .group = "Dashcam", .type = KVM_VT_BOOL,
        .title = "Keep the last minutes in memory",
        .help = "Keeps the screen's recent past in memory, and saves it to the microSD card as a "
                "clip when something happens - the screen stays one colour, a watched phrase "
                "appears, the power goes off - or when you press Save clip. How far back "
                "depends on the picture: minutes of a still screen, seconds of a playing video. "
                "Needs H.264, and keeps the encoder running while it is on. On boards with an "
                "older (pre-3.0) chip there is less memory left, so it reaches back seconds, not minutes; "
                "keep the past on the microSD card there instead.",
        .def = 0, .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "dashcam_store", .section = "video", .group = "Dashcam", .type = KVM_VT_ENUM,
        .title = "Where the past is kept",
        .help = "\"Memory\" holds what PSRAM can spare and does not touch the card until something "
                "happens. \"microSD\" writes the screen to the card all the time, in 15-second pieces "
                "in VIDEO/.dashcam, and deletes the old ones: it reaches back as far as the setting "
                "below on any board, but the card cannot be handed to the target meanwhile.",
        .min = 0, .max = ENUM_MAX(s_dashcam_store_choices), .def = 0,
        .choices = s_dashcam_store_choices, .requires_cap = KVM_CAP_H264,
        .visible_key = "dashcam", .visible_val = 1,
    },
    {
        .key = "dashcam_pre_s", .section = "video", .group = "Dashcam", .type = KVM_VT_INT,
        .title = "Before the event",
        .help = "At most this much of the past goes into a clip - if memory holds that much, "
                "or all of it when the past is kept on the card.",
        .min = 5, .max = 600, .def = 120, .requires_cap = KVM_CAP_H264,
        .unit = "s",
        .visible_key = "dashcam", .visible_val = 1,
    },
    {
        .key = "dashcam_post_s", .section = "video", .group = "Dashcam", .type = KVM_VT_INT,
        .title = "After the event",
        .help = "How long a clip goes on after what set it off. Another event meanwhile "
                "makes it longer, up to ten minutes in all.",
        .min = 5, .max = 600, .def = 30, .requires_cap = KVM_CAP_H264,
        .unit = "s",
        .visible_key = "dashcam", .visible_val = 1,
    },
    {
        .key = "dashcam_on_flat", .section = "video", .group = "Dashcam", .type = KVM_VT_BOOL,
        .title = "Save when the screen stays one colour",
        .help = "Half a minute of one flat colour, such as a stop screen. A black screen does "
                "not count: that is usually the display going to sleep.",
        .def = 1, .requires_cap = KVM_CAP_H264,
        .visible_key = "dashcam", .visible_val = 1,
    },
    {
        .key = "dashcam_on_watch", .section = "video", .group = "Dashcam", .type = KVM_VT_BOOL,
        .title = "Save when a watched phrase appears",
        .help = "The phrases are the ones set for screen alerts.",
        .def = 1, .requires_cap = KVM_CAP_H264,
        .visible_key = "dashcam", .visible_val = 1,
    },
    {
        .key = "dashcam_on_power", .section = "video", .group = "Dashcam", .type = KVM_VT_BOOL,
        .title = "Save when the target's power goes off",
        .help = "Needs the ATX power LED wired, so the device can see it.",
        .def = 1, .requires_cap = KVM_CAP_H264,
        .visible_key = "dashcam", .visible_val = 1,
    },
    {
        .key = "rec_text", .section = "video", .group = "Recording", .type = KVM_VT_BOOL,
        .title = "Save the screen's text with a recording",
        .help = "Reads the screen as characters every few seconds while recording and writes "
                "what it says into a .txt beside the video, so the recordings panel can search "
                "it and jump to the moment. Only screens drawn as text can be read - a BIOS, an "
                "installer, a console - and reading one costs about a tenth of a second.",
        .def = 1, .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "rec_tl_every", .section = "video", .group = "Recording", .type = KVM_VT_INT,
        .title = "Timelapse: a frame every",
        .help = "What a timelapse started from Home Assistant, or offered first in the "
                "recordings panel, keeps: one frame this often, played back at 25 fps.",
        .min = 1, .max = 3600, .def = 10, .requires_cap = KVM_CAP_H264,
        .unit = "s",
    },
    /* Uploading recordings and screenshots: a destination of the operator's
       own. WebDAV is the first; empty address = no upload buttons. */
    {
        .key = "up_dav_url", .section = "video", .group = "Upload", .type = KVM_VT_STR,
        .title = "WebDAV folder",
        .help = "Where the Upload button sends a recording or a screenshot: a folder on a WebDAV "
                "server, such as https://cloud.example.com/remote.php/dav/files/me/espkvm/ on "
                "Nextcloud, or a share on a NAS. Files go into a folder named after this device, "
                "with VIDEO and SCREENSHOTS in it. HTTPS needs a certificate the device trusts; "
                "plain http sends the password unencrypted, so keep it to your own network.",
        .def_str = "", .max_len = 200, .requires_cap = -1,
    },
    {
        .key = "up_dav_user", .section = "video", .group = "Upload", .type = KVM_VT_STR,
        .title = "User name",
        .help = "For the WebDAV server. Empty if it needs none.",
        .def_str = "", .max_len = 64, .requires_cap = -1,
    },
    {
        .key = "up_dav_pass", .section = "video", .group = "Upload", .type = KVM_VT_STR,
        .title = "Password",
        .help = "On Nextcloud, an app password made under Settings, Security.",
        .def_str = "", .max_len = 128, .requires_cap = -1, .flags = KVM_SF_SECRET,
    },
    /* One column per destination in the console's table; WebDAV is the first. */
    {
        .key = "up_dav_ev", .section = "video", .group = "Upload automatically", .type = KVM_VT_INT,
        .title = "WebDAV",
        .help = "What goes to the WebDAV folder on its own, as soon as it is saved on the card. "
                "Files wait on the card while the server cannot be reached, also over a restart.",
        .min = 0, .max = UP_EV_ALL, .def = 0, .choices = s_up_ev, .labels = s_up_ev_labels,
        .requires_cap = -1, .flags = KVM_SF_MASK,
    },
    {
        .key = "rec_subs", .section = "video", .group = "Recording", .type = KVM_VT_ENUM,
        .title = "Keystrokes in recordings",
        .help = "Writes what was pressed as subtitles next to each recording (a .srt "
                "file of the same name; VLC and mpv show it). \"Keys\": shortcuts, named keys "
                "and clicks, with typed characters shown as dots. \"Everything\": the typed "
                "text too - which includes any password typed on the target, stored on "
                "the card in plain text.",
        .min = 0, .max = ENUM_MAX(s_rec_subs_choices), .def = 0, .choices = s_rec_subs_choices,
        .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "rec_clicks", .section = "video", .group = "Recording", .type = KVM_VT_BOOL,
        .title = "Mouse clicks in recordings",
        .help = "With keystrokes in recordings on, also write each mouse click and "
                "where it landed.",
        .def = 1, .requires_cap = KVM_CAP_H264,
    },
    {
        .key = "scr_watch", .section = "video", .group = "Screen watch", .type = KVM_VT_BOOL,
        .title = "Watch the screen for words",
        .help = "Read a text screen even when nobody is looking, and raise an alert "
                "when it says one of the phrases below. This is the case a KVM is "
                "bought for - a machine that fell over at three in the morning - so it "
                "keeps working with the console closed. Costs one pass over the frame "
                "a second, and only while the target is showing text.",
        .def = 0, .requires_cap = KVM_CAP_VIDEO,
    },
    {
        .key = "scr_match", .section = "video", .group = "Screen watch", .type = KVM_VT_STR,
        .title = "Phrases to watch for",
        .help = "Comma-separated, matched without regard to case, anywhere on a line - "
                "for example: no boot device, kernel panic, press F1 to continue. An "
                "alert is raised the moment one appears and cleared when it goes, and "
                "every phrase on screen is named, not only the first; the log records "
                "both, and Home Assistant gets a sensor if MQTT is on.",
        .def_str = "", .max_len = 255, .requires_cap = KVM_CAP_VIDEO,
        .visible_key = "scr_watch", .visible_val = 1,
    },

    /* ---- input ---------------------------------------------------------- */
    {
        .key = "target_os", .section = "input", .group = "Target", .type = KVM_VT_ENUM,
        .title = "Target OS",
        .help = "Which machine's conventions the console follows - the label on the "
                "Meta key, and which OS-specific key combinations it offers. \"Detect\" "
                "trusts the guess made from how the target enumerates USB, shown next "
                "to the USB status; set it by hand if that guess is wrong or unknown.",
        .min = 0, .max = ENUM_MAX(s_targetos_choices), .def = 0, .choices = s_targetos_choices, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .labels = s_targetos_labels,
    },
    {
        .key = "mouse_mode", .section = "input", .group = "Pointer", .type = KVM_VT_ENUM,
        .title = "Pointer mode",
        .help = "Absolute puts the target's cursor exactly where you click and is "
                "the right choice almost always. Relative is for software that "
                "captures the pointer, such as games and 3D viewers, and for "
                "SteamOS in game mode, where the absolute one does not move. In "
                "relative mode the console hides your own cursor while in "
                "control, so the target's is the only one; Esc gives it back.",
        .min = 0, .max = ENUM_MAX(s_mouse_choices), .def = 0, .choices = s_mouse_choices, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
    },
    {
        .key = "ptr_engage", .section = "input", .group = "Pointer", .type = KVM_VT_ENUM,
        .title = "Start controlling on",
        .help = "\"Click\" waits for a click on the video before input reaches the "
                "target, and stops again on Esc or a click outside; the engaging click "
                "is still delivered. \"Hover\" tracks the pointer as soon as it is over "
                "the video, the way a remote desktop behaves. Keyboard input always "
                "requires a click first.",
        .min = 0, .max = ENUM_MAX(s_engage_choices), .def = 0, .choices = s_engage_choices, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
    },
    {
        .key = "ptr_cursor", .section = "input", .group = "Pointer", .type = KVM_VT_ENUM,
        .title = "Your cursor over the picture",
        .help = "Your own cursor over the picture, while you have control in absolute "
                "mode. \"arrow\" is the usual one. \"dot\" is a small dot, so the "
                "target's own cursor is easy to see and you still know where you point. "
                "\"hidden\" leaves only the target's cursor, which trails yours a little.",
        .min = 0, .max = ENUM_MAX(s_cursor_choices), .def = 0, .choices = s_cursor_choices,
        .requires_cap = KVM_CAP_HID, .flags = KVM_SF_PROFILE,
    },
    /* A target with several screens: an absolute pointer addresses the whole
       desktop, not the one screen captured. Read by the console only. */
    {
        .key = "ptr_desk_w", .section = "input", .group = "Several screens", .type = KVM_VT_INT,
        .title = "Whole desktop width",
        .help = "For a target with more than one screen, or one that puts black borders "
                "round its picture. The absolute pointer covers the whole desktop, so with "
                "two screens the cursor runs off onto the other one, and with borders it "
                "lands short of where you click. \"Find this screen\" fills in these four. "
                "By hand, in the picture's pixels: for two 1920x1080 screens side by side "
                "with this one on the right, 3840, 1080, 1920, 0. For a 1440x1080 desktop "
                "in the middle of a 1920x1080 picture, 1440, 1080, -240, 0. 0 here turns it "
                "off.",
        .min = 0, .max = 32767, .def = 0, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .unit = "px",
    },
    {
        .key = "ptr_desk_h", .section = "input", .group = "Several screens", .type = KVM_VT_INT,
        .title = "Whole desktop height",
        .help = "The height of the whole desktop. 0 means the same as this screen.",
        .min = 0, .max = 32767, .def = 0, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .unit = "px",
    },
    {
        .key = "ptr_scr_x", .section = "input", .group = "Several screens", .type = KVM_VT_INT,
        .title = "This screen's left edge",
        .help = "Where this screen starts, from the left of the whole desktop. Below 0 "
                "when the desktop starts to the right of the picture's left edge (black borders).",
        .min = -32767, .max = 32767, .def = 0, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .unit = "px",
    },
    {
        .key = "ptr_scr_y", .section = "input", .group = "Several screens", .type = KVM_VT_INT,
        .title = "This screen's top edge",
        .help = "Where this screen starts, from the top of the whole desktop. Below 0 "
                "when the desktop starts below the picture's top edge (black borders).",
        .min = -32767, .max = 32767, .def = 0, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .unit = "px",
    },
    {
        .key = "mouse_sens", .section = "input", .group = "Pointer", .type = KVM_VT_INT,
        .title = "Relative sensitivity",
        .help = "Scales pointer movement in relative mode only.",
        .min = 10, .max = 400, .def = 100, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .unit = "%",
        .visible_key = "mouse_mode", .visible_val = 1,
    },
    {
        .key = "jiggle_s", .section = "input", .group = "Pointer", .type = KVM_VT_INT,
        .title = "Mouse jiggler",
        .help = "0 turns it off. Otherwise the pointer is nudged one pixel and put straight "
                "back this often, so the target does not lock or fall asleep while you are "
                "watching it. It stands aside whenever you are using the mouse yourself, and "
                "does nothing when no target is attached.",
        .min = 0, .max = 3600, .def = 0, .requires_cap = KVM_CAP_HID,
        .unit = "s",
    },

    {
        .key = "usb_legacy", .section = "input", .group = "Target", .type = KVM_VT_BOOL,
        .title = "Old BIOS keyboard",
        .help = "For an old BIOS that does not see the keyboard. The target then gets one "
                "plain keyboard at USB 1.1 speed, the way a real keyboard looks. The mouse, "
                "media keys and virtual media are off while this is on. Takes effect after "
                "a restart.",
        .def = 0, .requires_cap = KVM_CAP_HID, .flags = KVM_SF_REBOOT | KVM_SF_PROFILE,
    },
    {
        .key = "usb_pad", .section = "input", .group = "Target", .type = KVM_VT_ENUM,
        .title = "Gamepad",
        .help = "A game controller for the target, and a gamepad panel in the console; a "
                "controller plugged into your own computer works through it too. "
                "\"Nintendo Switch\": a HORI Pokken pad. \"Xbox 360 (XInput)\": a "
                "wired Xbox 360 pad, for Windows, a Steam Deck and Linux. \"Original Xbox\": the "
                "original Xbox pad, always alone; the console's controller port needs an "
                "adapter to a USB socket. The pad joins the keyboard and mouse; the "
                "\"pad only\" choices show the pad and nothing else, the way the real one "
                "looks - Windows needs that for the Xbox pad, and a console may ignore a pad "
                "with company. Takes effect after a restart.",
        .min = 0, .max = ENUM_MAX(s_pad_choices), .def = 0, .choices = s_pad_choices,
        .requires_cap = KVM_CAP_HID, .flags = KVM_SF_REBOOT | KVM_SF_PROFILE,
        .labels = s_pad_labels,
    },
    {
        .key = "scroll_inv", .section = "input", .group = "Pointer", .type = KVM_VT_BOOL,
        .title = "Invert scroll wheel",
        .def = 0, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
    },
    {
        .key = "scroll_speed", .section = "input", .group = "Pointer", .type = KVM_VT_INT,
        .title = "Scroll speed",
        .help = "How far the target scrolls for a turn of the wheel or a swipe on a "
                "touchpad. 100 sends one wheel click per notch, 50 one per two notches.",
        .min = 25, .max = 400, .def = 100, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .unit = "%",
    },
    {
        .key = "kbd_layout", .section = "input", .group = "Keyboard", .type = KVM_VT_ENUM,
        .title = "Target keyboard layout",
        .help = "Used when pasting text, so the characters sent match what the target "
                "actually types. A KVM sends key positions, not characters.",
        .min = 0, .max = ENUM_MAX(s_layout_choices), .def = 0, .choices = s_layout_choices, .requires_cap = KVM_CAP_HID,
        .flags = KVM_SF_PROFILE,
        .labels = s_layout_labels,
    },
    {
        .key = "type_delay", .section = "input", .group = "Keyboard", .type = KVM_VT_INT,
        .title = "Paste keystroke delay",
        .help = "Raise it if the target drops characters while text is being pasted.",
        .min = 1, .max = 200, .def = 8, .requires_cap = KVM_CAP_HID,
        .unit = "ms",
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
    {
        /* The buttons someone pinned to the console's bar: a JSON array of
         * {t, v} - a key ("key", "f2"), a built-in combination, a macro or a
         * runbook by name. Kept here so every browser shows the same ones. */
        .key = "quick_json", .section = "macros", .type = KVM_VT_STR,
        .title = "Quick buttons",
        .help = "Buttons pinned to the console's bar, edited from the Input panel.",
        .def_str = "[]", .max_len = 600, .requires_cap = KVM_CAP_HID,
    },
    {
        /* Runbooks, the same shape as the macros: a JSON array of {name, script}
         * the console edits, in a section the settings panel does not render.
         * The size is what one PUT of the settings can carry. */
        .key = "runbooks_json", .section = "runbooks", .type = KVM_VT_STR,
        .title = "Runbooks",
        .help = "Saved runbooks, edited from the Runbooks panel.",
        .def_str = "[]", .max_len = 3000, .requires_cap = KVM_CAP_RUNBOOK,
    },
    {
        .key = "sched_enable", .section = "schedules", .type = KVM_VT_BOOL,
        .title = "Run schedules",
        .help = "Let the device fire scheduled actions. It needs the clock set over the network.",
        .def = 0, .requires_cap = KVM_CAP_SCHED,
    },
    /* The clock. The keys keep their schedule names so saved values carry over;
     * they sit in System because recordings use the clock too. */
    {
        .key = "time_sync", .section = "system", .group = "Clock", .type = KVM_VT_BOOL,
        .title = "Set the clock over the network",
        .help = "Recordings and screenshots are named by the clock, and schedules run by it. "
                "Without this, the console sets the clock when you sign in. Schedules set it "
                "over the network anyway.",
        .def = 0, .requires_cap = KVM_CAP_SCHED,
    },
    {
        .key = "sched_ntp", .section = "system", .group = "Clock", .type = KVM_VT_STR,
        .title = "Time server (NTP)",
        .help = "Where the clock is set from. A name on the local network works with no internet.",
        .def_str = "pool.ntp.org", .max_len = 64, .requires_cap = KVM_CAP_SCHED,
        .visible_key = "time_sync", .visible_val = 1,
    },
    {
        .key = "sched_tz", .section = "system", .group = "Clock", .type = KVM_VT_STR,
        .title = "Time zone",
        .help = "Pick your city; the browser's own zone is at the top of the list. File names and schedules use it. "
                "Stored as a POSIX TZ string, e.g. MSK-3 or CET-1CEST,M3.5.0,M10.5.0/3.",
        .def_str = "UTC0", .max_len = 48, .requires_cap = KVM_CAP_SCHED,
    },
    /* A battery-backed clock chip, for the time after a restart with no
     * network. Auto finds the common ones; the rest must be named. */
    {
        .key = "rtc_chip", .section = "system", .group = "Clock", .type = KVM_VT_ENUM,
        .title = "Clock chip",
        .help = "A battery-backed clock module keeps the time through a restart with no network. "
                "Auto finds a DS3231 (or DS3231M, DS3232) and a PCF8563 or BM8563. A PCF85063 or "
                "PCF8523 shares an address with something else and must be named here. Off leaves "
                "the bus alone. Only the DS3231 has been tried on hardware.",
        .min = 0, .max = ENUM_MAX(s_rtc_chip_choices), .def = 0, .choices = s_rtc_chip_choices,
        .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "rtc_bus", .section = "system", .group = "Clock", .type = KVM_VT_ENUM,
        .title = "Clock chip wiring",
        .help = "Where the module is wired. The capture board's I2C is pins 3 (SDA) and 5 (SCL) "
                "of the 40-pin header on most boards, and a DS3231 for Raspberry Pi plugs "
                "straight on there. Own pins puts it on a second I2C bus.",
        .min = 0, .max = ENUM_MAX(s_rtc_bus_choices), .def = 0, .choices = s_rtc_bus_choices,
        .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_REBOOT | KVM_SF_SHOW_NE,
        .visible_key = "rtc_chip", .visible_val = 1,
    },
    {
        .key = "rtc_sda", .section = "system", .group = "Clock", .type = KVM_VT_INT,
        .title = "Clock chip SDA",
        .help = "The data line of the clock module's own bus.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "rtc_bus", .visible_val = 1,
    },
    {
        .key = "rtc_scl", .section = "system", .group = "Clock", .type = KVM_VT_INT,
        .title = "Clock chip SCL",
        .help = "The clock line of the clock module's own bus.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "rtc_bus", .visible_val = 1,
    },
    {
        /* The schedules themselves, the same shape as macros and runbooks: a
         * JSON array the console edits, in a section the settings panel does
         * not render. */
        .key = "schedules_json", .section = "schedules", .type = KVM_VT_STR,
        .title = "Schedules",
        .help = "Saved schedules, edited from the Automation panel.",
        .def_str = "[]", .max_len = 3000, .requires_cap = KVM_CAP_SCHED,
    },
    {
        .key = "notify_enable", .section = "notify", .type = KVM_VT_BOOL,
        .title = "Send notifications",
        .help = "Send a message when something happens on the target: a watched phrase, a "
                "blank screen, a dashcam clip. The table below picks what goes where.",
        .def = 0, .requires_cap = KVM_CAP_NOTIFY,
    },
    {
        .key = "notify_snap", .section = "notify", .type = KVM_VT_BOOL,
        .title = "Attach a screenshot",
        .help = "Send the screen with the message, where the channel can carry a picture.",
        .def = 1, .requires_cap = KVM_CAP_NOTIFY,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    {
        .key = "notify_log", .section = "notify", .type = KVM_VT_BOOL,
        .title = "Attach recent log",
        .help = "Include the tail of the device log with the message - the context around an alert.",
        .def = 0, .requires_cap = KVM_CAP_NOTIFY,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    {
        .key = "notify_tg_token", .section = "notify", .group = "Where", .type = KVM_VT_STR,
        .title = "Telegram bot token",
        .help = "From @BotFather, like 123456:AA... . Message the bot once so it may write to you.",
        .def_str = "", .max_len = 64, .requires_cap = KVM_CAP_NOTIFY, .flags = KVM_SF_SECRET,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    {
        .key = "notify_tg_chat", .section = "notify", .group = "Where", .type = KVM_VT_STR,
        .title = "Telegram chat id",
        .help = "The chat to message. Find chats lists the ones that wrote to the bot lately; "
                "a group or channel id typed in works too.",
        .def_str = "", .max_len = 32, .requires_cap = KVM_CAP_NOTIFY,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    {
        .key = "notify_url", .section = "notify", .group = "Where", .type = KVM_VT_STR,
        .title = "Webhook URL",
        .help = "A URL to POST a small JSON message to. The body is "
                "{\"title\":\"...\",\"message\":\"...\",\"device\":\"<hostname>\"}, "
                "with a \"log\" field added when the log tail is switched on above, sent as "
                "application/json; anything answering 2xx counts as delivered. Optional.",
        .def_str = "", .max_len = 200, .requires_cap = KVM_CAP_NOTIFY,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    /* What goes where: a mask per channel, a bit per kind of event, in the
       order of kvm_notify_ev_t. A test and the "alerts dropped" note go
       everywhere. The blank screen starts off, as it always did. */
    {
        .key = "notify_tg_ev", .section = "notify", .group = "What goes where", .type = KVM_VT_INT,
        .title = "Telegram",
        .help = "A dashcam clip under 50 MB plays in the chat. A bigger one is named.",
        .min = 0, .max = NOTIFY_EV_ALL, .def = NOTIFY_EV_DEF, .choices = s_notify_ev, .labels = s_notify_ev_labels,
        .requires_cap = KVM_CAP_NOTIFY, .flags = KVM_SF_MASK,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    {
        .key = "notify_hook_ev", .section = "notify", .group = "What goes where", .type = KVM_VT_INT,
        .title = "Webhook",
        .help = "A dashcam clip is named, with its file on the card.",
        .min = 0, .max = NOTIFY_EV_ALL, .def = NOTIFY_EV_DEF, .choices = s_notify_ev, .labels = s_notify_ev_labels,
        .requires_cap = KVM_CAP_NOTIFY, .flags = KVM_SF_MASK,
        .visible_key = "notify_enable", .visible_val = 1,
    },
    {
        .key = "notify_push_ev", .section = "notify", .group = "What goes where", .type = KVM_VT_INT,
        .title = "Browsers",
        .help = "Text only: a push message has no room for a picture.",
        .min = 0, .max = NOTIFY_EV_ALL, .def = NOTIFY_EV_DEF, .choices = s_notify_ev, .labels = s_notify_ev_labels,
        .requires_cap = KVM_CAP_NOTIFY, .flags = KVM_SF_MASK,
        .visible_key = "notify_enable", .visible_val = 1,
    },

    /* ---- storage -------------------------------------------------------- */
    {
        .key = "msc_enable", .section = "storage", .group = "Virtual media", .type = KVM_VT_BOOL,
        .title = "Expose virtual media",
        .help = "Present a USB drive to the target that it can boot from. Off keeps the "
                "device a plain keyboard and mouse, which also frees the USB endpoints the "
                "drive would use. Adding or removing the drive re-enumerates the device to "
                "the target, so it takes effect after a restart; swapping the image once the "
                "drive exists does not.",
        .def = 0, .requires_cap = KVM_CAP_MSC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "msc_mode", .section = "storage", .group = "Virtual media", .type = KVM_VT_ENUM,
        .title = "Media type",
        .help = "How an image is presented to the target. \"Auto\" serves a .iso as a "
                "CD-ROM (so it boots and mounts as an optical drive) and any other file "
                "as a removable disk - the right choice almost always. Force \"CD-ROM\" or "
                "\"Disk\" only if a file is misnamed. Handing over the whole card is picked "
                "in the Media panel, not here. Switching the type re-attaches the USB drive.",
        .min = 0, .max = ENUM_MAX(s_media_choices), .def = 0, .choices = s_media_choices, .requires_cap = KVM_CAP_MSC,
        .labels = s_media_labels,
        .visible_key = "msc_enable", .visible_val = 1,
    },
    {
        .key = "sd_speed", .section = "storage", .group = "microSD card", .type = KVM_VT_ENUM,
        .title = "microSD speed",
        .help = "The fastest clock the card is tried at. \"Auto\" starts at what the board "
                "supports and steps down by itself when the card fails to mount, fails a test "
                "read, or fails a transfer later. Pick a lower one for a card that keeps "
                "stepping down. Takes effect after a restart.",
        .min = 0, .max = ENUM_MAX(s_sd_speed_choices), .def = 0, .choices = s_sd_speed_choices,
        .requires_cap = KVM_CAP_MSC, .flags = KVM_SF_REBOOT,
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
        .key = "pwr_wol_mac", .section = "power", .group = "Wake-on-LAN", .type = KVM_VT_STR,
        .title = "Target MAC for Wake-on-LAN",
        .help = "The target's Ethernet MAC, e.g. AA:BB:CC:DD:EE:FF. The Wake button sends "
                "it a magic packet. The target must have Wake-on-LAN enabled in its BIOS and "
                "keep standby power.",
        .def_str = "", .max_len = 17, .requires_cap = KVM_CAP_WOL,
    },
    {
        .key = "cec_enable", .section = "power", .group = "HDMI-CEC", .type = KVM_VT_BOOL,
        .title = "HDMI-CEC",
        .help = "Talk to the target over the HDMI cable's CEC line: see what is connected, "
                "put it to sleep, wake it and send remote-control keys. Works with TV boxes, "
                "consoles, a Raspberry Pi; most PCs do not speak CEC.",
        .def = 1, .requires_cap = KVM_CAP_CEC,
    },
    {
        .key = "atx_enable", .section = "power", .group = "ATX wiring", .type = KVM_VT_BOOL,
        .title = "Enable ATX control",
        .help = "Requires optocouplers wired to the target's front-panel header.",
        .def = 0, .requires_cap = KVM_CAP_GPIO,
    },
    /* Pins, so the console offers only what is free on this board. The defaults
     * come from Kconfig and are picked per board clear of everything else,
     * because two settings on one GPIO is now refused rather than obeyed. */
    {
        .key = "atx_pwr_gpio", .section = "power", .group = "ATX wiring", .type = KVM_VT_INT,
        .title = "Power button GPIO",
        .help = "Drives the optocoupler across the target's power button. -1 to disable.",
        .min = -1, .max = 54, .def = CONFIG_KVM_ATX_PWR_GPIO, .requires_cap = KVM_CAP_GPIO,
        .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        .key = "atx_rst_gpio", .section = "power", .group = "ATX wiring", .type = KVM_VT_INT,
        .title = "Reset button GPIO",
        .help = "Drives the optocoupler across the target's reset button. -1 to disable.",
        .min = -1, .max = 54, .def = CONFIG_KVM_ATX_RST_GPIO, .requires_cap = KVM_CAP_GPIO,
        .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        .key = "atx_led_gpio", .section = "power", .group = "ATX wiring", .type = KVM_VT_INT,
        .title = "Power LED sense GPIO",
        .help = "Reads the target's power LED through an optocoupler. Wire it to the "
                "power LED, not the HDD LED, which only blinks on disk activity. -1 if "
                "you are not sensing the LED.",
        .min = -1, .max = 54, .def = CONFIG_KVM_ATX_LED_GPIO, .requires_cap = KVM_CAP_GPIO,
        .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        .key = "atx_hdd_gpio", .section = "power", .group = "ATX wiring", .type = KVM_VT_INT,
        .title = "HDD LED sense GPIO",
        .help = "Reads the target's HDD LED the same way as the power LED, and with the "
                "same active-high setting. The power button in the console then blinks "
                "while the disk works. -1 if you are not sensing it.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO,
        .flags = KVM_SF_PIN | KVM_SF_REBOOT,
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        .key = "atx_short_ms", .section = "power", .group = "ATX wiring", .type = KVM_VT_INT,
        .title = "Short press",
        .help = "How long a normal power or reset press is held.",
        .min = 50, .max = 2000, .def = 200, .requires_cap = KVM_CAP_GPIO,
        .unit = "ms",
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        .key = "atx_long_ms", .section = "power", .group = "ATX wiring", .type = KVM_VT_INT,
        .title = "Force-off hold",
        .help = "How long the power button is held for a hard power off.",
        .min = 1000, .max = 15000, .def = 5000, .requires_cap = KVM_CAP_GPIO,
        .unit = "ms",
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        .key = "atx_active_high", .section = "power", .group = "ATX wiring", .type = KVM_VT_BOOL,
        .title = "Buttons active-high",
        .help = "On to press the button when the GPIO drives high (high-level-trigger "
                "optocoupler modules). Turn off if your module presses on a low.",
        .def = 1, .requires_cap = KVM_CAP_GPIO,
        .visible_key = "atx_enable", .visible_val = 1,
    },
    {
        /* Key kept <= 15 chars: NVS rejects longer keys, and "atx_led_active_high"
         * (19) silently failed to persist, reverting to the default every boot. */
        .key = "atx_led_ah", .section = "power", .group = "ATX wiring", .type = KVM_VT_BOOL,
        .title = "Power LED active-high",
        .help = "On if the LED sense reads high when the target is powered. Flip it if "
                "the reported power state is inverted.",
        .def = 1, .requires_cap = KVM_CAP_GPIO,
        .visible_key = "atx_enable", .visible_val = 1,
    },

    /* The buttons on the box - see components/kvm_sched/button.c. Button 1
     * keeps the keys it had when it was the only one. Each next button shows
     * once the one before it has a pin, so an unused list stays one line. */
#define BTN_PRESS_HELP                                                                         \
    "What a short press does: the target's power button, a hard power off, reset, "            \
    "Wake-on-LAN, a runbook (named below), saving the dashcam's last seconds as a clip, or a " \
    "screenshot to the card. On a board with WiFi it can also change the connection, which "   \
    "restarts the device: \"hotspot on/off\" goes to its own hotspot and back to the mode it " \
    "had, \"next network mode\" steps through them all, \"Ethernet / WiFi\" swaps the two. "   \
    "\"next display page\" and \"display on/off\" drive the status display."
#define BTN_HOLD_HELP "What holding it for 1.5 s does. It fires as soon as it has been held long enough, so you know it took."
#define BTN_RUNBOOK_HELP "The name of the runbook, for either action set to \"runbook\"."
#define BTN_HIGH_HELP                                                                      \
    "Off for a button that connects the pin to ground, which is the usual kind and the " \
    "M5Stack Unit Button. On for one that connects it to 3V3."
#define BTN_GPIO_HELP                                                                      \
    "A push button on a free pin: the M5Stack Unit Button on a Grove port (its yellow "    \
    "wire), or any switch to ground. -1 for none. Not tried with real hardware yet."
#define BTN_ROWS(GROUP, K_GPIO, K_PRESS, K_HOLD, K_RB, K_HIGH, GPIO_DEF, PRESS_DEF, HOLD_DEF, PREV, PREV_VAL, PREV_FLAG, DEP) \
    {                                                                                            \
        .key = K_GPIO, .section = "buttons", .group = GROUP, .type = KVM_VT_INT,                 \
        .title = "GPIO", .help = BTN_GPIO_HELP,                                                  \
        .min = -1, .max = 54, .def = GPIO_DEF, .requires_cap = -1, .flags = KVM_SF_PIN | PREV_FLAG, \
        .visible_key = PREV, .visible_val = PREV_VAL,                                            \
    },                                                                                           \
    {                                                                                            \
        .key = K_PRESS, .section = "buttons", .group = GROUP, .type = KVM_VT_ENUM,               \
        .title = "A short press", .help = BTN_PRESS_HELP,                                        \
        .min = 0, .max = ENUM_MAX(s_btn_action_choices), .def = PRESS_DEF,                       \
        .choices = s_btn_action_choices, .requires_cap = -1,                                     \
        .visible_key = DEP, .visible_val = -1, .flags = KVM_SF_SHOW_NE,                          \
    },                                                                                           \
    {                                                                                            \
        .key = K_HOLD, .section = "buttons", .group = GROUP, .type = KVM_VT_ENUM,                \
        .title = "Held for 1.5 s", .help = BTN_HOLD_HELP,                                        \
        .min = 0, .max = ENUM_MAX(s_btn_action_choices), .def = HOLD_DEF,                        \
        .choices = s_btn_action_choices, .requires_cap = -1,                                     \
        .visible_key = DEP, .visible_val = -1, .flags = KVM_SF_SHOW_NE,                          \
    },                                                                                           \
    {                                                                                            \
        .key = K_RB, .section = "buttons", .group = GROUP, .type = KVM_VT_STR,                   \
        .title = "Runbook to run", .help = BTN_RUNBOOK_HELP,                                     \
        .def_str = "", .max_len = 47, .requires_cap = -1,                                        \
        .visible_key = DEP, .visible_val = -1, .flags = KVM_SF_SHOW_NE,                          \
    },                                                                                           \
    {                                                                                            \
        .key = K_HIGH, .section = "buttons", .group = GROUP, .type = KVM_VT_BOOL,                \
        .title = "Reads high when pressed", .help = BTN_HIGH_HELP,                               \
        .def = 0, .requires_cap = -1,                                                            \
        .visible_key = DEP, .visible_val = -1, .flags = KVM_SF_SHOW_NE,                          \
    }
/* How many buttons this board offers, and whether button 1's pin is the
 * board's own (a closed box like the JetKVM Mini). A hidden pin never equals
 * -2, which is what keeps a row out of sight; the rows under a fixed pin hang
 * on nothing, since their pin is never shown. */
#define BTN_ROWS_X(...) BTN_ROWS(__VA_ARGS__) /* expands the *_SHOW lists first */
#if CONFIG_KVM_BTN1_FIXED
#define BTN1_SHOW "btn_gpio", -2, 0, NULL
#define BTN2_PREV NULL /* button 1's pin is never shown, so do not wait for it */
#else
#define BTN1_SHOW NULL, 0, 0, "btn_gpio"
#define BTN2_PREV "btn_gpio"
#endif
#if CONFIG_KVM_BTN_COUNT >= 2
#define BTN2_SHOW BTN2_PREV, -1, KVM_SF_SHOW_NE
#else
#define BTN2_SHOW "btn2_gpio", -2, 0
#endif
#if CONFIG_KVM_BTN_COUNT >= 3
#define BTN3_SHOW "btn2_gpio", -1, KVM_SF_SHOW_NE
#else
#define BTN3_SHOW "btn3_gpio", -2, 0
#endif
#if CONFIG_KVM_BTN_COUNT >= 4
#define BTN4_SHOW "btn3_gpio", -1, KVM_SF_SHOW_NE
#else
#define BTN4_SHOW "btn4_gpio", -2, 0
#endif
    BTN_ROWS_X("Button 1", "btn_gpio", "btn_press", "btn_hold", "btn_runbook", "btn_active_high",
               CONFIG_KVM_BTN_GPIO_DEFAULT, CONFIG_KVM_BTN_PRESS_DEFAULT, CONFIG_KVM_BTN_HOLD_DEFAULT, BTN1_SHOW),
    BTN_ROWS_X("Button 2", "btn2_gpio", "btn2_press", "btn2_hold", "btn2_runbook", "btn2_high", -1, 0, 0,
               BTN2_SHOW, "btn2_gpio"),
    BTN_ROWS_X("Button 3", "btn3_gpio", "btn3_press", "btn3_hold", "btn3_runbook", "btn3_high", -1, 0, 0,
               BTN3_SHOW, "btn3_gpio"),
    BTN_ROWS_X("Button 4", "btn4_gpio", "btn4_press", "btn4_hold", "btn4_runbook", "btn4_high", -1, 0, 0,
               BTN4_SHOW, "btn4_gpio"),

    {
        .key = "ser_enable", .section = "power", .group = "Serial console", .type = KVM_VT_BOOL,
        .title = "Serial console",
        .help = "The target's serial port, for a machine with no screen at all - a NAS, a "
                "router, a server - or one that shows its BIOS over serial. Wire the target's "
                "TX to the RX pin below and its RX to the TX pin, plus ground. 3.3 V logic "
                "(a Raspberry Pi, a router's header) goes straight in; a real RS-232 port "
                "(9-pin, +-12 V) needs a MAX3232 module in between, or it damages the pin.",
        .def = 0, .requires_cap = KVM_CAP_GPIO,
    },
    {
        .key = "usb_serial", .section = "power", .group = "Serial console", .type = KVM_VT_ENUM,
        .title = "Serial port over USB",
        .help = "A serial port on the USB cable the target already has, so no wires. Linux "
                "sees it as /dev/ttyACM0, Windows as a COM port, macOS as /dev/cu.usbmodem*. "
                "\"Serial console\": the serial console uses it instead of the pins below; on Linux "
                "run a login on it with \"systemctl enable --now serial-getty@ttyACM0\". "
                "\"Agent\": it is for espkvm-agent, a small program on the target that shows "
                "what the machine is and shares its clipboard. Not together with a gamepad. "
                "Takes effect after a restart.",
        .min = 0, .max = ENUM_MAX(s_usb_serial_choices), .def = 0, .choices = s_usb_serial_choices,
        .requires_cap = KVM_CAP_HID, .flags = KVM_SF_REBOOT,
        .labels = s_usb_serial_labels,
    },
    {
        .key = "ser_tx", .section = "power", .group = "Serial console", .type = KVM_VT_INT,
        .title = "TX pin (to the target's RX)",
        .help = "The GPIO this device sends on. -1 until you pick one.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN,
        .visible_key = "ser_enable", .visible_val = 1,
    },
    {
        .key = "ser_rx", .section = "power", .group = "Serial console", .type = KVM_VT_INT,
        .title = "RX pin (from the target's TX)",
        .help = "The GPIO this device listens on. -1 until you pick one.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN,
        .visible_key = "ser_enable", .visible_val = 1,
    },
    {
        .key = "ser_baud", .section = "power", .group = "Serial console", .type = KVM_VT_ENUM,
        .title = "Speed (baud)",
        .help = "What the target's console runs at; 115200 for Linux and most boards, 9600 "
                "or 115200 for a PC BIOS. Always 8 data bits, no parity, one stop bit.",
        .min = 0, .max = ENUM_MAX(s_baud_choices), .def = 4, .choices = s_baud_choices,
        .requires_cap = KVM_CAP_GPIO,
        .visible_key = "ser_enable", .visible_val = 1,
    },

    /* ---- audio ---------------------------------------------------------- */
    {
        .key = "aud_enable", .section = "audio", .group = "HDMI audio", .type = KVM_VT_BOOL,
        .title = "HDMI audio",
        .help = "The capture board sends the HDMI sound as I2S on three wires: bit clock "
                "(BCLK or SCK), word clock (WS, LRCK or WFS) and data (SD or DATA). Wire them "
                "to three free pins, plus ground, and pick those pins below. On the Geekworm "
                "C790 they are on the J4 header.",
        .def = 0, .requires_cap = KVM_CAP_GPIO,
    },
    {
        .key = "aud_bclk", .section = "audio", .group = "HDMI audio", .type = KVM_VT_INT,
        .title = "BCLK pin (bit clock)",
        .help = "-1 until you pick one.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN,
        .visible_key = "aud_enable", .visible_val = 1,
    },
    {
        .key = "aud_ws", .section = "audio", .group = "HDMI audio", .type = KVM_VT_INT,
        .title = "WS pin (word clock)",
        .help = "-1 until you pick one.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN,
        .visible_key = "aud_enable", .visible_val = 1,
    },
    {
        .key = "aud_din", .section = "audio", .group = "HDMI audio", .type = KVM_VT_INT,
        .title = "DATA pin",
        .help = "-1 until you pick one.",
        .min = -1, .max = 54, .def = -1, .requires_cap = KVM_CAP_GPIO, .flags = KVM_SF_PIN,
        .visible_key = "aud_enable", .visible_val = 1,
    },

    /* ---- network -------------------------------------------------------- */
    {
        .key = "net_hostname", .section = "network", .group = "Address", .type = KVM_VT_STR,
        .title = "Hostname",
        .help = "Also the mDNS name: <hostname>.local",
        .def_str = CONFIG_KVM_MDNS_HOSTNAME, .max_len = 31, .requires_cap = -1,
        .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_mdns_ident", .section = "network", .group = "Address", .type = KVM_VT_BOOL,
        .title = "Say on the network that this is an ESP-KVM",
        .help = "Adds the product and the board to the mDNS announcement, and an "
                "_espkvm._tcp service, so a discovery tool or a script can find every "
                "ESP-KVM on the network. Off, only the hostname is announced. The "
                "firmware version is never announced.",
        .def = 0, .requires_cap = -1, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_dhcp", .section = "network", .group = "Address", .type = KVM_VT_BOOL,
        .title = "Obtain address automatically",
        .def = 1, .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "net_ip", .section = "network", .group = "Address", .type = KVM_VT_STR,
        .title = "Static address", .def_str = "", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
        .visible_key = "net_dhcp", .visible_val = 0,
    },
    {
        .key = "net_mask", .section = "network", .group = "Address", .type = KVM_VT_STR,
        .title = "Netmask", .def_str = "255.255.255.0", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
        .visible_key = "net_dhcp", .visible_val = 0,
    },
    {
        .key = "net_gw", .section = "network", .group = "Address", .type = KVM_VT_STR,
        .title = "Gateway", .def_str = "", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
        .visible_key = "net_dhcp", .visible_val = 0,
    },
    {
        .key = "net_dns", .section = "network", .group = "Address", .type = KVM_VT_STR,
        .title = "DNS server", .def_str = "", .max_len = 15,
        .requires_cap = KVM_CAP_NET_STATIC, .flags = KVM_SF_REBOOT,
        .visible_key = "net_dhcp", .visible_val = 0,
    },
    {
        .key = "net_ipv6", .section = "network", .group = "Address", .type = KVM_VT_BOOL,
        .title = "IPv6",
        .help = "Also answer on an IPv6 address, alongside IPv4. There is nothing to "
                "configure: the address comes from the router's advertisements. Turn it "
                "off to keep the device off IPv6 entirely.",
        .def = 1, .requires_cap = -1, .flags = KVM_SF_REBOOT,
    },

    /* ---- wifi (boards with an ESP32-C6 co-processor only) ---------------- */
    {
        .key = "net_mode", .section = "network", .group = "WiFi", .type = KVM_VT_ENUM,
        .title = "Connection",
        .help = "\"Ethernet\": the wired port. \"WiFi\": join the network below "
                "(Ethernet is left down). \"Own hotspot\": the device makes its own WiFi hotspot "
                "for setup where there is no network to join. \"Ethernet, WiFi as backup\": the wired port, "
                "with the WiFi network below joined and waiting - when the cable is "
                "pulled the device moves to WiFi, and back when the cable returns. It "
                "answers on both addresses. If WiFi is unreachable: reset the board, then hold "
                "the button for two seconds - that returns it to Ethernet, and "
                "clears the password too. Hold it AFTER the reset, not through it.",
        .min = 0, .max = ENUM_MAX(s_netmode_choices),
#if CONFIG_KVM_ETH_ENABLE
        .def = 0,
#else
        /* No wired port on this board: booting into "ethernet" would leave no
         * console at all, so it starts as its own hotspot and WiFi is set from
         * there. The button reset returns here for the same reason. */
        .def = 2,
#endif
        .choices = s_netmode_choices,
        .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT,
        .labels = s_netmode_labels,
    },
    {
        .key = "wifi_ssid", .section = "network", .group = "WiFi", .type = KVM_VT_STR,
        .title = "WiFi network (SSID)",
        .help = "The name of the network to join in WiFi mode.",
        .def_str = "", .max_len = 32, .requires_cap = KVM_CAP_WIFI,
        .flags = KVM_SF_REBOOT | KVM_SF_SHOW_NE,
        .visible_key = "net_mode", .visible_val = 0,
    },
    {
        .key = "wifi_pass", .section = "network", .group = "WiFi", .type = KVM_VT_STR,
        .title = "WiFi password",
        .help = "Left blank for an open network. Stored write-only.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_WIFI,
        .flags = KVM_SF_SECRET | KVM_SF_REBOOT | KVM_SF_SHOW_NE,
        .visible_key = "net_mode", .visible_val = 0,
    },
    {
        .key = "ap_open", .section = "network", .group = "WiFi", .type = KVM_VT_BOOL,
        .title = "Open hotspot (no password)",
        .help = "Run the device's hotspot with no password at all. Off, and with "
                "no password set, the device makes one up on first use and prints "
                "it in the log and on the display - an open network anybody in the "
                "building can join is a poor way to reach a server. Turn this on "
                "only where that is what you want.",
        .def = 0, .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "ap_pass", .section = "network", .group = "WiFi", .type = KVM_VT_STR,
        .title = "Hotspot password",
        .help = "Password for the device's own hotspot (\"Own hotspot\" mode, or the rescue "
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
        .key = "net_fallback", .section = "network", .group = "WiFi", .type = KVM_VT_ENUM,
        .title = "If WiFi can't connect",
        .help = "\"Keep trying\": keep retrying the network - it reconnects on its own "
                "when the network comes back (best for a device you cannot reach "
                "physically). \"Also start a hotspot\": ALSO run a rescue hotspot (ESP-KVM-xxxx) the "
                "whole time WiFi is trying, so you can always reach the device "
                "on-site to fix its settings - set a Hotspot password first. Only "
                "applies in WiFi (station) mode.",
        .min = 0, .max = ENUM_MAX(s_fallback_choices), .def = 0, .choices = s_fallback_choices,
        .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT | KVM_SF_SHOW_NE,
        .labels = s_fallback_labels,
        .visible_key = "net_mode", .visible_val = 0,
    },

    {
        .key = "setup_ap", .section = "network", .group = "WiFi", .type = KVM_VT_BOOL,
        .title = "Setup hotspot on an unclaimed device",
        .help = "While no password has been set and no network cable is plugged "
                "in, put out an open hotspot (ESP-KVM-xxxx) so the device can be "
                "reached and given one. It stops happening the moment a password "
                "exists. Until then a visitor can only set that password - video, "
                "keyboard and every other endpoint stay shut.",
        .def = 1, .requires_cap = KVM_CAP_WIFI, .flags = KVM_SF_REBOOT,
    },

    /* ---- vpn / wireguard ------------------------------------------------- */
    {
        .key = "nc_enable", .section = "network", .group = "Netconsole", .type = KVM_VT_BOOL,
        .title = "Receive the target's log",
        .help = "Listen for the target's kernel messages over the network: Linux netconsole, "
                "or anything that sends plain syslog over UDP. It keeps working when the "
                "target's disk is gone, which is when you want it. On the target, for example: "
                "modprobe netconsole netconsole=@/,6666@<this device's IP>/. What arrives is "
                "unauthenticated UDP - anyone on the network can read it or send lines - so it "
                "is kept as text and can only raise a notification, never run anything.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "nc_port", .section = "network", .group = "Netconsole", .type = KVM_VT_INT,
        .title = "UDP port",
        .help = "6666 is netconsole's usual port; 514 is syslog's.",
        .min = 1, .max = 65535, .def = 6666, .requires_cap = -1,
        .visible_key = "nc_enable", .visible_val = 1,
    },
    {
        .key = "nc_from", .section = "network", .group = "Netconsole", .type = KVM_VT_STR,
        .title = "Accept from",
        .help = "The target's IP address; several separated by commas. Empty accepts lines "
                "from anyone on the network.",
        .def_str = "", .max_len = 95, .requires_cap = -1,
        .visible_key = "nc_enable", .visible_val = 1,
    },
    {
        .key = "nc_match", .section = "network", .group = "Netconsole", .type = KVM_VT_STR,
        .title = "Alert on",
        .help = "Comma-separated phrases, matched without regard to case. A line holding one "
                "sends a notification, at most one every 30 seconds. Empty for none.",
        .def_str = "Kernel panic,Oops,BUG:,Call Trace", .max_len = 127, .requires_cap = -1,
        .visible_key = "nc_enable", .visible_val = 1,
    },
    {
        .key = "wg_enable", .section = "vpn", .group = "WireGuard", .type = KVM_VT_BOOL,
        .title = "Enable WireGuard",
        .help = "Bring up a classic WireGuard tunnel to a hub so the device is "
                "reachable over the VPN. Off by default. Split-tunnel: only the "
                "tunnel subnet goes through WireGuard, so the console stays "
                "reachable on the LAN too. An alternative to Tailscale below, not "
                "a companion - enable one.",
        .def = 0, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_address", .section = "vpn", .group = "WireGuard", .type = KVM_VT_STR,
        .title = "Tunnel address",
        .help = "The device's own IP on the WireGuard network, e.g. 10.9.0.2.",
        .def_str = "", .max_len = 31, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_private_key", .section = "vpn", .group = "WireGuard", .type = KVM_VT_STR,
        .title = "Private key",
        .help = "Base64 WireGuard private key. Leave empty and the device "
                "generates one on first connect; its public key is shown below.",
        .def_str = "", .max_len = 47, .flags = KVM_SF_SECRET, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_peer_key", .section = "vpn", .group = "WireGuard", .type = KVM_VT_STR,
        .title = "Peer public key",
        .help = "Base64 public key of the WireGuard peer (the hub/server).",
        .def_str = "", .max_len = 47, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_endpoint", .section = "vpn", .group = "WireGuard", .type = KVM_VT_STR,
        .title = "Peer endpoint",
        .help = "host:port of the peer, e.g. vpn.example.com:51820.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_keepalive", .section = "vpn", .group = "WireGuard", .type = KVM_VT_INT,
        .title = "Persistent keepalive",
        .help = "Keeps a NAT/firewall mapping open. 25 is typical; 0 disables it.",
        .min = 0, .max = 65535, .def = 25, .requires_cap = KVM_CAP_WG,
        .unit = "s",
    },
    {
        .key = "wg_sntp", .section = "vpn", .group = "WireGuard", .type = KVM_VT_BOOL,
        .title = "Sync time over SNTP",
        .help = "WireGuard handshakes carry a timestamp; without a real clock a "
                "reboot can make the peer reject them. Turn this on if the device "
                "can reach an NTP server. Off by default (isolated networks).",
        .def = 0, .requires_cap = KVM_CAP_WG,
    },
    {
        .key = "wg_sntp_srv", .section = "vpn", .group = "WireGuard", .type = KVM_VT_STR,
        .title = "NTP server",
        .help = "Used only when SNTP is on.",
        .def_str = "pool.ntp.org", .max_len = 47, .requires_cap = KVM_CAP_WG,
        .visible_key = "wg_sntp", .visible_val = 1,
    },

    /* ---- vpn / tailscale ------------------------------------------------- */
    {
        .key = "ts_enable", .section = "vpn", .group = "Tailscale", .type = KVM_VT_BOOL,
        .title = "Enable Tailscale",
        .help = "Join a Tailscale network natively - the device gets a 100.x "
                "address reachable from anywhere on your tailnet, with NAT "
                "traversal handled for you and no separate gateway. Off by "
                "default. An alternative to WireGuard above, not a companion; "
                "enabling both is unusual.",
        .def = 0, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_auth_key", .section = "vpn", .group = "Tailscale", .type = KVM_VT_STR,
        .title = "Auth key",
        .help = "Optional. A Tailscale auth key (tskey-auth-...) that authorises this "
                "device to join. Empty, the device shows a link to sign in with your "
                "Tailscale account instead. Generate a key in the Tailscale admin "
                "console; a reusable key survives re-registration across reboots.",
        .def_str = "", .max_len = 63, .flags = KVM_SF_SECRET, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_hostname", .section = "vpn", .group = "Tailscale", .type = KVM_VT_STR,
        .title = "Tailnet hostname",
        .help = "The name this device takes on the tailnet. Empty uses the mDNS "
                "hostname from the Network section.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_only", .section = "vpn", .group = "Tailscale", .type = KVM_VT_BOOL,
        .title = "Console only over Tailscale",
        .help = "While the device is on the tailnet, the console answers only to "
                "tailnet addresses (100.64.0.0/10). The setup hotspot still works, and "
                "while Tailscale is down the console answers everywhere again, so a "
                "broken tailnet cannot lock you out.",
        .def = 0, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_trust", .section = "vpn", .group = "Tailscale", .type = KVM_VT_STR,
        .title = "Sign in these tailnet users",
        .help = "Tailscale logins, separated by commas (alice@example.com). A browser "
                "on a device of one of these users is signed in as the owner without a "
                "password or a code. Works once a password of your own is set. Signing "
                "out there signs straight back in.",
        .def_str = "", .max_len = 127, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_trust_view", .section = "vpn", .group = "Tailscale", .type = KVM_VT_STR,
        .title = "Let these tailnet users watch",
        .help = "Tailscale logins, separated by commas. These are signed in as the "
                "viewer: they see the screen and cannot type, click or change anything.",
        .def_str = "", .max_len = 127, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_control_url", .section = "vpn", .group = "Tailscale", .type = KVM_VT_STR,
        .title = "Control server",
        .help = "Coordination server for a self-hosted control plane (Headscale, "
                "Ionscale). Empty uses Tailscale's own (controlplane.tailscale.com). "
                "A host (headscale.example.com) or a URL (https://headscale.example.com:8443), "
                "which then sets TLS and the port too.",
        .def_str = "", .max_len = 63, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_control_port", .section = "vpn", .group = "Tailscale", .type = KVM_VT_INT,
        .title = "Control server port",
        .help = "Port for the control server. 0 = default (443 with TLS, else 80). "
                "Set only if your Headscale listens on a non-standard port.",
        .def = 0, .min = 0, .max = 65535, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_ctrl_tls", .section = "vpn", .group = "Tailscale", .type = KVM_VT_BOOL,
        .title = "Control plane over TLS",
        .help = "Reach the coordination server over HTTPS. Required for the hosted "
                "Tailscale service (the default). Turn off only for a self-hosted "
                "Headscale served over plain HTTP.",
        .def = 1, .requires_cap = KVM_CAP_TS,
    },
    {
        .key = "ts_key_warn", .section = "vpn", .group = "Tailscale", .type = KVM_VT_INT,
        .title = "Warn before the key runs out",
        .help = "Send a notification this many days before the tailnet key expires. "
                "Tailscale gives a node six months at most, and when that runs out the "
                "device drops off the tailnet until someone authorises it again. 0 turns "
                "the warning off. Needs notifications set up and the clock in sync.",
        .def = 14, .min = 0, .max = 90, .requires_cap = KVM_CAP_TS,
        .unit = "days",
    },
    {
        .key = "ts_cert", .section = "vpn", .group = "Tailscale", .type = KVM_VT_BOOL,
        .title = "Trusted certificate (Let's Encrypt)",
        .help = "Get a Let's Encrypt certificate for the device's tailnet name "
                "(host.tailnet.ts.net) and renew it on its own, so a browser trusts the "
                "console with nothing imported. Needs MagicDNS and HTTPS certificates "
                "turned on in the Tailscale admin console (DNS page). The name is "
                "published in Certificate Transparency logs. The local name and the IP "
                "addresses keep the device's own certificate.",
        .def = 0, .requires_cap = KVM_CAP_TS,
    },

    /* ---- mqtt / home assistant ------------------------------------------ */
    {
        .key = "mqtt_enable", .section = "mqtt", .group = "Broker", .type = KVM_VT_BOOL,
        .title = "Publish to MQTT",
        .help = "Report status to an MQTT broker and appear in Home Assistant "
                "(auto-discovered). Off by default; costs nothing when off.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "mqtt_host", .section = "mqtt", .group = "Broker", .type = KVM_VT_STR,
        .title = "Broker host",
        .help = "Hostname or IP of the MQTT broker, e.g. the Home Assistant host.",
        .def_str = "", .max_len = 63, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_port", .section = "mqtt", .group = "Broker", .type = KVM_VT_INT,
        .title = "Broker port",
        .help = "1883 for plain MQTT, 8883 for MQTT over TLS.",
        .min = 1, .max = 65535, .def = 1883, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_tls", .section = "mqtt", .group = "Broker", .type = KVM_VT_BOOL,
        .title = "Use TLS",
        .help = "Connect with mqtts. Set the port to 8883 as well.",
        .def = 0, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_verify", .section = "mqtt", .group = "Broker", .type = KVM_VT_BOOL,
        .title = "Verify broker certificate",
        .help = "With TLS on, check the broker's certificate against the built-in "
                "CA bundle (public CAs, e.g. Let's Encrypt). Turn off for a broker "
                "with a self-signed certificate.",
        .def = 1, .requires_cap = -1,
        .visible_key = "mqtt_tls", .visible_val = 1,
    },
    {
        .key = "mqtt_user", .section = "mqtt", .group = "Broker", .type = KVM_VT_STR,
        .title = "Username",
        .help = "Leave empty for an anonymous broker.",
        .def_str = "", .max_len = 47, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_pass", .section = "mqtt", .group = "Broker", .type = KVM_VT_STR,
        .title = "Password",
        .help = "Stored on the device; never sent back to the console.",
        .def_str = "", .max_len = 63, .flags = KVM_SF_SECRET, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_base", .section = "mqtt", .group = "What it publishes", .type = KVM_VT_STR,
        .title = "Base topic",
        .help = "Topic prefix; the device id is appended, e.g. espkvm/a1b2c3.",
        .def_str = "espkvm", .max_len = 31, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_disco", .section = "mqtt", .group = "What it publishes", .type = KVM_VT_STR,
        .title = "Discovery prefix",
        .help = "Home Assistant MQTT discovery prefix. Default suits a stock HA.",
        .def_str = "homeassistant", .max_len = 31, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_interval", .section = "mqtt", .group = "What it publishes", .type = KVM_VT_INT,
        .title = "Publish interval",
        .help = "How often telemetry is published.",
        .min = 5, .max = 3600, .def = 30, .requires_cap = -1,
        .unit = "s",
        .visible_key = "mqtt_enable", .visible_val = 1,
    },
    {
        .key = "mqtt_snap", .section = "mqtt", .group = "What it publishes", .type = KVM_VT_BOOL,
        .title = "Send a picture with a screen alert",
        .help = "When the screen watch fires, publish a still of the screen as a "
                "camera in Home Assistant, so the notification carries what the "
                "machine actually shows. The button is always there; this only "
                "decides whether an alert takes a picture by itself. Works on "
                "either codec, and a 1080p frame is a few hundred kilobytes over "
                "the broker.",
        .def = 0, .requires_cap = -1,
        .visible_key = "mqtt_enable", .visible_val = 1,
    },

    /* ---- security ------------------------------------------------------- */
    {
        .key = "sec_https", .section = "security", .group = "Access", .type = KVM_VT_BOOL,
        .title = "Serve over HTTPS",
        .help = "Uses a self-signed certificate generated on first boot. Disable only "
                "on a trusted network or behind a VPN.",
        .def = 1, .requires_cap = KVM_CAP_HTTPS, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "sec_auth", .section = "security", .group = "Access", .type = KVM_VT_BOOL,
        .title = "Require login",
        .def = 1, .requires_cap = KVM_CAP_HTTPS, .flags = KVM_SF_REBOOT,
    },
    {
        .key = "sec_user", .section = "security", .group = "Access", .type = KVM_VT_STR,
        .title = "Username", .def_str = "admin", .max_len = 31, .requires_cap = KVM_CAP_HTTPS,
    },
    {
        .key = "agent_api", .section = "security", .group = "Access", .type = KVM_VT_BOOL,
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
        .key = "upd_check", .section = "system", .group = "Updates", .type = KVM_VT_BOOL,
        .title = "Offer firmware updates",
        .help = "The browser asks the address below whether a newer build exists and offers to "
                "install it. With the project's own address it also tells check.espkvm.io, at "
                "most once a day, this device's random id, its board, its version and the chip revision, so "
                "the author can count the devices in use. The id is not taken from the hardware; "
                "a settings reset makes a new one. The device never reaches out on its own - a "
                "KVM that phones home is not what belongs in an isolated network.",
        .def = 0, .requires_cap = KVM_CAP_OTA,
    },
    {
        .key = "upd_url", .section = "system", .group = "Updates", .type = KVM_VT_STR,
        .title = "Update manifest",
        .help = "URL of a manifest.json describing the newest build. The project's own builds "
                "are published at the default address; point it at your fork, or at a file "
                "server inside your network, and it will use that instead. Whatever it points "
                "at is what gets written to the device, so point it somewhere you trust.",
        .def_str = CONFIG_KVM_UPDATE_URL,
        .max_len = 200, .requires_cap = KVM_CAP_OTA,
        .visible_key = "upd_check", .visible_val = 1,
    },

    {
        .key = "fw_fetch", .section = "system", .group = "Updates", .type = KVM_VT_BOOL,
        .title = "Let the device fetch releases itself",
        .help = "Off by default, and deliberately so: a KVM often sits where nothing is "
                "supposed to reach the internet, and it does not talk to GitHub unless it is "
                "told to. Turn it on and the console can offer any published release - which "
                "is how you go back to an earlier one, since the browser is not allowed to "
                "download the image itself. Ordinary updates do not need this; the browser "
                "fetches those and hands them over.",
        .def = 0, .requires_cap = KVM_CAP_OTA,
        .visible_key = "upd_check", .visible_val = 1,
    },

    {
        .key = "therm_guard", .section = "system", .group = "Thermal", .type = KVM_VT_BOOL,
        .title = "Thermal protection",
        .help = "Cap the frame rate when the chip gets warm and stop encoding if it gets hot. "
                "Keyboard, mouse and the web interface keep running either way - a KVM that "
                "stops accepting keystrokes because it is warm has failed at its job.",
        .def = 1, .requires_cap = -1,
    },
    {
        .key = "therm_warn", .section = "system", .group = "Thermal", .type = KVM_VT_INT,
        .title = "Warm threshold",
        .help = "Above this the frame rate is halved. Measured on this board: 1080p MJPEG at "
                "full rate settles around 46 C in open air, so the default leaves plenty of "
                "room before anything is given up.",
        .min = 35, .max = 100, .def = 70, .requires_cap = -1,
        .unit = "°C",
        .visible_key = "therm_guard", .visible_val = 1,
    },
    {
        .key = "therm_stop", .section = "system", .group = "Thermal", .type = KVM_VT_INT,
        .title = "Hot threshold",
        .help = "Above this encoding stops until the chip cools. The ESP32 family is rated to "
                "85 C ambient and the die runs hotter than the air around it.",
        .min = 40, .max = 110, .def = 85, .requires_cap = -1,
        .unit = "°C",
        .visible_key = "therm_guard", .visible_val = 1,
    },

    {
        .key = "log_level", .section = "system", .group = "Console", .type = KVM_VT_ENUM,
        .title = "Log verbosity",
        .min = 0, .max = ENUM_MAX(s_log_choices), .def = 2, .choices = s_log_choices, .requires_cap = -1,
        .labels = s_log_labels,
    },
    {
        .key = "ui_side", .section = "ui", .group = "Layout", .type = KVM_VT_ENUM,
        .title = "Panel side",
        .help = "Which side of the screen the button rail and its panels sit on.",
        .min = 0, .max = ENUM_MAX(s_side_choices), .def = 0, .choices = s_side_choices, .requires_cap = -1,
    },
    {
        .key = "ui_keys", .section = "ui", .group = "Layout", .type = KVM_VT_BOOL,
        .title = "Show the keys sent",
        .help = "The last few keys and key combinations that went to the target, in the "
                "bottom bar. Handy when the target seems to ignore a key, or to show "
                "someone what you press.",
        .def = 0, .requires_cap = -1,
    },
    {
        .key = "ui_fs_hide", .section = "ui", .group = "Full screen", .type = KVM_VT_BOOL,
        .title = "Hide the bars in full screen",
        .help = "In full screen the status strip, the rail and the bottom bar slide away "
                "and the picture takes the whole screen. They come back over it with the "
                "mouse at an edge (after a moment while you have control), the small tab "
                "at the top, or a tap of the right Ctrl key on its own, and go again a few "
                "seconds after you leave them.",
        .def = 1, .requires_cap = -1,
        .flags = KVM_SF_PROFILE,
    },
    {
        .key = "ui_touch_kbd", .section = "ui", .group = "Touch controls", .type = KVM_VT_ENUM,
        .title = "Keyboard button",
        .help = "What the keyboard button under the picture opens in touch mode. "
                "\"Console\" is the console's own keyboard, with Ctrl, Alt, the F keys and "
                "the arrows a phone's keyboard does not have. \"Phone\" is the phone's own "
                "keyboard, for typing text with its layouts and suggestions.",
        .min = 0, .max = ENUM_MAX(s_touch_kbd_choices), .def = 0, .choices = s_touch_kbd_choices,
        .requires_cap = -1,
        .flags = KVM_SF_PROFILE,
    },
    {
        .key = "ui_hidden", .section = "ui", .group = "Buttons", .type = KVM_VT_STR,
        .title = "Shown in the console",
        .help = "Untick a control to take its button out of the console, for everyone who "
                "signs in. It only hides the button: the feature itself stays as its own "
                "settings have it. Stored as the ids of the hidden controls; the console "
                "keeps the list of what can be hidden.",
        .def_str = "", .max_len = 255, .requires_cap = -1,
        .flags = KVM_SF_PROFILE,
    },
    /* Vibration on a phone. Read by the console only; the device keeps the
       values so every phone that signs in gets the same feel. */
    {
        .key = "ui_haptic_keys", .section = "ui", .group = "Vibration on a phone", .type = KVM_VT_BOOL,
        .title = "On key presses",
        .help = "A short tick under the finger on each press of the on-screen keyboard, the "
                "arrow keys and the gamepad buttons, like a phone's own keyboard. Android "
                "only: Safari on an iPhone cannot vibrate.",
        .def = 1, .requires_cap = -1,
    },
    {
        .key = "ui_haptic_pad", .section = "ui", .group = "Vibration on a phone", .type = KVM_VT_BOOL,
        .title = "On the touchpad",
        .help = "In touch mode, a faint tick every few millimetres the finger travels, the "
                "way the touchpads of the Steam Controller feel. Half the strength of a key "
                "press.",
        .def = 1, .requires_cap = -1,
    },
    {
        .key = "ui_haptic_level", .section = "ui", .group = "Vibration on a phone", .type = KVM_VT_ENUM,
        .title = "Strength",
        .help = "A browser cannot set how strong a vibration is, only how long, so stronger "
                "means a longer tick: about 12, 25 or 45 ms.",
        .min = 0, .max = ENUM_MAX(s_haptic_choices), .def = 0, .choices = s_haptic_choices, .requires_cap = -1,
    },

    /* ---- status display ------------------------------------------------- */
    {
        .key = "disp_enable", .section = "display", .group = "Screen", .type = KVM_VT_BOOL,
        .title = "Status display",
        .help = "Drive a small display that shows the IP, link, capture status and "
                "health. An I2C OLED (SSD1306/SH1106) shares the capture chip's I2C "
                "and is auto-detected with no extra pins; an SPI LCD (GC9A01, ST7789) "
                "uses the GPIOs set below, after you pick it as the display type. "
                "Off by default; does nothing when no panel is connected.",
        .def = DISP_DEFAULT_ON, .requires_cap = -1,
    },
    {
        .key = "disp_type", .section = "display", .group = "Screen", .type = KVM_VT_ENUM,
        .title = "Panel",
        .help = "Which panel is wired, controller and size together. SSD1306 and SH1106 "
                "are I2C OLEDs - pick SH1106 if the image is shifted by two pixels or "
                "wraps. GC9A01 is a round colour SPI LCD, e.g. the Waveshare 1.28\" "
                "module. ST7789 is the square colour LCD built into the JetKVM Mini. None of this can be detected: one chip drives every size and "
                "reports none of it. A wrong size shows the picture squeezed into part "
                "of the glass, or every other row missing. Shorter panels show fewer "
                "status lines.",
        .min = 0, .max = ENUM_MAX(s_display_choices), .def = CONFIG_KVM_DISP_DEFAULT_TYPE, .choices = s_display_choices, .requires_cap = -1,
        .visible_key = "disp_enable", .visible_val = DISP_TYPE_SHOW,
    },
    {
        .key = "disp_bright", .section = "display", .group = "Screen", .type = KVM_VT_INT,
        .title = "Brightness",
        .help = "How bright the backlight of a colour SPI LCD is. Takes effect at once. "
                "Needs the backlight pin set, and does nothing on the OLEDs.",
        .min = 5, .max = 100, .def = CONFIG_KVM_DISP_BRIGHT_DEFAULT, .unit = "%",
        .requires_cap = -1, .flags = DISP_COLOUR_FLAGS,
        .visible_key = DISP_COLOUR_KEY, .visible_val = DISP_COLOUR_VAL,
    },
    {
        .key = "disp_dwell", .section = "display", .group = "Screen", .type = KVM_VT_INT,
        .title = "Each page for",
        .help = "How long each status page stays up before the next one. 0 keeps the page "
                "until you press the display button.",
        .min = 0, .max = 60, .def = 4, .unit = "s", .requires_cap = -1,
        .visible_key = "disp_enable", .visible_val = 1,
    },
    {
        .key = "disp_picture", .section = "display", .group = "Screen", .type = KVM_VT_BOOL,
        .title = "Picture page",
        .help = "Adds a page with a small copy of the target's screen. It redraws ten "
                "times a second, or twice while a browser is watching. Colour panels "
                "only.",
        .def = 0, .requires_cap = -1, .flags = DISP_COLOUR_FLAGS,
        .visible_key = DISP_COLOUR_KEY, .visible_val = DISP_COLOUR_VAL,
    },
    {
        .key = "disp_rotate_180", .section = "display", .group = "Screen", .type = KVM_VT_BOOL,
        .title = "Upside down",
        .help = "Turn the picture on the status display through 180 degrees, for a panel "
                "mounted the other way up because of where its connector or its enclosure "
                "put it. Takes effect at once: the panel is re-initialised where it stands, "
                "with no restart.",
        .def = 0, .requires_cap = -1,
        .visible_key = "disp_enable", .visible_val = 1,
    },
    /* GC9A01 SPI pins (the I2C OLEDs need none - they share the capture bus).
     * Pins, so the console offers only free GPIOs; a restart re-attaches on the
     * new wiring. Defaults are a sane free set on the P4; set them to your board. */
    {
        .key = "disp_sclk", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "LCD SCLK / CLK",
        .help = "SPI clock GPIO for the SPI LCD. Ignored by the I2C OLEDs.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_SCLK_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_PIN_SHOW, /* SPI LCDs */
    },
    {
        .key = "disp_mosi", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "LCD MOSI / DIN",
        .help = "SPI data GPIO for the SPI LCD.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_MOSI_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_PIN_SHOW, /* SPI LCDs */
    },
    {
        .key = "disp_cs", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "LCD CS",
        .help = "Chip-select GPIO for the SPI LCD.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_CS_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_PIN_SHOW, /* SPI LCDs */
    },
    {
        .key = "disp_dc", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "LCD DC",
        .help = "Data/command GPIO for the SPI LCD.",
        /* Not 45: on the Function EV board that pin carries SD_PWRn unless a
         * resistor is moved, so a panel wired there never sees clean levels and
         * stays dark. 26 is free on every board we support. */
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_DC_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_PIN_SHOW, /* SPI LCDs */
    },
    {
        .key = "disp_rst", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "LCD RST",
        .help = "Reset GPIO for the SPI LCD. -1 (None) if RST is tied to 3V3.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_RST_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_PIN_SHOW, /* SPI LCDs */
    },
    {
        .key = "disp_sda", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "OLED SDA",
        .help = "I2C data GPIO for an OLED on a bus of its own, such as the Grove port of the "
                "M5Stack Unit PoE-P4. -1 (None) puts the OLED on the capture chip's bus, where "
                "most boards have it. Set both SDA and SCL, or neither.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_I2C_SDA_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_NE | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_OLED_SHOW,
    },
    {
        .key = "disp_scl", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "OLED SCL",
        .help = "I2C clock GPIO for an OLED on a bus of its own. -1 (None) puts it on the "
                "capture chip's bus.",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_I2C_SCL_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_NE | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_OLED_SHOW,
    },
    {
        .key = "disp_bl", .section = "display", .group = "Wiring", .type = KVM_VT_INT, .title = "LCD backlight",
        .help = "Backlight GPIO for the SPI LCD. -1 (None) if BL is tied to 3V3 (always on).",
        .min = -1, .max = 54, .def = CONFIG_KVM_DISP_BL_GPIO, .requires_cap = -1, .flags = KVM_SF_PIN | KVM_SF_REBOOT | KVM_SF_SHOW_IN,
        .visible_key = "disp_type", .visible_val = DISP_PIN_SHOW, /* SPI LCDs */
    },
};
/* clang-format on */

/*
 * The sections, in the order the console shows them. A section with no entry
 * here is not drawn at all, which is how storage_hidden and the JSON blobs stay
 * out of the way: they are edited from their own panels.
 */
/* clang-format off */
static const kvm_section_t s_sections[] = {
    {"video", "Video", "The picture, and what is recorded from it."},
    {"audio", "Audio", "The target's sound, on three wires from the capture board."},
    {"input", "Input", "Keyboard and pointer, as the target sees them."},
    {"storage", "Virtual media", "The microSD card and what is offered to the target."},
    {"power", "Power", "ATX wiring, Wake-on-LAN, HDMI-CEC and the serial console."},
    {"buttons", "Buttons", "Push buttons wired to the device, and what a press or a hold of each does."},
    {"network", "Network", "How the device is reached."},
    {"vpn", "VPN", "One tunnel at a time: WireGuard or Tailscale."},
    {"mqtt", "MQTT / Home Assistant", "Reporting to a home automation system."},
    {"notify", "Notifications", "Where the device sends news of its own accord."},
    {"security", "Security", "Who may connect, and over what."},
    {"display", "Display", "The optional screen on the device."},
    {"ui", "UI", "How the console in the browser looks and feels."},
    {"system", "System", "The clock, updates and logging."},
};
/* clang-format on */

const kvm_section_t *kvm_settings_sections(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_sections) / sizeof(s_sections[0]);
    }
    return s_sections;
}

const kvm_setting_t *kvm_settings_table(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_settings) / sizeof(s_settings[0]);
    }
    return s_settings;
}
