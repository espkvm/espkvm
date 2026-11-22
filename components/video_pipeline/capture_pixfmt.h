/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one place the two boards' capture pipelines diverge.
 *
 * Everything that differs between the rev < 3.0 board (Waveshare ESP32-P4-ETH:
 * capture RGB888, then a PPA pass converts to YUV for the encoders) and the
 * rev >= 3.0 board (Function-EV: the encoder consumes the captured pixels
 * directly, no PPA) is a single row of this table, chosen once by chip
 * revision. The rest of the pipeline - the CSI DMA ring, the HDMI monitor, the
 * recovery path - is byte-for-byte identical and stays shared.
 *
 * Fields that name a hardware enum are stored as int so this header pulls in no
 * cam / isp / jpeg / h264 driver headers; each consumer casts to its own enum.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

typedef struct {
    const char *name;    /* "rgb888" / "bgr888" / "yuv422", for logs           */
    uint8_t csi_dt;      /* CSI-2 data type on the wire: 0x24 RGB888, 0x1e YUV422-8 */
    uint8_t bpp;         /* bits per captured pixel in DRAM: 24 or 16          */
    bool v_align_mb;     /* pad frame height up to a multiple of 16 rows?      */
    bool direct;         /* encoder reads captured pixels directly (no PPA)?   */
    int cam_color;       /* cam_ctlr_color_t for the esp_cam in/out            */
    int isp_color;       /* isp_color_t for the (bypassed) ISP in/out          */
    int h264_pic;        /* esp_h264_raw_format_t fed to the H.264 encoder     */
    int jpeg_src;        /* jpeg_enc_input_format_t fed to the JPEG encoder    */
    bool swap_bytes;     /* reorder the captured bytes before the encoder?     */
    bool luma_first;     /* packed 4:2:2 with Y in the even bytes (YUYV)?      */
    bool full_range;     /* YUV to the encoders in 0-255 rather than 16-235?   */
    int jpeg_subsample;  /* jpeg_down_sampling_type_t: must match the input's
                          * chroma - the P4 JPEG engine can't resample a packed
                          * YUV422 input down to 4:2:0, so UYVY needs 4:2:2 out  */
} capture_pixfmt_t;

/*
 * The single revision gate for the whole component. rev >= 3.0 unlocks the
 * esp_h264 direct-input formats (BGR888 today, YUV422 next), so that build feeds
 * the encoder straight from the capture buffer and skips the PPA colour convert;
 * rev < 3.0 keeps the PPA path. Every board difference keys off this one macro.
 */
#if defined(CONFIG_ESP_REV_MIN_FULL) && CONFIG_ESP_REV_MIN_FULL >= 300
#define CAPTURE_DIRECT_ENCODE 1
#else
#define CAPTURE_DIRECT_ENCODE 0
#endif

/*
 * The third case: an LT6911D on pre-3.0 silicon (M5Stack Unit PoE-P4). That
 * bridge sends YUV422 and nothing else, so RGB888 is not on offer - but the
 * bytes arrive as Y U Y V and the JPEG engine wants them the other way round,
 * and the colour-mode block that would fix it in the CSI bridge only exists
 * from rev 3.0. A PPA pass reorders them instead; see capture_yuv_swap.c.
 */
#if CONFIG_KVM_LT6911 && !CAPTURE_DIRECT_ENCODE
#define CAPTURE_YUV_SWAP 1
#else
#define CAPTURE_YUV_SWAP 0
#endif

/*
 * The fourth: any other rev < 3.0 image meeting a bridge that sends only YUV422
 * (the RK628, kvm_bridge_t.yuv422_only). The same reorder-for-JPEG path as the
 * LT6911D, chosen at start-up rather than at build time, since the same image
 * also has to drive a TC358743 in RGB888 (which H.264 on this revision needs).
 * MJPEG only. CAPTURE_YUV_CODE says the reorder code is in the image at all.
 */
#define CAPTURE_YUV_CODE (!CAPTURE_DIRECT_ENCODE)

/** The active capture pixel format: by chip revision at build time, and on rev
 *  < 3.0 by the bridge as well (capture_pixfmt_select). */
const capture_pixfmt_t *capture_pixfmt(void);

/** Bytes per captured pixel (bpp / 8): 3 for RGB/BGR888, 2 for YUV422. */
static inline unsigned capture_pixfmt_bytes(void)
{
    return capture_pixfmt()->bpp / 8u;
}

/** Pick the profile for the bridge that answered; before this, the build's own.
 *  Only a rev < 3.0 image without CAPTURE_YUV_SWAP has a choice to make. */
void capture_pixfmt_select(bool bridge_yuv422_only);
