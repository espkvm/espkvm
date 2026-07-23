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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "kvm_caps.h"
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

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + NUM_HID_ITF * TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)

/*
 * One layout, two instances: the MSC bulk endpoint is 512 bytes at high speed
 * and must be 64 at full speed, so the descriptor is parameterised on that size
 * and built once for each speed. The HID interrupt endpoints keep one buffer
 * size at both speeds, which is legal. On the P4's high-speed PHY the host uses
 * the high-speed config; the full-speed one covers a full-speed host or hub.
 */
#define CONFIGURATION_DESCRIPTOR(msc_epsize)                                                        \
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, \
                          100),                                                                     \
    /* interface, string index, protocol, report descriptor length, endpoint, size, interval */    \
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_kbd_report_desc), 0x81, \
                       CFG_TUD_HID_EP_BUFSIZE, 10),                                                 \
    TUD_HID_DESCRIPTOR(ITF_POINTER, 5, HID_ITF_PROTOCOL_NONE, sizeof(s_pointer_report_desc), 0x82,  \
                       CFG_TUD_HID_EP_BUFSIZE, 10),                                                 \
    TUD_HID_DESCRIPTOR(ITF_REL_MOUSE, 6, HID_ITF_PROTOCOL_NONE, sizeof(s_rel_report_desc), 0x83,    \
                       CFG_TUD_HID_EP_BUFSIZE, 10),                                                 \
    /* interface, string index, EP out, EP in, EP size */                                          \
    TUD_MSC_DESCRIPTOR(ITF_MSC, 7, EPNUM_MSC_OUT, EPNUM_MSC_IN, (msc_epsize))

static const uint8_t s_fs_config_descriptor[] = {CONFIGURATION_DESCRIPTOR(64)};
static const uint8_t s_hs_config_descriptor[] = {CONFIGURATION_DESCRIPTOR(512)};

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
static volatile bool s_usb_mounted;
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

static void tinyusb_on_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!event) {
        return;
    }
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_usb_mounted = true;
        ESP_LOGI(TAG, "target attached");
        break;
    case TINYUSB_EVENT_DETACHED:
        s_usb_mounted = false;
        s_leds = 0;
        ESP_LOGI(TAG, "target detached");
        break;
    default:
        break;
    }
}

/* ---- MSC (virtual media) callbacks -------------------------------------- */
/*
 * The target sees one removable, read-only LUN. Its medium is whatever image
 * kvm_storage currently has open; with none open the drive answers "no medium"
 * like an empty card reader, which is a state every host understands. All the
 * disk lives on the microSD and is read on the target's behalf, so the firmware
 * never yields the card - uploads and target reads run at the same time.
 */

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
    return false; /* images are served read-only in this version */
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    const uint64_t addr = (uint64_t)lba * 512u + offset;
    return kvm_storage_media_read(addr, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize)
{
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)bufsize;
    /* Read-only: reject writes with a write-protect sense. is_writable_cb above
     * already tells the host as much, so this is the belt-and-suspenders path. */
    tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
    return -1;
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

/* ---- report emission ---------------------------------------------------- */

/** Wait for the previous report to leave the endpoint. */
static bool wait_report_sent(void)
{
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(80)) > 0;
}

static void send_abs(const abs_mouse_report_t *r)
{
    s_last_abs_x = r->x;
    s_last_abs_y = r->y;
    if (tud_hid_n_report(ITF_POINTER, RID_ABS_MOUSE, r, sizeof(*r))) {
        (void)wait_report_sent();
    }
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
    if (tud_hid_n_report(ITF_REL_MOUSE, 0, &r, sizeof(r))) {
        (void)wait_report_sent();
    }
}

static void send_keyboard(uint8_t modifier, const uint8_t keycode[6])
{
    if (tud_hid_n_keyboard_report(ITF_KEYBOARD, 0, modifier, (uint8_t *)keycode)) {
        (void)wait_report_sent();
    }
}

static void send_consumer(uint16_t usage)
{
    if (tud_hid_n_report(ITF_POINTER, RID_CONSUMER, &usage, sizeof(usage))) {
        (void)wait_report_sent();
    }
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
    return s_usb_mounted && tud_mounted();
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
    const q_msg_t m = {.type = Q_RELEASE_ALL};
    enqueue(&m);
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
