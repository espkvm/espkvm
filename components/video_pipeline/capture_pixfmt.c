/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture_pixfmt.h"

#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_h264_enc_single_hw.h"
#include "driver/jpeg_encode.h"

#if !CAPTURE_DIRECT_ENCODE && !CAPTURE_YUV_SWAP
/*
 * rev < 3.0 (Waveshare): capture RGB888 at 0x24, and a PPA pass turns it into
 * YUV420 for whichever encoder is running. This is the original, shared path.
 */
static const capture_pixfmt_t k_rgb888 = {
    .name = "rgb888",
    .csi_dt = 0x24u,
    .bpp = 24u,
    .v_align_mb = false,
    .direct = false,
    .cam_color = CAM_CTLR_COLOR_RGB888,
    .isp_color = ISP_COLOR_RGB888,
    .h264_pic = ESP_H264_RAW_FMT_O_UYY_E_VYY,
    .jpeg_src = JPEG_ENCODE_IN_FORMAT_RGB888,
    /* RGB in: the engine converts and can subsample to 4:2:0. */
    .jpeg_subsample = JPEG_DOWN_SAMPLING_YUV420,
    /* The PPA pass is set to full range for H.264. JPEG converts RGB itself. */
    .full_range = true,
};
#endif

#if CAPTURE_DIRECT_ENCODE
/*
 * rev >= 3.0 (Function-EV): capture native YUV422 (UYVY, 2 bytes/px) at 0x1e and
 * feed it to the encoder directly - no PPA, and a third less CSI-DMA PSRAM traffic
 * than RGB888 (4 vs 6 MB/frame at 1080p), which is the actual bottleneck. Both
 * encoders take it as-is: the H.264 hardware accepts ESP_H264_RAW_FMT_UYVY on this
 * rev, and the JPEG engine's "YUV422" input (fourcc names the little-endian word,
 * whose bytes are U-Y-V-Y) is exactly UYVY - so a single capture serves both. The
 * frame is padded to macroblock height for the H.264 encoder.
 *
 * The TC358743 emits UYVY only when capture_hw enables it (kvm_bridge_set_csi_uyvy422),
 * which it keys off this profile; on rev < 3.0 nothing here compiles and the bridge
 * stays RGB888.
 */
static const capture_pixfmt_t k_direct = {
    .name = "yuv422",
    .csi_dt = 0x1eu,
    .bpp = 16u,
    .v_align_mb = true,
    .direct = true,
    .cam_color = CAM_CTLR_COLOR_YUV422_UYVY,
    .isp_color = ISP_COLOR_YUV422,
    .h264_pic = ESP_H264_RAW_FMT_UYVY,
    .jpeg_src = JPEG_ENCODE_IN_FORMAT_YUV422,
    /* Packed UYVY in: the JPEG engine can't resample 4:2:2 to 4:2:0, so the
     * output subsampling must stay 4:2:2 or the chroma corrupts (acid colours,
     * vertical banding). */
    .jpeg_subsample = JPEG_DOWN_SAMPLING_YUV422,
    /* The TC358743 is told to send full range (tc358743.c). */
    .full_range = true,
};
#endif

#if CAPTURE_YUV_SWAP
/*
 * rev < 3.0 with an LT6911D: capture the YUV422 the bridge sends, then let the
 * PPA put the bytes in the order the JPEG engine reads. H.264 is out of reach
 * on this silicon whatever we do - its encoder takes only YUV420 with line
 * prefixes, and every PPA mode that would produce it from YUV422 is gated on
 * rev 3.0 as well.
 */
static const capture_pixfmt_t k_yuv422_swap = {
    .name = "yuv422+swap",
    .csi_dt = 0x1eu,
    .bpp = 16u,
    .v_align_mb = false,
    .direct = false,
    .swap_bytes = true,
    /* The LT6911D puts luma in the even bytes; the TC358743 profile above does
     * not, and everything that reads the captured frame without an encoder -
     * the screen-text reader, the blank-screen check - has to know which. */
    .luma_first = true,
    .cam_color = CAM_CTLR_COLOR_YUV422_UYVY,
    .isp_color = ISP_COLOR_YUV422,
    .h264_pic = ESP_H264_RAW_FMT_O_UYY_E_VYY, /* never reached: no H.264 here */
    .jpeg_src = JPEG_ENCODE_IN_FORMAT_YUV422,
    .jpeg_subsample = JPEG_DOWN_SAMPLING_YUV422,
    /* The LT6911D's own firmware picks the range, and there is no register map
     * to ask it. Bridges send limited range by default. */
    .full_range = false,
};
#endif

const capture_pixfmt_t *capture_pixfmt(void)
{
#if CAPTURE_DIRECT_ENCODE
    return &k_direct;
#elif CAPTURE_YUV_SWAP
    return &k_yuv422_swap;
#else
    return &k_rgb888;
#endif
}
