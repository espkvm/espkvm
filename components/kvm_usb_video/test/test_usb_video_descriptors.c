/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "usb_video_driver.h"
#include "class/cdc/cdc.h"
#include "class/msc/msc.h"
#include "xinput.h"

size_t xinput_iface_desc(uint8_t *out, uint8_t itf, uint8_t ep_in, uint8_t ep_out)
{
    (void)out; (void)itf; (void)ep_in; (void)ep_out;
    abort();
}

size_t xid_iface_desc(uint8_t *out, uint8_t ep_in, uint8_t ep_out)
{
    (void)out; (void)ep_in; (void)ep_out;
    abort();
}

#include "config_descriptor.inc"

typedef void *esp_timer_handle_t;
typedef void *SemaphoreHandle_t;
typedef int StaticSemaphore_t;
#define portMAX_DELAY 0
#define USB_REATTACH_LONG_MS 1000
#define pdMS_TO_TICKS(ms) (ms)

static atomic_bool s_replugging;
static int64_t now_us;
static int64_t alarm_us;
static bool reconnect_locked;
static unsigned attaches, detaches;
static unsigned detaches_before_restart;
static const uint8_t *connected_fs;
static const uint8_t *connected_hs;

static void xSemaphoreTake(SemaphoreHandle_t mutex, int timeout)
{
    (void)timeout;
    assert(mutex && !reconnect_locked);
    reconnect_locked = true;
}

static void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex && reconnect_locked);
    reconnect_locked = false;
}

static int64_t esp_timer_get_time(void) { return now_us; }

static void esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer);
    alarm_us = 0;
}

static void esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay_us)
{
    assert(timer && !alarm_us);
    alarm_us = now_us + delay_us;
}

static void vTaskDelay(uint32_t ms);

#include "reconnect.inc"

static void vTaskDelay(uint32_t ms)
{
    assert(!reconnect_locked);
    assert(detaches == detaches_before_restart + 1);
    now_us += (int64_t)ms * 1000;
    if (s_reconnect_timer) {
        reconnect_cb(NULL);
    }
}

bool tud_connect(void)
{
    ++attaches;
    connected_fs = kvm_usb_config_descriptor(s_fs_config_descriptor);
    connected_hs = kvm_usb_config_descriptor(s_hs_config_descriptor);
    return true;
}

bool tud_disconnect(void) { ++detaches; return true; }

static void check_reconnect(void)
{
    s_reconnect_mutex = &s_reconnect_mutex_storage;
    s_usb_driver_installed = true;
    s_reconnect_timer = &alarm_us;
    assert(kvm_usb_config_descriptor(s_hs_config_descriptor) == s_hs_config_without_video);

    usb_video_visibility(false);
    assert(detaches == 0);
    usb_video_visibility(true);
    assert(detaches == 1 && alarm_us == 1000000);
    usb_video_visibility(true);
    assert(detaches == 1 && alarm_us == 1000000);
    assert(kvm_usb_config_descriptor(s_fs_config_descriptor) == s_fs_config_without_video);

    /* The first expiry is queued just as a longer replug starts. */
    now_us = alarm_us;
    reattach(USB_REATTACH_LONG_MS);
    reconnect_cb(NULL);
    assert(attaches == 0 && s_replugging && alarm_us == 2000000);

    usb_video_visibility(false);
    usb_video_visibility(true);
    assert(detaches == 2);
    now_us = alarm_us;
    alarm_us = 0;
    reconnect_cb(NULL);
    assert(attaches == 1 && !s_replugging);
    assert(connected_fs == s_fs_config_descriptor && connected_hs == s_hs_config_descriptor);

    usb_video_visibility(false);
    assert(detaches == 3 && s_replugging);
    now_us = alarm_us;
    reconnect_cb(NULL);
    assert(attaches == 2 && !s_replugging);
    assert(connected_fs == s_fs_config_without_video && connected_hs == s_hs_config_without_video);

    assert(finish_initial_reattach() && attaches == 3);

    /* A queued reattach can dispatch during the restart's detach interval. */
    usb_video_visibility(true);
    assert(alarm_us > now_us);
    assert(!finish_initial_reattach() && attaches == 3);
    detaches_before_restart = detaches;
    usb_hid_prepare_restart();
    assert(attaches == 3 && s_replugging && alarm_us == 0);

    const unsigned shutdown_detaches = detaches;
    reattach(USB_REATTACH_LONG_MS);
    assert(detaches == shutdown_detaches && alarm_us == 0);
    assert(!finish_initial_reattach() && attaches == 3);
}

static void check_restart_without_timer(void)
{
    s_preparing_restart = false;
    s_replugging = false;
    s_reconnect_timer = NULL;
    assert(finish_initial_reattach());
    const unsigned attaches_before_restart = attaches;

    detaches_before_restart = detaches;
    const int64_t restart_at = now_us;
    usb_hid_prepare_restart();
    assert(detaches == detaches_before_restart + 1 && s_replugging);
    assert(now_us - restart_at == USB_REATTACH_LONG_MS * 1000);
    assert(!finish_initial_reattach() && attaches == attaches_before_restart);
}

static void check(bool fs, bool keyboard, pad_mode_t pad, bool media, bool tools, bool audio, bool video)
{
    uint8_t *desc = fs ? s_fs_config_descriptor : s_hs_config_descriptor;
    uint8_t *without = fs ? s_fs_config_without_video : s_hs_config_without_video;
    const uint8_t *msc = fs ? k_msc_iface_fs : k_msc_iface_hs;
    const size_t len = build_config_descriptor(desc, fs, keyboard, pad, media, msc, sizeof(k_msc_iface_hs),
                                               tools, audio, true);
    const size_t without_len = build_config_descriptor(without, fs, keyboard, pad, media, msc,
                                                       sizeof(k_msc_iface_hs), tools, audio, false);
    assert(len == tu_le16toh(tu_unaligned_read16(desc + 2)));
    assert(without_len == tu_le16toh(tu_unaligned_read16(without + 2)));
    assert(desc[4] == without[4] + video);
    assert(len == without_len + (video ? 23 : 0));
    assert(!memcmp(desc + desc[0], without + without[0], without_len - without[0]));
    bool seen[256] = {false};
    unsigned interfaces = 0, in_count = 1, video_endpoints = 0, cdc_endpoints = 0;
    unsigned rx_packet = CFG_TUD_ENDPOINT0_SIZE;
    unsigned tx_words = CFG_TUD_ENDPOINT0_SIZE / 4;
    uint8_t interface_class = 0;
    bool video_interface = false;
    for (size_t i = desc[0]; i < len; i += desc[i]) {
        const uint8_t *block = desc + i;
        assert(block[0] >= 2 && i + block[0] <= len);
        if (block[1] == TUSB_DESC_INTERFACE) {
            if (block[3] == 0) {
                assert(block[2] == interfaces++);
            }
            interface_class = block[5];
            video_interface = interface_class == TUSB_CLASS_VENDOR_SPECIFIC &&
                              block[6] == USB_VIDEO_SUBCLASS && block[7] == USB_VIDEO_PROTOCOL;
            if (video_interface) {
                assert(without[4] == block[2] && block[4] == 2);
            }
        } else if (block[1] == TUSB_DESC_ENDPOINT) {
            const uint8_t address = block[2];
            const unsigned packet = tu_le16toh(tu_unaligned_read16(block + 4));
            assert(address && (address & 0x7f) < 16 && !seen[address]);
            seen[address] = true;
            if (address & 0x80) {
                ++in_count;
                tx_words += (packet + 3) / 4;
            } else if (packet > rx_packet) {
                rx_packet = packet;
            }
            if (video_interface) {
                assert(block[3] == TUSB_XFER_BULK && packet == (fs ? 64 : 512));
                video_endpoints |= (address & 0x80) ? 2 : 1;
            } else if (interface_class == TUSB_CLASS_CDC) {
                assert(address == EPNUM_CDC_NOTIF && block[3] == TUSB_XFER_INTERRUPT && packet == 16);
                cdc_endpoints |= 4;
            } else if (interface_class == TUSB_CLASS_CDC_DATA) {
                assert(address == EPNUM_CDC_IN || address == EPNUM_CDC_OUT);
                assert(block[3] == TUSB_XFER_BULK && packet == (fs ? 64 : 512));
                cdc_endpoints |= (address & 0x80) ? 2 : 1;
            }
        }
    }
    assert(interfaces == desc[4]);
    assert(cdc_endpoints == (tools ? 7 : 0));
    if (video_endpoints != (video ? 3 : 0)) {
        fprintf(stderr, "USB video endpoints missing or unexpected: fs=%d keyboard=%d pad=%d media=%d tools=%d audio=%d\n",
                fs, keyboard, pad, media, tools, audio);
    }
    assert(video_endpoints == (video ? 3 : 0));
    /* calc_device_grxfsiz includes two RX status words per endpoint number.
     * dfifo_device_init reserves two more per endpoint number for DMA. */
    const unsigned endpoint_numbers = 16;
    const unsigned rx_words = 14 + 2 * ((rx_packet + 3) / 4 + 1) + 2 * endpoint_numbers;
    const unsigned dma_words = 2 * endpoint_numbers;
    assert(in_count <= 8 && tx_words + rx_words + dma_words <= 1024);
}

int main(void)
{
    for (unsigned fs = 0; fs <= 1; ++fs) {
        check(fs, false, PAD_OFF, false, false, false, true);
        check(fs, false, PAD_OFF, false, true, false, true);
        check(fs, false, PAD_OFF, true, true, false, true);
        check(fs, false, PAD_OFF, true, true, true, true);
        check(fs, false, PAD_ADDED, true, false, true, true);
        check(fs, true, PAD_OFF, false, false, false, false);
        check(fs, false, PAD_ALONE, false, false, false, false);
    }
    check_reconnect();
    check_restart_without_timer();
    puts("USB video composite descriptor tests passed");
}
