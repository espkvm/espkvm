/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture_pixfmt.h"

#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_h264_enc_single_hw.h"
#include "driver/jpeg_encode.h"

#if !CAPTURE_DIRECT_ENCODE
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
};
#endif

const capture_pixfmt_t *capture_pixfmt(void)
{
#if CAPTURE_DIRECT_ENCODE
    return &k_direct;
#else
    return &k_rgb888;
#endif
}
