/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usb_hid.h"

#include <string.h>

#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "class/msc/msc_device.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_storage.h"

static const char *TAG = "usb_hid";

/* ---- HID interfaces ----------------------------------------------------- */

enum {
    ITF_KEYBOARD = 0,
    /*
     * Absolute mouse and consumer control share one interface; the relative
     * mouse gets its own. macOS binds a mouse's buttons per interface, and two
     * pointer collections in a single interface left it unable to tell which
     * one a click belonged to - the cursor moved and scrolled but never
     * clicked. One pointer per interface and it behaves.
     */
    ITF_POINTER = 1,
    ITF_REL_MOUSE = 2,
    /* Virtual media. Always present in the descriptor so the target need not
     * re-enumerate when an image is inserted; the drive simply reports no
     * medium until one is selected. */
    ITF_MSC = 3,
    ITF_COUNT,
};

#define NUM_HID_ITF 3 /* keyboard, pointer, relative mouse */

/* MSC bulk endpoints, sharing endpoint number 4 (IN 0x84, OUT 0x04) beside the
 * three HID interrupt IN endpoints 0x81/0x82/0x83. */
#define EPNUM_MSC_OUT 0x04
#define EPNUM_MSC_IN 0x84

/** Report IDs on the ITF_POINTER interface. Other interfaces use none. */
enum {
    RID_ABS_MOUSE = 1,
    RID_CONSUMER = 3,
};

#if CFG_TUD_HID < 2
#error "Set CONFIG_TINYUSB_HID_COUNT to 2: keyboard and pointer are separate interfaces."
#endif

/*
 * Boot-protocol keyboard: no report ID, 8-byte input, 1-byte LED output.
 * A legacy BIOS will not talk to anything else.
 */
static const uint8_t s_kbd_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

/*
 * Absolute pointer. Logical range 0..32767 on both axes.
 *
 * No physical dimensions are declared, and that is deliberate. With a physical
 * range present, macOS reads it as the shape of the input surface and fits that
 * shape into the display while preserving its aspect ratio - a square range
 * (0..0x7fff on both axes) letterboxed into a 16:9 screen, so the cursor tracks
 * only near the centre and drifts further off toward the edges. Linux and
 * Windows ignore the physical range and map each axis to the screen directly;
 * dropping it makes macOS do the same, and the cursor lands where it is aimed
 * on all three.
 */
#define REPORT_DESC_ABS_MOUSE(...)                                                                  \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP), HID_USAGE(HID_USAGE_DESKTOP_MOUSE),                     \
        HID_COLLECTION(HID_COLLECTION_APPLICATION), __VA_ARGS__ HID_USAGE(HID_USAGE_DESKTOP_POINTER),\
        HID_COLLECTION(HID_COLLECTION_PHYSICAL),                                                    \
        HID_USAGE_PAGE(HID_USAGE_PAGE_BUTTON), HID_USAGE_MIN(1), HID_USAGE_MAX(5),                  \
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1), HID_REPORT_COUNT(5), HID_REPORT_SIZE(1),            \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), HID_REPORT_COUNT(1), HID_REPORT_SIZE(3), \
        HID_INPUT(HID_CONSTANT), HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),                            \
        HID_USAGE(HID_USAGE_DESKTOP_X), HID_USAGE(HID_USAGE_DESKTOP_Y), HID_LOGICAL_MIN_N(0, 2),    \
        HID_LOGICAL_MAX_N(0x7fff, 2),                                                               \
        HID_REPORT_SIZE(16), HID_REPORT_COUNT(2),                                                   \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), HID_USAGE(HID_USAGE_DESKTOP_WHEEL),      \
        HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f), HID_REPORT_SIZE(8), HID_REPORT_COUNT(1),      \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE), HID_USAGE_PAGE(HID_USAGE_PAGE_CONSUMER), \
        HID_USAGE_N(HID_USAGE_CONSUMER_AC_PAN, 2), HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),    \
        HID_REPORT_SIZE(8), HID_REPORT_COUNT(1), HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE), \
        HID_COLLECTION_END, HID_COLLECTION_END

/*
 * Relative pointer with 16-bit deltas. The usual 8-bit form caps one report at
 * 127 mickeys, so a fast drag has to be split across many USB frames.
 */
#define REPORT_DESC_REL_MOUSE(...)                                                                  \
    HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP), HID_USAGE(HID_USAGE_DESKTOP_MOUSE),                     \
        HID_COLLECTION(HID_COLLECTION_APPLICATION), __VA_ARGS__ HID_USAGE(HID_USAGE_DESKTOP_POINTER),\
        HID_COLLECTION(HID_COLLECTION_PHYSICAL),                                                    \
        HID_USAGE_PAGE(HID_USAGE_PAGE_BUTTON), HID_USAGE_MIN(1), HID_USAGE_MAX(5),                  \
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1), HID_REPORT_COUNT(5), HID_REPORT_SIZE(1),            \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), HID_REPORT_COUNT(1), HID_REPORT_SIZE(3), \
        HID_INPUT(HID_CONSTANT), HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),                            \
        HID_USAGE(HID_USAGE_DESKTOP_X), HID_USAGE(HID_USAGE_DESKTOP_Y),                             \
        HID_LOGICAL_MIN_N(-32768, 2), HID_LOGICAL_MAX_N(32767, 2), HID_REPORT_SIZE(16),             \
        HID_REPORT_COUNT(2), HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE),                     \
        HID_USAGE(HID_USAGE_DESKTOP_WHEEL), HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),           \
        HID_REPORT_SIZE(8), HID_REPORT_COUNT(1), HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE), \
        HID_USAGE_PAGE(HID_USAGE_PAGE_CONSUMER), HID_USAGE_N(HID_USAGE_CONSUMER_AC_PAN, 2),         \
        HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f), HID_REPORT_SIZE(8), HID_REPORT_COUNT(1),      \
        HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE), HID_COLLECTION_END, HID_COLLECTION_END

static const uint8_t s_pointer_report_desc[] = {
    REPORT_DESC_ABS_MOUSE(HID_REPORT_ID(RID_ABS_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
};

/* The relative mouse, alone on its own interface and so needing no report ID. */
static const uint8_t s_rel_report_desc[] = {
    REPORT_DESC_REL_MOUSE(),
};

typedef struct __attribute__((packed)) {
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
    int8_t wheel;
    int8_t pan;
} abs_mouse_report_t;

typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int16_t dx;
    int16_t dy;
    int8_t wheel;
    int8_t pan;
} rel_mouse_report_t;

/* Order matters: the string index each interface names below is this array's
 * subscript. 0 is the language ID, 1..3 are the device strings, 4+ name the
 * interfaces (keyboard 4, pointer 5, relative mouse 6, virtual media 7). */
static const char *s_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "ESP-KVM",
    "ESP-KVM Keyboard/Mouse",
    "0",
    "Keyboard",
    "Pointer",
    "Relative Mouse",
    "Virtual Media",
};

/*
 * The configuration descriptor is assembled at start-up rather than baked in,
 * because the set of USB functions is not fixed. The keyboard and pointers are
 * always present - they are a KVM's reason to exist - while mass storage (and,
 * later, a USB network interface) is optional. Presenting every function at once
 * can exceed the controller's endpoint budget, so only the enabled ones go into
 * the descriptor. The optional blocks are always appended after the three HID
 * interfaces, which keeps the interface numbers contiguous as required; a change
 * to the set needs a re-enumeration, so the toggles that drive it are marked
 * restart-required.
 *
 * The two speeds differ only in the MSC bulk endpoint size (512 high speed, 64
 * full speed), so each optional block that has one is kept in both flavours.
 */
#define CFG_DESC_MAX (TUD_CONFIG_DESC_LEN + NUM_HID_ITF * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN + 64)

/* A placeholder header: wTotalLength and bNumInterfaces are patched in once the
 * enabled blocks are known. */
static const uint8_t k_cfg_header[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CFG_DESC_MAX, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
};
/* interface, string index, protocol, report descriptor length, endpoint, size, interval */
static const uint8_t k_hid_ifaces[] = {
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_kbd_report_desc), 0x81,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(ITF_POINTER, 5, HID_ITF_PROTOCOL_NONE, sizeof(s_pointer_report_desc), 0x82,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(ITF_REL_MOUSE, 6, HID_ITF_PROTOCOL_NONE, sizeof(s_rel_report_desc), 0x83,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};
/* interface, string index, EP out, EP in, EP size */
static const uint8_t k_msc_iface_fs[] = {
    TUD_MSC_DESCRIPTOR(ITF_MSC, 7, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};
static const uint8_t k_msc_iface_hs[] = {
    TUD_MSC_DESCRIPTOR(ITF_MSC, 7, EPNUM_MSC_OUT, EPNUM_MSC_IN, 512),
};

static uint8_t s_fs_config_descriptor[CFG_DESC_MAX];
static uint8_t s_hs_config_descriptor[CFG_DESC_MAX];

/*
 * Assemble the configuration descriptor for the enabled functions into @p buf
 * and return its length. HID is always included; @p msc_iface (the speed's MSC
 * block) is appended when @p with_msc. The config header's wTotalLength and
 * bNumInterfaces are then patched to match what was actually emitted.
 */
static size_t build_config_descriptor(uint8_t *buf, bool with_msc, const uint8_t *msc_iface,
                                      size_t msc_len)
{
    size_t n = 0;
    uint8_t ifaces = NUM_HID_ITF;
    memcpy(buf + n, k_cfg_header, sizeof(k_cfg_header));
    n += sizeof(k_cfg_header);
    memcpy(buf + n, k_hid_ifaces, sizeof(k_hid_ifaces));
    n += sizeof(k_hid_ifaces);
    if (with_msc) {
        memcpy(buf + n, msc_iface, msc_len);
        n += msc_len;
        ifaces++; /* MSC is ITF_MSC == NUM_HID_ITF, appended last, no renumbering */
    }
    buf[2] = (uint8_t)(n & 0xff); /* wTotalLength, little-endian */
    buf[3] = (uint8_t)(n >> 8);
    buf[4] = ifaces;              /* bNumInterfaces */
    return n;
}

/* ---- report queue ------------------------------------------------------- */

typedef enum {
    Q_MOUSE_ABS,
    Q_MOUSE_REL,
    Q_KEY,
    Q_CONSUMER,
    Q_RELEASE_ALL,
} q_type_t;

typedef struct {
    q_type_t type;
    union {
        abs_mouse_report_t abs;
        struct {
            uint8_t buttons;
            int32_t dx;
            int32_t dy;
            int8_t wheel;
            int8_t pan;
        } rel;
        struct {
            uint8_t modifier;
            uint8_t keycode[6];
        } key;
        uint16_t consumer;
    } u;
} q_msg_t;

static QueueHandle_t s_hid_q;
static TaskHandle_t s_hid_task;
/* Last absolute position sent, so buttons can be released without moving the
 * target's cursor somewhere it never was. */
static uint16_t s_last_abs_x;
static uint16_t s_last_abs_y;
static volatile uint8_t s_leds;
static usb_hid_led_cb_t s_led_cb;
static void *s_led_cb_user;

/* ---- TinyUSB callbacks -------------------------------------------------- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case ITF_KEYBOARD:
        return s_kbd_report_desc;
    case ITF_REL_MOUSE:
        return s_rel_report_desc;
    default:
        return s_pointer_report_desc;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    /* The only output report we expect is the keyboard LED bitmap. Reflecting it
     * to the browser is the only way an operator can tell whether Caps Lock is
     * on: the target's own indicator is not visible remotely. */
    if (instance != ITF_KEYBOARD || report_type != HID_REPORT_TYPE_OUTPUT || bufsize < 1) {
        return;
    }
    const uint8_t leds = buffer[0];
    if (leds == s_leds) {
        return;
    }
    s_leds = leds;
    if (s_led_cb) {
        s_led_cb(leds, s_led_cb_user);
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
    if (s_hid_task) {
        xTaskNotifyGive(s_hid_task);
    }
}

/* ---- target-OS detection ------------------------------------------------ */
/*
 * The host reveals its OS in how it enumerates us. Fingerprints measured on
 * real machines (the string column is the descriptor index; "D" device, "Cx"
 * configuration, "Sxx" string):
 *
 *   Windows  requests string 0xEE (the MS OS descriptor); the others never do.
 *   Android  D D C0 C0 <strings> then a second pass re-reading every string,
 *            each preceded by a langid (S00) request - so many S00 requests.
 *   macOS    reads each string twice in a row (S02 S02 ...) and asks for the
 *            langid (S00) last.
 *   Linux    reads the langid (S00) first and each string once; ~16 requests.
 *
 * The trace is recorded here and also exposed raw over the API
 * (usb_hid_probe_trace). kvm_usb_host_probe overrides the weak stub in
 * esp_tinyusb/descriptors_control.c.
 */
#define PROBE_MAX 80
static volatile int s_probe_n;
static uint16_t s_probe[PROBE_MAX]; /* (kind << 12) | index, in request order */

void kvm_usb_host_probe(int kind, uint16_t index)
{
    /*
     * A device-descriptor request after strings have already been read is a
     * fresh enumeration - the target rebooted or was replugged - so start the
     * trace over. This re-detects the current host without relying on a clean
     * detach event, which this OTG port does not always deliver.
     */
    if (kind == 0) {
        for (int i = 0; i < s_probe_n; i++) {
            if ((s_probe[i] >> 12) == 2) {
                s_probe_n = 0;
                break;
            }
        }
    }
    const int n = s_probe_n;
    if (n < PROBE_MAX) {
        s_probe[n] = (uint16_t)((kind << 12) | (index & 0x0FFF));
        s_probe_n = n + 1;
    }
}

const char *usb_hid_target_os(void)
{
    const int n = s_probe_n;
    if (n < 6) {
        return "unknown"; /* too little of an enumeration to tell */
    }
    int langid = 0;
    bool has_ee = false, dup = false, langid_first = false, first_string = true;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        if ((s_probe[i] >> 12) != 2) { /* string requests only */
            prev = -1;
            continue;
        }
        const int idx = s_probe[i] & 0x0FFF;
        if (first_string) {
            /* Linux asks for the langid (S00) before any string; macOS asks for
             * it last. This is the tell that separates them when a Linux host also
             * happens to re-read one string (e.g. udev reading the serial twice). */
            langid_first = (idx == 0);
            first_string = false;
        }
        if (idx == 0xEE) {
            has_ee = true;
        }
        if (idx == 0) {
            langid++;
            prev = 0;
        } else {
            if (idx == prev) {
                dup = true;
            }
            prev = idx;
        }
    }
    if (has_ee) {
        return "windows";
    }
    if (langid >= 3) {
        return "android";
    }
    /* macOS reads strings twice in a row AND requests the langid last; a lone
     * repeated string on a host that asked for the langid first is Linux, not
     * macOS (an ASUS NUC on Ubuntu that re-read its serial three times was being
     * misidentified as a Mac). */
    if (dup && !langid_first) {
        return "macos";
    }
    return "linux";
}

size_t usb_hid_probe_trace(char *out, size_t len)
{
    if (!out || len == 0) {
        return 0;
    }
    size_t off = 0;
    const int n = s_probe_n;
    for (int i = 0; i < n && off + 8 < len; i++) {
        const int kind = s_probe[i] >> 12;
        const int idx = s_probe[i] & 0x0FFF;
        if (i) {
            out[off++] = ' ';
        }
        if (kind == 0) {
            off += snprintf(out + off, len - off, "D");
        } else if (kind == 1) {
            off += snprintf(out + off, len - off, "C%x", idx);
        } else {
            off += snprintf(out + off, len - off, "S%02x", idx);
        }
    }
    out[off] = '\0';
    return off;
}

static void tinyusb_on_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!event) {
        return;
    }
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "target attached");
        break;
    case TINYUSB_EVENT_DETACHED:
        s_leds = 0;
        s_probe_n = 0; /* next host's enumeration starts a fresh trace */
        ESP_LOGI(TAG, "target detached");
        break;
    default:
        break;
    }
}

/* ---- MSC (virtual media) callbacks -------------------------------------- */
/*
 * The target sees one removable LUN. Its medium is whatever kvm_storage has open;
 * with none open the drive answers "no medium" like an empty card reader, which
 * every host understands. The disk lives on the microSD (or on-flash rescue) and
 * is read on the target's behalf, so the firmware never yields the card - uploads
 * and target reads run at the same time. It is read-only except for the whole-card
 * passthrough, which the target may write while it owns the card exclusively.
 */

/*
 * The device type the host sees: a CD-ROM for .iso images (so it boots and mounts
 * as an optical drive), a removable disk otherwise. The host reads this once, at
 * enumeration; usb_hid_msc_set_type re-attaches the device when the type changes
 * so it is read again. s_msc_present tracks whether the MSC interface is even in
 * the descriptor (only when virtual media was enabled at boot). Block size follows
 * the type: 2048 for a CD, 512 for a disk.
 */
static bool s_msc_cdrom;
static bool s_msc_present;
static esp_timer_handle_t s_msc_reconnect_timer;

static uint32_t msc_block_size(void)
{
    return s_msc_cdrom ? 2048u : 512u;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "ESP-KVM ", 8);
    memcpy(product_id, "Virtual Media   ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    kvm_media_t m;
    kvm_storage_media_info(&m);
    if (!m.present) {
        /* Not ready, no medium - the standard "empty drive" answer (sense
         * 3A). Without setting sense a host may keep polling or error out. */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    kvm_media_t m;
    kvm_storage_media_info(&m);
    *block_count = (uint32_t)m.block_count;
    *block_size = (uint16_t)m.block_size;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    /* The host's software-eject (e.g. dragging the disk to the trash) removes
     * the medium; the operator re-inserts it from the console. */
    if (load_eject && !start) {
        kvm_storage_media_eject();
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    /* Only the whole-card passthrough is writable; everything else is read-only. */
    return kvm_storage_media_writable();
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    const uint64_t addr = (uint64_t)lba * msc_block_size() + offset;
    return kvm_storage_media_read(addr, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize)
{
    if (!kvm_storage_media_writable()) {
        /* Reject with a write-protect sense; is_writable_cb already told the host. */
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        return -1;
    }
    const uint64_t addr = (uint64_t)lba * msc_block_size() + offset;
    const int32_t n = kvm_storage_media_write(addr, buffer, bufsize);
    if (n < 0) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x00);
    }
    return n;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        /* We never lock the medium; acknowledge and move on. */
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *resp, uint32_t bufsize)
{
    (void)lun;
    (void)bufsize;
    resp->peripheral_device_type = s_msc_cdrom ? SCSI_PDT_CD_DVD : SCSI_PDT_DIRECT_ACCESS;
    resp->is_removable = 1;
    memcpy(resp->vendor_id, "ESP-KVM ", 8);
    memcpy(resp->product_id, "Virtual Media   ", 16);
    memcpy(resp->product_rev, "1.0 ", 4);
    return sizeof(scsi_inquiry_resp_t);
}

static void msc_reconnect_cb(void *arg)
{
    (void)arg;
    tud_connect();
}

void usb_hid_msc_set_type(bool cdrom)
{
    if (cdrom == s_msc_cdrom) {
        return;
    }
    s_msc_cdrom = cdrom;
    /* Nothing for the host to re-read until the drive exists and is enumerated. */
    if (!s_msc_present || !s_msc_reconnect_timer || !tud_mounted()) {
        return;
    }
    /* Drop off the bus and come back, so the host re-issues INQUIRY and picks up
     * the new device type. Deferred reconnect keeps the caller (a settings write
     * on the web task) from blocking; the HID endpoints blink out for the ~100 ms
     * this takes, which is the cost of swapping an ISO for a disk image. */
    tud_disconnect();
    (void)esp_timer_stop(s_msc_reconnect_timer);
    (void)esp_timer_start_once(s_msc_reconnect_timer, 100 * 1000);
}

/* ---- report emission ---------------------------------------------------- */

/** Wait for the previous report to leave the endpoint. */
static bool wait_report_sent(void)
{
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(80)) > 0;
}

/*
 * Submit one report and wait for it to leave the endpoint, retrying once if the
 * endpoint is still busy with a prior report. Any stale completion left by an
 * earlier timeout is cleared first so it cannot satisfy this wait early: the
 * completion callback is not per-instance, but only one report is ever in flight
 * at a time, so a single shared notification is correct once stale ones are
 * dropped. Without the retry a report dropped on a busy endpoint - typically the
 * key-up/button-up after the host stalled an IN transfer past the 80 ms wait -
 * was never resent, leaving the key or button stuck down on the target.
 */
static void hid_emit(uint8_t itf, uint8_t id, const void *data, uint16_t len)
{
    (void)ulTaskNotifyTake(pdTRUE, 0); /* drop any stale completion from a prior timeout */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (tud_hid_n_report(itf, id, data, len)) {
            (void)wait_report_sent();
            return;
        }
        /* Endpoint busy: wait for the in-flight report to drain, then retry. */
        if (!wait_report_sent()) {
            return;
        }
    }
}

static void send_abs(const abs_mouse_report_t *r)
{
    s_last_abs_x = r->x;
    s_last_abs_y = r->y;
    hid_emit(ITF_POINTER, RID_ABS_MOUSE, r, sizeof(*r));
}

static void send_rel(uint8_t buttons, int32_t dx, int32_t dy, int8_t wheel, int8_t pan)
{
    if (dx > INT16_MAX) {
        dx = INT16_MAX;
    } else if (dx < INT16_MIN) {
        dx = INT16_MIN;
    }
    if (dy > INT16_MAX) {
        dy = INT16_MAX;
    } else if (dy < INT16_MIN) {
        dy = INT16_MIN;
    }
    rel_mouse_report_t r = {
        .buttons = buttons,
        .dx = (int16_t)dx,
        .dy = (int16_t)dy,
        .wheel = wheel,
        .pan = pan,
    };
    hid_emit(ITF_REL_MOUSE, 0, &r, sizeof(r));
}

static void send_keyboard(uint8_t modifier, const uint8_t keycode[6])
{
    (void)ulTaskNotifyTake(pdTRUE, 0); /* drop any stale completion from a prior timeout */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (tud_hid_n_keyboard_report(ITF_KEYBOARD, 0, modifier, (uint8_t *)keycode)) {
            (void)wait_report_sent();
            return;
        }
        if (!wait_report_sent()) {
            return;
        }
    }
}

static void send_consumer(uint16_t usage)
{
    hid_emit(ITF_POINTER, RID_CONSUMER, &usage, sizeof(usage));
}

static void send_release_all(void)
{
    const uint8_t none[6] = {0};
    send_keyboard(0, none);
    send_consumer(0);
    send_rel(0, 0, 0, 0, 0);
    /*
     * The absolute pointer needs its own release. Clearing only the relative
     * one leaves a button the host believes is still down on the other report,
     * and the symptom is a target whose pointer moves but never clicks -
     * everything reads as one endless drag. Re-send the last position so the
     * release does not also teleport the cursor.
     */
    const abs_mouse_report_t idle = {
        .buttons = 0,
        .x = s_last_abs_x,
        .y = s_last_abs_y,
        .wheel = 0,
        .pan = 0,
    };
    send_abs(&idle);
}

/**
 * Fold a newer message into a pending one of the same kind.
 * @return false when the kinds differ and the pending message must go out first.
 */
static bool merge_mouse(q_msg_t *acc, const q_msg_t *add)
{
    if (acc->type != add->type) {
        return false;
    }
    /*
     * Coalesce motion, never a button edge. A click is a press report followed
     * by a release report; if both are waiting in the queue and get folded into
     * one, only the newest survives - the release - and the press is gone, so
     * the target sees the cursor move but never registers the click. Motion is
     * safe to drop because only the latest position matters; a change in the
     * buttons is not, so it forces the pending report out first.
     */
    const uint8_t acc_buttons =
        acc->type == Q_MOUSE_ABS ? acc->u.abs.buttons : acc->u.rel.buttons;
    const uint8_t add_buttons =
        add->type == Q_MOUSE_ABS ? add->u.abs.buttons : add->u.rel.buttons;
    if (acc_buttons != add_buttons) {
        return false;
    }
    if (acc->type == Q_MOUSE_ABS) {
        /* Only the newest position matters - intermediate ones are not motion
         * the target needs to see. Scroll clicks still accumulate. */
        const int wheel = (int)acc->u.abs.wheel + (int)add->u.abs.wheel;
        const int pan = (int)acc->u.abs.pan + (int)add->u.abs.pan;
        acc->u.abs = add->u.abs;
        acc->u.abs.wheel = (int8_t)(wheel > 127 ? 127 : (wheel < -127 ? -127 : wheel));
        acc->u.abs.pan = (int8_t)(pan > 127 ? 127 : (pan < -127 ? -127 : pan));
        return true;
    }
    acc->u.rel.buttons = add->u.rel.buttons;
    acc->u.rel.dx += add->u.rel.dx;
    acc->u.rel.dy += add->u.rel.dy;
    const int wheel = (int)acc->u.rel.wheel + (int)add->u.rel.wheel;
    const int pan = (int)acc->u.rel.pan + (int)add->u.rel.pan;
    acc->u.rel.wheel = (int8_t)(wheel > 127 ? 127 : (wheel < -127 ? -127 : wheel));
    acc->u.rel.pan = (int8_t)(pan > 127 ? 127 : (pan < -127 ? -127 : pan));
    return true;
}

static void dispatch(const q_msg_t *m)
{
    switch (m->type) {
    case Q_MOUSE_ABS:
        send_abs(&m->u.abs);
        break;
    case Q_MOUSE_REL:
        send_rel(m->u.rel.buttons, m->u.rel.dx, m->u.rel.dy, m->u.rel.wheel, m->u.rel.pan);
        break;
    case Q_KEY:
        send_keyboard(m->u.key.modifier, m->u.key.keycode);
        break;
    case Q_CONSUMER:
        send_consumer(m->u.consumer);
        break;
    case Q_RELEASE_ALL:
        send_release_all();
        break;
    }
}

static void hid_worker(void *arg)
{
    (void)arg;
    q_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_hid_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!tud_mounted()) {
            continue;
        }
        if (msg.type != Q_MOUSE_ABS && msg.type != Q_MOUSE_REL) {
            dispatch(&msg);
            continue;
        }

        /* Coalesce the motion that piled up while the previous report was in
         * flight: replaying every queued position would lag behind the operator. */
        q_msg_t acc = msg;
        for (;;) {
            q_msg_t next;
            if (xQueuePeek(s_hid_q, &next, 0) != pdTRUE) {
                break;
            }
            if (!merge_mouse(&acc, &next)) {
                break;
            }
            (void)xQueueReceive(s_hid_q, &next, 0);
        }
        dispatch(&acc);
    }
}

/* ---- public API --------------------------------------------------------- */

bool usb_hid_ready(void)
{
    /* tud_mounted() is TinyUSB's own authoritative "the host has enumerated and
     * configured us" state. An earlier shadow flag set from the mount event could
     * desync: if the target had already enumerated the device before this task
     * registered its event handler (a warm reboot with the cable still attached),
     * the flag stayed false while the device was plainly mounted - so input kept
     * working (the worker only checks tud_mounted()) but the status indicator and
     * the REST "no USB target" checks wrongly reported no target until a replug. */
    return tud_mounted();
}

uint8_t usb_hid_leds(void)
{
    return s_leds;
}

void usb_hid_set_led_callback(usb_hid_led_cb_t cb, void *user)
{
    s_led_cb_user = user;
    s_led_cb = cb;
}

static void enqueue(const q_msg_t *m)
{
    if (!s_hid_q) {
        return;
    }
    (void)xQueueSend(s_hid_q, m, 0);
}

void usb_hid_mouse_abs(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel, int8_t pan)
{
    if (x > USB_HID_ABS_MAX) {
        x = USB_HID_ABS_MAX;
    }
    if (y > USB_HID_ABS_MAX) {
        y = USB_HID_ABS_MAX;
    }
    const q_msg_t m = {
        .type = Q_MOUSE_ABS,
        .u.abs = {.buttons = buttons, .x = x, .y = y, .wheel = wheel, .pan = pan},
    };
    enqueue(&m);
}

void usb_hid_mouse_rel(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    const q_msg_t m = {
        .type = Q_MOUSE_REL,
        .u.rel = {.buttons = buttons, .dx = dx, .dy = dy, .wheel = wheel, .pan = pan},
    };
    enqueue(&m);
}

void usb_hid_keyboard(uint8_t modifier, const uint8_t keycode[6])
{
    if (!keycode) {
        return;
    }
    q_msg_t m = {.type = Q_KEY};
    m.u.key.modifier = modifier;
    memcpy(m.u.key.keycode, keycode, 6);
    enqueue(&m);
}

void usb_hid_consumer(uint16_t usage)
{
    const q_msg_t m = {.type = Q_CONSUMER, .u.consumer = usage};
    enqueue(&m);
}

void usb_hid_release_all(void)
{
    if (!s_hid_q) {
        return;
    }
    const q_msg_t m = {.type = Q_RELEASE_ALL};
    if (xQueueSend(s_hid_q, &m, 0) != pdTRUE) {
        /*
         * The queue is full and this must not be the message that gets dropped:
         * a key or button left held on the target (the exact thing release-all
         * exists to prevent, e.g. on a control-session drop) is far worse than
         * losing queued motion. Clear the backlog and enqueue the release.
         */
        xQueueReset(s_hid_q);
        (void)xQueueSend(s_hid_q, &m, 0);
    }
}

esp_err_t usb_hid_init(void)
{
    if (s_hid_q) {
        return ESP_OK;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "gpio_install_isr_service");
    }

    s_hid_q = xQueueCreate(192, sizeof(q_msg_t));
    ESP_RETURN_ON_FALSE(s_hid_q, ESP_ERR_NO_MEM, TAG, "queue");

    /*
     * Decide which optional USB functions to expose before the descriptor is
     * built. Mass storage is off unless the operator turned it on, so the device
     * is a plain keyboard and mouse by default and only claims the extra
     * endpoints when virtual media is actually wanted.
     */
    const bool with_msc = kvm_setting_bool("msc_enable");
    s_msc_present = with_msc;
    if (with_msc) {
        const esp_timer_create_args_t args = {.callback = msc_reconnect_cb, .name = "msc_reconn"};
        (void)esp_timer_create(&args, &s_msc_reconnect_timer);
    }
    build_config_descriptor(s_fs_config_descriptor, with_msc, k_msc_iface_fs, sizeof(k_msc_iface_fs));
    build_config_descriptor(s_hs_config_descriptor, with_msc, k_msc_iface_hs, sizeof(k_msc_iface_hs));
    ESP_LOGI(TAG, "USB functions: HID%s", with_msc ? " + mass storage" : " only");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_on_event);
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = s_fs_config_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_hs_config_descriptor;
#endif

    esp_err_t usb_err = tinyusb_driver_install(&tusb_cfg);
    kvm_cap_report(KVM_CAP_HID, usb_err == ESP_OK, "USB device stack failed to start (%s)",
                   esp_err_to_name(usb_err));
    ESP_RETURN_ON_ERROR(usb_err, TAG, "tinyusb_driver_install");

    /* Above stream/httpd work so HID reports are not delayed by MJPEG or WS parsing. */
    BaseType_t ok = xTaskCreate(hid_worker, "usb_hid", 4096, NULL, tskIDLE_PRIORITY + 8, &s_hid_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");

    return ESP_OK;
}
