/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_video_driver.h"
#include "usb_video_transport.h"

enum { OUT = 0x05, IN = 0x85, PACKET = 512 };
static struct transfer {
    uint8_t *buffer;
    uint16_t size;
    bool busy;
} out, in;
static unsigned readable, resets;

static struct transfer *endpoint(uint8_t address)
{
    assert(address == OUT || address == IN);
    return address == OUT ? &out : &in;
}

bool usbd_edpt_claim(uint8_t port, uint8_t address)
{
    assert(port == 0);
    return !endpoint(address)->busy;
}

bool usbd_edpt_release(uint8_t port, uint8_t address)
{
    assert(port == 0 && !endpoint(address)->busy);
    return true;
}

bool usbd_edpt_xfer(uint8_t port, uint8_t address, uint8_t *data, uint16_t size, bool is_isr)
{
    assert(!is_isr && usbd_edpt_claim(port, address));
    assert(size <= PACKET && (size == 0 || data));
    *endpoint(address) = (struct transfer){data, size, true};
    return true;
}

bool usbd_edpt_xfer_fifo(uint8_t port, uint8_t address, tu_fifo_t *fifo, uint16_t size, bool is_isr)
{
    (void)port;
    (void)address;
    (void)fifo;
    (void)size;
    (void)is_isr;
    abort();
}

bool usbd_open_edpt_pair(uint8_t port, const uint8_t *desc, uint8_t count, uint8_t type,
                        uint8_t *ep_out, uint8_t *ep_in)
{
    assert(port == 0 && count == 2 && type == TUSB_XFER_BULK);
    *ep_out = ((const tusb_desc_endpoint_t *)desc)->bEndpointAddress;
    *ep_in = ((const tusb_desc_endpoint_t *)tu_desc_next(desc))->bEndpointAddress;
    assert(*ep_out == OUT && *ep_in == IN);
    return true;
}

void usb_video_transport_event(bool reset)
{
    if (reset) {
        resets++;
    } else {
        readable++;
    }
}

/* The runtime and ISR entry points are unused by the endpoint stream helpers. */
void dcd_int_handler(uint8_t port) { (void)port; abort(); }
bool tud_deinit(uint8_t port) { (void)port; abort(); }
bool tud_inited(void) { abort(); }
bool tud_rhport_init(uint8_t port, const tusb_rhport_init_t *init) { (void)port; (void)init; abort(); }
uint32_t tusb_time_millis_api(void) { abort(); }

static void complete(uint8_t address, uint16_t size)
{
    struct transfer *transfer = endpoint(address);
    assert(transfer->busy && size <= transfer->size);
    transfer->busy = false;
    assert(usb_video_driver.xfer_cb(0, address, XFER_RESULT_SUCCESS, size));
}

int main(void)
{
    static const uint8_t desc[] = {
        9, TUSB_DESC_INTERFACE, 1, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, USB_VIDEO_SUBCLASS, USB_VIDEO_PROTOCOL, 0,
        7, TUSB_DESC_ENDPOINT, OUT, TUSB_XFER_BULK, 0, 2, 0,
        7, TUSB_DESC_ENDPOINT, IN, TUSB_XFER_BULK, 0, 2, 0,
    };
    const tusb_desc_interface_t *interface = (const void *)desc;
    uint8_t packet[PACKET];
    memset(packet, 0x5a, sizeof(packet));
    usb_video_driver.init();
    assert(usb_video_driver.open(0, interface, sizeof(desc) - 1) == 0);
    assert(usb_video_driver.open(0, interface, sizeof(desc)) == sizeof(desc));
    assert(usb_video_transport_mounted() && !out.busy && !in.busy);

    uint8_t *rx = malloc(USB_VIDEO_RX_FIFO_SIZE);
    uint8_t *tx = malloc(USB_VIDEO_TX_FIFO_SIZE);
    assert(rx && tx);
    usb_video_transport_attach(rx, tx);
    assert(!out.busy && usb_video_transport_write(packet, sizeof(packet)) == 0);
    usb_video_transport_reset_session();
    assert(out.busy && out.size == PACKET);
    for (unsigned i = 0; i < USB_VIDEO_RX_FIFO_SIZE / PACKET; i++) {
        memcpy(out.buffer, packet, sizeof(packet));
        complete(OUT, PACKET);
    }
    assert(usb_video_transport_pending() && !out.busy && readable == USB_VIDEO_RX_FIFO_SIZE / PACKET);
    uint8_t received[PACKET];
    assert(usb_video_transport_read(received, sizeof(received)) == sizeof(received));
    assert(memcmp(received, packet, sizeof(packet)) == 0 && out.busy);

    assert(usb_video_transport_write(packet, sizeof(packet)) == sizeof(packet));
    assert(in.busy && in.size == PACKET && memcmp(in.buffer, packet, sizeof(packet)) == 0);
    complete(IN, PACKET);
    assert(in.busy && in.size == 0);
    complete(IN, 0);
    assert(usb_video_transport_write(packet, sizeof(packet)) == sizeof(packet));

    usb_video_transport_detach();
    free(rx);
    free(tx);
    assert(usb_video_transport_mounted() && !usb_video_transport_pending());
    complete(IN, PACKET);
    complete(OUT, PACKET);
    assert(!in.busy && !out.busy && readable == USB_VIDEO_RX_FIFO_SIZE / PACKET);

    usb_video_driver.reset(0);
    assert(resets == 1 && !usb_video_transport_mounted());
    assert(usb_video_driver.open(0, interface, sizeof(desc)) == sizeof(desc));
    assert(usb_video_transport_mounted() && !out.busy);

    uint8_t next_rx[USB_VIDEO_RX_FIFO_SIZE];
    uint8_t next_tx[USB_VIDEO_TX_FIFO_SIZE];
    usb_video_transport_attach(next_rx, next_tx);
    usb_video_transport_reset_session();
    memcpy(out.buffer, packet, sizeof(packet));
    complete(OUT, PACKET);
    assert(usb_video_transport_pending() && out.busy);
    out.busy = false;
    assert(usb_video_driver.xfer_cb(0, OUT, XFER_RESULT_FAILED, 0));
    assert(resets == 2 && !usb_video_transport_pending() && !out.busy);
    usb_video_transport_detach();
    puts("USB video dormant, active, and detached endpoint tests passed");
}
