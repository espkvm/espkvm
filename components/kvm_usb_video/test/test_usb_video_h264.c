/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_video_h264.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* SPS from the macOS VideoToolbox H.264 encoder, 1920x1200 at 30 fps. */
static const uint8_t sps_vt[] = {
    0x27, 0x64, 0x00, 0x32, 0xac, 0x56, 0x80, 0x78, 0x02, 0x5e, 0x40,
};
/* SPS from libx264 baseline, 1920x1080 visible with 1088 coded lines. */
static const uint8_t sps_baseline[] = {
    0x67, 0x42, 0xc0, 0x28, 0xd9, 0x00, 0x78, 0x02, 0x27, 0xe5, 0xc0, 0x44, 0x00, 0x00, 0x03,
    0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xf0, 0x3c, 0x60, 0xc9, 0x20,
};
/* SPS from libx264 high, 2048x1200. */
static const uint8_t sps_wide[] = {
    0x67, 0x64, 0x00, 0x32, 0xac, 0xd9, 0x40, 0x20, 0x00, 0x97, 0xb0, 0x11, 0x00, 0x00, 0x03,
    0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x3c, 0x0f, 0x18, 0x31, 0x96,
};

static size_t append(uint8_t *out, const uint8_t *nal, size_t len)
{
    const uint8_t start[] = {0, 0, 0, 1};
    memcpy(out, start, sizeof(start));
    memcpy(out + sizeof(start), nal, len);
    return sizeof(start) + len;
}

static size_t keyframe(uint8_t *out, const uint8_t *sps, size_t len)
{
    const uint8_t pps[] = {0x68, 0xff};
    const uint8_t idr[] = {0x65, 0xff};
    size_t size = append(out, sps, len);
    size += append(out + size, pps, sizeof(pps));
    size += append(out + size, idr, sizeof(idr));
    return size;
}

int main(void)
{
    uint8_t au[128];
    size_t len = keyframe(au, sps_vt, sizeof(sps_vt));
    assert(usb_video_h264_access_unit(au, len, 3, 1920, 1200));
    assert(!usb_video_h264_access_unit(au, len, 3, 1920, 1198));
    assert(!usb_video_h264_access_unit(au, len, 0, 1920, 1200));
    au[7] = 60;
    assert(!usb_video_h264_access_unit(au, len, 3, 1920, 1200));
    au[7] = sps_vt[3];
    au[5] = 110;
    assert(!usb_video_h264_access_unit(au, len, 3, 1920, 1200));
    len = keyframe(au, sps_baseline, sizeof(sps_baseline));
    assert(usb_video_h264_access_unit(au, len, 3, 1920, 1080));
    assert(!usb_video_h264_access_unit(au, len, 3, 1918, 1080));
    len += append(au + len, sps_wide, sizeof(sps_wide));
    assert(!usb_video_h264_access_unit(au, len, 3, 1920, 1200));
    len = keyframe(au, sps_wide, sizeof(sps_wide));
    assert(usb_video_h264_access_unit(au, len, 3, 2048, 1200));
    assert(!usb_video_h264_access_unit(au, len, 3, 1920, 1200));
    for (size_t n = 1; n < sizeof(sps_vt); n++) {
        len = keyframe(au, sps_vt, n);
        assert(!usb_video_h264_access_unit(au, len, 3, 1920, 1200));
    }
    const uint8_t non_idr[] = {0, 0, 1, 0x61, 0xff};
    assert(usb_video_h264_access_unit(non_idr, sizeof(non_idr), 0, 1920, 1200));
    assert(!usb_video_h264_access_unit(non_idr, sizeof(non_idr) - 2, 0, 1920, 1200));
    puts("USB video access unit checks passed");
}
