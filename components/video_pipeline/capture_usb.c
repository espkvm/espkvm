/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"

#if CONFIG_KVM_ENABLE_USB_VIDEO
#include <stdatomic.h>
#include "capture_priv.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "kvm_caps.h"
#include "kvm_regions.h"
#include "usb_video.h"

static uint8_t *slots[VIDEO_SLOT_COUNT];
static atomic_bool running;
static uint32_t stream_sequence;
static bool await_keyframe;

bool capture_usb_wanted(void)
{
    usb_video_status_t status;
    return usb_video_status(&status) && status.playing;
}

bool capture_usb_running(void)
{
    return running;
}

void capture_usb_update_status(kvm_video_status_t *out)
{
    usb_video_status_t status;
    if (!running || !usb_video_status(&status)) {
        return;
    }
    out->signal = status.last_frame_us != 0;
    out->hres = status.width;
    out->vres = status.height;
    out->input_hz = status.fps;
    out->interlaced = false;
    out->too_fast = false;
    out->flat_ms = 0;
    out->flat_dark = false;
    out->frames = status.frames;
    out->frame_age_ms = out->signal ? (esp_timer_get_time() - status.last_frame_us) / 1000 : UINT32_MAX;
    if (!out->signal) {
        out->fps_x100 = 0;
        out->kbps = 0;
    }
}

static void usb_close(void)
{
    if (atomic_exchange(&running, false)) {
        kvm_cap_report(KVM_CAP_LOCAL_FRAMES, true, NULL);
    }
    for (size_t i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        heap_caps_free(slots[i]);
        slots[i] = NULL;
    }
}

static esp_err_t usb_open(void)
{
    capture_release_idle_buffers();
    for (size_t i = 0; i < VIDEO_SLOT_COUNT; ++i) {
        slots[i] = heap_caps_malloc(USB_VIDEO_FRAME_CAP, MALLOC_CAP_SPIRAM);
        if (!slots[i]) {
            usb_close();
            return ESP_ERR_NO_MEM;
        }
    }
    video_frame_install(VIDEO_PAYLOAD_H264, slots, VIDEO_SLOT_COUNT, USB_VIDEO_FRAME_CAP);
    capture_screentext_forget();
    kvm_regions_forget();
    kvm_cap_report(KVM_CAP_LOCAL_FRAMES, false, CAPTURE_USB_LOCAL_FRAMES_UNAVAILABLE);
    running = true;
    capture_snapshot_usb_started();
    await_keyframe = true;
    usb_video_request_keyframe();
    return ESP_OK;
}

static esp_err_t usb_publish(capture_ctx_t *c, const void *src, bool force)
{
    (void)c;
    (void)src;
    (void)force;
    usb_video_status_t status;
    if (!usb_video_status(&status)) {
        return ESP_OK;
    }
    if (stream_sequence != status.sequence) {
        video_frame_invalidate();
        stream_sequence = status.sequence;
        await_keyframe = true;
    }
    if (!status.last_frame_us) {
        return ESP_OK;
    }
    if (video_frame_take_keyframe_request()) {
        usb_video_request_keyframe();
    }
    int slot;
    uint8_t *data;
    size_t cap, len;
    bool keyframe;
    if (video_frame_begin_write(&slot, &data, &cap, 0) != ESP_OK ||
        !usb_video_take(stream_sequence, data, cap, &len, &keyframe)) {
        return ESP_OK;
    }
    if (await_keyframe && !keyframe) {
        usb_video_request_keyframe();
        return ESP_OK;
    }
    await_keyframe = false;
    video_frame_set_source_time(status.last_frame_us);
    video_frame_publish(slot, len, keyframe);
    capture_status_add_frame(len);
    return ESP_OK;
}

const capture_codec_t *capture_codec_usb(void)
{
    static const capture_codec_t codec = {
        .name = "USB screen sharing",
        .payload = VIDEO_PAYLOAD_H264,
        .open = usb_open,
        .close = usb_close,
        .encode = usb_publish,
    };
    return &codec;
}
#endif
