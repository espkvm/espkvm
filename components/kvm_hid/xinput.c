/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A wired Xbox 360 controller (XInput), as a TinyUSB class driver of our own.
 *
 * It is not HID: the interface is vendor class 0xFF, subclass 0x5D, protocol
 * 0x01, with interrupt endpoints - TinyUSB's vendor driver only opens bulk ones,
 * hence a driver here. Windows binds its XInput driver by the device's IDs
 * (045E:028E), Linux's xpad by the interface triple. The input report is 20
 * bytes; the 32-byte output reports (rumble, the ring of lights) are read and
 * dropped. The descriptor bytes are those of the real pad's first interface.
 *
 * The same driver is the original Xbox pad (XID) when xinput_enable() is told
 * so: class 0x58 / 0x42, no class descriptor, the same 20-byte report in a
 * different layout, and three vendor requests the console asks before it reads
 * a report. The bytes come from ZacharyLeahan's fork, where they were tried on a
 * real Xbox.
 */
#include "xinput.h"

#include <string.h>

#include "device/usbd_pvt.h"
#if CONFIG_KVM_ENABLE_USB_VIDEO
#include "usb_video_driver.h"
#endif
#include "esp_log.h"
#include "tusb.h"

static const char *TAG = "xinput";

#define XINPUT_SUBCLASS 0x5D
#define XINPUT_PROTOCOL 0x01
#define XID_CLASS 0x58
#define XID_SUBCLASS 0x42

const tusb_desc_device_t xinput_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xFF,
    .bDeviceSubClass = 0xFF,
    .bDeviceProtocol = 0xFF,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x045E,  /* Microsoft */
    .idProduct = 0x028E, /* Xbox 360 Controller */
    .bcdDevice = 0x0114,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const tusb_desc_device_t xid_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0110,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x045E,  /* Microsoft */
    .idProduct = 0x0289, /* Xbox Controller S */
    .bcdDevice = 0x0121,
    .bNumConfigurations = 1,
};

/* GET_DESCRIPTOR 0x42: XID 1.00, type gamepad, 20 bytes in, 6 out. */
static const uint8_t k_xid_desc[16] = {16, 0x42, 0x00, 0x01, 0x01, 0x02, 20, 6,
                                       255, 255, 255, 255, 255, 255, 255, 255};

static bool s_xid;

size_t xid_iface_desc(uint8_t *out, uint8_t ep_in, uint8_t ep_out)
{
    const uint8_t d[XID_IFACE_DESC_LEN] = {
        0x09, TUSB_DESC_INTERFACE, 0, 0x00, 0x02, XID_CLASS, XID_SUBCLASS, 0x00, 0x00,
        0x07, TUSB_DESC_ENDPOINT, ep_in, TUSB_XFER_INTERRUPT, 0x20, 0x00, 0x04,
        0x07, TUSB_DESC_ENDPOINT, ep_out, TUSB_XFER_INTERRUPT, 0x20, 0x00, 0x04,
    };
    memcpy(out, d, sizeof(d));
    return sizeof(d);
}

size_t xinput_iface_desc(uint8_t *out, uint8_t itf, uint8_t ep_in, uint8_t ep_out)
{
    const uint8_t d[XINPUT_IFACE_DESC_LEN] = {
        /* interface: 2 endpoints, vendor class 0xFF / 0x5D / 0x01 */
        0x09, TUSB_DESC_INTERFACE, itf, 0x00, 0x02, 0xFF, XINPUT_SUBCLASS, XINPUT_PROTOCOL, 0x00,
        /* the pad's own class descriptor, copied byte for byte */
        0x11, 0x21, 0x00, 0x01, 0x01, 0x25, ep_in, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, ep_out, 0x08,
        0x00, 0x00,
        /* interrupt IN, 32 bytes, every frame; interrupt OUT, 32 bytes */
        0x07, TUSB_DESC_ENDPOINT, ep_in, TUSB_XFER_INTERRUPT, 0x20, 0x00, 0x01,
        0x07, TUSB_DESC_ENDPOINT, ep_out, TUSB_XFER_INTERRUPT, 0x20, 0x00, 0x08,
    };
    memcpy(out, d, sizeof(d));
    return sizeof(d);
}

static uint8_t s_rhport;
static uint8_t s_ep_in;
static uint8_t s_ep_out;
static bool s_open;
static uint8_t s_in_buf[XINPUT_REPORT_LEN];
static uint8_t s_out_buf[32];
static uint8_t s_last[XINPUT_REPORT_LEN] = {0, XINPUT_REPORT_LEN};
static uint8_t s_ctrl_buf[XINPUT_REPORT_LEN];
static void (*s_done_cb)(void);

void xinput_set_done_cb(void (*cb)(void))
{
    s_done_cb = cb;
}

static void xi_init(void)
{
    s_open = false;
}

static bool xi_deinit(void)
{
    s_open = false;
    return true;
}

static void xi_reset(uint8_t rhport)
{
    (void)rhport;
    s_open = false;
}

static uint16_t xi_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (s_xid ? (itf->bInterfaceClass != XID_CLASS || itf->bInterfaceSubClass != XID_SUBCLASS)
              : (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC ||
                 itf->bInterfaceSubClass != XINPUT_SUBCLASS ||
                 itf->bInterfaceProtocol != XINPUT_PROTOCOL)) {
        return 0;
    }
    const uint16_t len = s_xid ? XID_IFACE_DESC_LEN : XINPUT_IFACE_DESC_LEN;
    if (max_len < len) {
        return 0;
    }
    /* Interface (9), the XInput class descriptor (17; XID has none), then the
     * two endpoints. */
    const uint8_t *p = (const uint8_t *)itf + 9 + (s_xid ? 0 : 17);
    for (int i = 0; i < 2; i++) {
        const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
        if (ep->bDescriptorType != TUSB_DESC_ENDPOINT || !usbd_edpt_open(rhport, ep)) {
            ESP_LOGW(TAG, "could not open endpoint 0x%02x", ep->bEndpointAddress);
            return 0;
        }
        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
            s_ep_in = ep->bEndpointAddress;
        } else {
            s_ep_out = ep->bEndpointAddress;
        }
        p += ep->bLength;
    }
    s_rhport = rhport;
    s_open = true;
    (void)usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false);
    ESP_LOGI(TAG, "opened by the host (in 0x%02x, out 0x%02x)", s_ep_in, s_ep_out);
    return len;
}

static bool xi_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (!s_xid) {
        return false; /* nothing the wired pad must answer; the rest is stalled */
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    const uint8_t type = req->bmRequestType;
    /* The XID descriptor. */
    if (type == 0xC1 && req->bRequest == 0x06 && req->wValue == 0x4200) {
        return tud_control_xfer(rhport, req, (void *)k_xid_desc, sizeof(k_xid_desc));
    }
    /* Capabilities: which input (0x0100) and output (0x0200) bytes are real. */
    if (type == 0xC1 && req->bRequest == 0x01 && (req->wValue == 0x0100 || req->wValue == 0x0200)) {
        const uint16_t n = req->wValue == 0x0100 ? XINPUT_REPORT_LEN : 6;
        memset(s_ctrl_buf, 0xFF, n);
        s_ctrl_buf[0] = 0;
        s_ctrl_buf[1] = (uint8_t)n;
        if (n == XINPUT_REPORT_LEN) {
            s_ctrl_buf[3] = 0;
        }
        return tud_control_xfer(rhport, req, s_ctrl_buf, n);
    }
    /* HID-style GET_REPORT: the state now. */
    if (type == 0xA1 && req->bRequest == 0x01 && req->wValue == 0x0100) {
        memcpy(s_ctrl_buf, s_last, XINPUT_REPORT_LEN);
        return tud_control_xfer(rhport, req, s_ctrl_buf, XINPUT_REPORT_LEN);
    }
    /* SET_REPORT with the rumble motors: taken and dropped. */
    if (type == 0x21 && req->bRequest == 0x09 && req->wValue == 0x0200 && req->wLength == 6) {
        return tud_control_xfer(rhport, req, s_ctrl_buf, 6);
    }
    return false;
}

static bool xi_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred)
{
    (void)result;
    (void)xferred;
    if (ep_addr == s_ep_out) {
        /* Rumble and lights: not ours to show. Keep listening. */
        (void)usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false);
    } else if (ep_addr == s_ep_in && s_done_cb) {
        s_done_cb();
    }
    return true;
}

static const usbd_class_driver_t k_driver = {
    .name = "XINPUT",
    .init = xi_init,
    .deinit = xi_deinit,
    .reset = xi_reset,
    .open = xi_open,
    .control_xfer_cb = xi_control,
    .xfer_cb = xi_xfer,
    .xfer_isr = NULL,
    .sof = NULL,
};

static bool s_enabled;

void xinput_enable(bool on, bool xid)
{
    s_enabled = on;
    s_xid = xid;
}

/* TinyUSB asks for extra class drivers once, at stack init. */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
#if CONFIG_KVM_ENABLE_USB_VIDEO
    static usbd_class_driver_t drivers[2];
    *count = 0;
    if (s_enabled) {
        drivers[(*count)++] = k_driver;
    }
    drivers[(*count)++] = usb_video_driver;
    return drivers;
#else
    *count = s_enabled ? 1 : 0;
    return s_enabled ? &k_driver : NULL;
#endif
}

bool xinput_ready(void)
{
    return s_open && tud_ready() && !usbd_edpt_busy(s_rhport, s_ep_in);
}

bool xinput_send(const uint8_t report[XINPUT_REPORT_LEN])
{
    if (!s_open || !tud_ready()) {
        return false;
    }
    if (!usbd_edpt_claim(s_rhport, s_ep_in)) {
        return false;
    }
    memcpy(s_in_buf, report, XINPUT_REPORT_LEN);
    memcpy(s_last, report, XINPUT_REPORT_LEN);
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, s_in_buf, XINPUT_REPORT_LEN, false)) {
        (void)usbd_edpt_release(s_rhport, s_ep_in);
        return false;
    }
    return true;
}
