/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_KVM_ENABLE_USB_VIDEO
#include <stdatomic.h>

#include "usb_video_driver.h"
#include "usb_video_transport.h"

/* An OUT transfer can still own its DMA buffer after sharing is disabled.
 * Keep one packet per endpoint valid until completion or bus reset. The
 * larger software FIFOs are installed only for the lifetime of the receiver. */
CFG_TUD_MEM_SECTION static struct {
    TUD_EPBUF_DEF(out, 512);
    TUD_EPBUF_DEF(in, 512);
} packets;

static tu_edpt_stream_t rx = {.ep_buf = packets.out};
static tu_edpt_stream_t tx = {.ep_buf = packets.in};
static atomic_bool mounted;
static atomic_bool pending;
static bool receiving;

bool usb_video_transport_mounted(void)
{
    return mounted;
}

bool usb_video_transport_pending(void)
{
    return pending;
}

void usb_video_transport_attach(uint8_t *rx_buffer, uint8_t *tx_buffer)
{
    tu_fifo_config(&rx.ff, rx_buffer, USB_VIDEO_RX_FIFO_SIZE, false);
    tu_fifo_config(&tx.ff, tx_buffer, USB_VIDEO_TX_FIFO_SIZE, false);
}

void usb_video_transport_detach(void)
{
    receiving = false;
    pending = false;
    rx.ff = (tu_fifo_t){0};
    tx.ff = (tu_fifo_t){0};
}

void usb_video_transport_reset_session(void)
{
    tu_edpt_stream_clear(&rx);
    tu_edpt_stream_clear(&tx);
    pending = false;
    receiving = mounted;
    if (receiving) {
        tu_edpt_stream_read_xfer(&rx);
    }
}

size_t usb_video_transport_read(uint8_t *data, size_t size)
{
    if (!receiving) {
        return 0;
    }
    const size_t count = tu_edpt_stream_read(&rx, data, size);
    pending = !tu_edpt_stream_empty(&rx);
    return count;
}

size_t usb_video_transport_write(const uint8_t *data, size_t size)
{
    if (!receiving || tu_edpt_stream_write_available(&tx) < size) {
        return 0;
    }
    const size_t count = tu_edpt_stream_write(&tx, data, size);
    tu_edpt_stream_write_xfer(&tx);
    return count;
}

static void transport_reset(uint8_t rhport)
{
    (void)rhport;
    const bool was_mounted = atomic_exchange(&mounted, false);
    receiving = false;
    pending = false;
    tu_edpt_stream_close(&rx);
    tu_edpt_stream_close(&tx);
    if (was_mounted) {
        usb_video_transport_event(true);
    }
}

static void transport_init(void)
{
    transport_reset(0);
}

static uint16_t transport_open(uint8_t rhport, const tusb_desc_interface_t *interface, uint16_t size)
{
    if (size < USB_VIDEO_INTERFACE_LEN || interface->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC ||
        interface->bInterfaceSubClass != USB_VIDEO_SUBCLASS || interface->bInterfaceProtocol != USB_VIDEO_PROTOCOL) {
        return 0;
    }
    uint8_t ep_out, ep_in;
    if (!usbd_open_edpt_pair(rhport, tu_desc_next(interface), 2, TUSB_XFER_BULK, &ep_out, &ep_in)) {
        return 0;
    }
    const tusb_desc_endpoint_t *first = (const void *)tu_desc_next(interface);
    const tusb_desc_endpoint_t *second = (const void *)tu_desc_next(first);
    const tusb_desc_endpoint_t *out = first->bEndpointAddress == ep_out ? first : second;
    const tusb_desc_endpoint_t *in = first->bEndpointAddress == ep_in ? first : second;
    tu_edpt_stream_open(&rx, rhport, out, tu_edpt_packet_size(out));
    tu_edpt_stream_open(&tx, rhport, in, sizeof(packets.in));
    mounted = true;
    return USB_VIDEO_INTERFACE_LEN;
}

static bool transport_control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    (void)rhport;
    (void)stage;
    (void)request;
    return false; /* TinyUSB dispatches vendor requests to the global callback. */
}

static bool transport_complete(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t count)
{
    (void)rhport;
    if (!receiving) {
        return true;
    }
    if (result != XFER_RESULT_SUCCESS) {
        receiving = false;
        pending = false;
        usb_video_transport_event(true);
    } else if (ep == rx.ep_addr) {
        tu_edpt_stream_read_xfer_complete(&rx, count);
        pending = !tu_edpt_stream_empty(&rx);
        usb_video_transport_event(false);
        tu_edpt_stream_read_xfer(&rx);
    } else if (!tu_edpt_stream_write_xfer(&tx)) {
        tu_edpt_stream_write_zlp_if_needed(&tx, count);
    }
    return true;
}

const usbd_class_driver_t usb_video_driver = {
    .name = "USB VIDEO",
    .init = transport_init,
    .reset = transport_reset,
    .open = transport_open,
    .control_xfer_cb = transport_control,
    .xfer_cb = transport_complete,
};
#endif
