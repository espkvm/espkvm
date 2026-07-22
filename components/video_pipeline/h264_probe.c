/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Decide at boot whether this chip can carry an H.264 path, and at what cost.
 *
 * Three blocks on the ESP32-P4 can convert colour, and on silicon older than
 * revision 3.0 two of them refuse the job:
 *
 *   CSI bridge  RGB888 -> YUV420   rev >= 3.0 only: esp_cam_ctlr_csi.c checks
 *                                  efuse_hal_chip_revision() and returns
 *                                  ESP_ERR_NOT_SUPPORTED below that
 *   ISP         RGB888 -> YUV420   never: its input stage accepts RAW8/10/12
 *                                  only, it is a Bayer pipeline
 *   PPA         RGB888 -> YUV420   yes
 *
 * So the path is CSI -> PSRAM(RGB888) -> PPA -> PSRAM(YUV420) -> encoder, and
 * the PPA pass is what limits it. Measured on an ESP32-P4 v1.3, one frame
 * converted and encoded, the two stages overlapped:
 *
 *     1920x1080   PPA 76.3 ms + encode 30.9 ms   13 fps
 *     1280x720    PPA 33.6 ms + encode 13.7 ms   29 fps
 *      960x540    PPA 19.1 ms + encode  7.9 ms   52 fps
 *      640x480    PPA 11.4 ms + encode  4.7 ms   87 fps
 *
 * Cost is linear in pixel count: the PPA is bandwidth-bound, not compute-bound.
 *
 * MEASURED AFTER THE PIPELINE WAS BUILT, and worse than these numbers suggest.
 * With the real capture running, one 1080p frame costs 145 ms end to end, not
 * the 107 ms the two stages add up to here, and the stream settles at 5-7 fps
 * against MJPEG's 20. Two reasons, both absent from a probe that runs alone:
 * the CSI DMA is writing 6 MB per frame into the same PSRAM the PPA is reading,
 * and capture_h264.c runs the conversion and the encode one after the other in
 * the capture task, so nothing overlaps.
 *
 * That makes H.264 a trade rather than a win on frame rate - and a rout on
 * bandwidth: measured on an idle desktop, 170 kbit/s against MJPEG's 8500.
 *
 * Two constraints learned the hard way, recorded so nobody rediscovers them:
 *
 *  - A PPA transaction that scales while writing YUV420 never completes. The
 *    driver's blocking mode waits forever, so such a call takes down whatever
 *    task issued it. Convert at the captured size; do not scale in this pass.
 *  - Probing at 1080p needs 12 MB of PSRAM, which collides with the capture
 *    buffers. The shipped probe runs at the smallest useful size and
 *    extrapolates, which the linear cost curve above justifies.
 */
#include "capture_priv.h"

#include <inttypes.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"

#include "kvm_caps.h"

/* Small enough not to compete with the capture buffers, large enough that
 * fixed overheads do not dominate the measurement. */
#define PROBE_W 640u
#define PROBE_H 480u

/** The first encoded frame is an IDR and says nothing about the steady state. */
#define PROBE_ITERATIONS 4

/** Measured cost per megapixel, used to predict the ceiling at other sizes. */
static uint32_t s_ppa_us_per_mpx;
static uint32_t s_enc_us_per_mpx;

static const char *h264_err_name(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return "ok";
    case ESP_H264_ERR_ARG:
        return "invalid argument";
    case ESP_H264_ERR_MEM:
        return "out of memory";
    case ESP_H264_ERR_UNSUPPORTED:
        return "unsupported on this chip";
    case ESP_H264_ERR_TIMEOUT:
        return "timeout";
    case ESP_H264_ERR_OVERFLOW:
        return "output buffer overflow";
    default:
        return "failed";
    }
}

/** Content that neither converts nor compresses away to nothing. */
static void fill_test_pattern(uint8_t *rgb, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = rgb + (size_t)y * w * 3u;
        for (uint32_t x = 0; x < w; x++) {
            row[x * 3 + 0] = (uint8_t)(x + y);
            row[x * 3 + 1] = (uint8_t)(x ^ y);
            row[x * 3 + 2] = (uint8_t)(x * 3 + y * 7);
        }
    }
}

uint32_t capture_h264_estimated_fps(uint32_t w, uint32_t h)
{
    if (!s_ppa_us_per_mpx || !w || !h) {
        return 0;
    }
    const uint64_t mpx_x1000 = ((uint64_t)w * h * 1000u) / 1000000u;
    const uint64_t ppa_us = (uint64_t)s_ppa_us_per_mpx * mpx_x1000 / 1000u;
    const uint64_t enc_us = (uint64_t)s_enc_us_per_mpx * mpx_x1000 / 1000u;
    /* Summed, not maximised: the two blocks could overlap, but the pipeline
     * runs them in one task, so a frame costs both. Even this is optimistic
     * once the CSI DMA is competing for PSRAM - see the note above. */
    const uint64_t total = ppa_us + enc_us;
    return total ? (uint32_t)(1000000u / total) : 0;
}

void capture_h264_probe(void)
{
    const size_t rgb_bytes = (size_t)PROBE_W * PROBE_H * 3u;
    const size_t yuv_bytes = (size_t)PROBE_W * PROBE_H * 3u / 2u;

    ppa_client_handle_t ppa = NULL;
    esp_h264_enc_handle_t enc = NULL;
    uint8_t *rgb = NULL;
    uint8_t *yuv = NULL;
    uint8_t *out_buf = NULL;
    uint32_t yuv_alloc = 0;
    uint32_t out_alloc = 0;

    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&ppa_cfg, &ppa);
    if (err != ESP_OK) {
        kvm_cap_report(KVM_CAP_H264, false, "PPA unavailable for colour conversion (%s)",
                       esp_err_to_name(err));
        return;
    }

    size_t align = 64;
    (void)esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align);
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    rgb = heap_caps_aligned_calloc(align, 1, rgb_bytes, caps);
    yuv = esp_h264_aligned_calloc(align, 1, yuv_bytes, &yuv_alloc, ESP_H264_MEM_SPIRAM);
    out_buf = esp_h264_aligned_calloc(align, 1, yuv_bytes, &out_alloc, ESP_H264_MEM_SPIRAM);
    if (!rgb || !yuv || !out_buf) {
        kvm_cap_report(KVM_CAP_H264, false, "not enough PSRAM to probe the H.264 path");
        goto cleanup;
    }
    fill_test_pattern(rgb, PROBE_W, PROBE_H);
    (void)esp_cache_msync(rgb, rgb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* scale_x/scale_y stay at 1.0 - see the note about scaling above. */
    ppa_srm_oper_config_t srm = {
        .in = {.buffer = rgb,
               .pic_w = PROBE_W,
               .pic_h = PROBE_H,
               .block_w = PROBE_W,
               .block_h = PROBE_H,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
        .out = {.buffer = yuv,
                .buffer_size = yuv_alloc,
                .pic_w = PROBE_W,
                .pic_h = PROBE_H,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    int64_t ppa_total_us = 0;
    for (int i = 0; i < PROBE_ITERATIONS; i++) {
        const int64_t t0 = esp_timer_get_time();
        err = ppa_do_scale_rotate_mirror(ppa, &srm);
        ppa_total_us += esp_timer_get_time() - t0;
        if (err != ESP_OK) {
            kvm_cap_report(KVM_CAP_H264, false, "PPA cannot convert RGB888 to YUV420 (%s)",
                           esp_err_to_name(err));
            goto cleanup;
        }
    }

    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = 30,
        .fps = 30,
        .res = {.width = PROBE_W, .height = PROBE_H},
        .rc = {.bitrate = 4 * 1000 * 1000, .qp_min = 25, .qp_max = 45},
    };
    esp_h264_err_t herr = esp_h264_enc_hw_new(&cfg, &enc);
    if (herr != ESP_H264_ERR_OK || !enc) {
        kvm_cap_report(KVM_CAP_H264, false, "hardware encoder unavailable: %s", h264_err_name(herr));
        goto cleanup;
    }
    herr = esp_h264_enc_open(enc);
    if (herr != ESP_H264_ERR_OK) {
        kvm_cap_report(KVM_CAP_H264, false, "hardware encoder refused to open: %s",
                       h264_err_name(herr));
        goto cleanup;
    }

    int64_t enc_total_us = 0;
    for (int i = 0; i < PROBE_ITERATIONS; i++) {
        esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = yuv, .len = (uint32_t)yuv_bytes},
                                      .pts = (uint32_t)(i * 33)};
        esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = out_buf, .len = out_alloc}};
        const int64_t t0 = esp_timer_get_time();
        herr = esp_h264_enc_process(enc, &in, &out);
        enc_total_us += esp_timer_get_time() - t0;
        if (herr != ESP_H264_ERR_OK) {
            kvm_cap_report(KVM_CAP_H264, false, "encode failed: %s", h264_err_name(herr));
            goto cleanup;
        }
    }

    const uint64_t mpx_x1000 = ((uint64_t)PROBE_W * PROBE_H * 1000u) / 1000000u;
    s_ppa_us_per_mpx = (uint32_t)((uint64_t)(ppa_total_us / PROBE_ITERATIONS) * 1000u / mpx_x1000);
    s_enc_us_per_mpx = (uint32_t)((uint64_t)(enc_total_us / PROBE_ITERATIONS) * 1000u / mpx_x1000);

    kvm_cap_report(KVM_CAP_H264, true, NULL);
    ESP_LOGI(CAPTURE_LOG_TAG,
             "H.264 available: %ux%u costs PPA %" PRId64 " us + encode %" PRId64
             " us; predicted ceiling %" PRIu32 " fps at 1080p, %" PRIu32 " fps at 720p",
             PROBE_W, PROBE_H, ppa_total_us / PROBE_ITERATIONS, enc_total_us / PROBE_ITERATIONS,
             capture_h264_estimated_fps(1920, 1080), capture_h264_estimated_fps(1280, 720));

cleanup:
    if (enc) {
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
    }
    if (ppa) {
        (void)ppa_unregister_client(ppa);
    }
    heap_caps_free(rgb);
    esp_h264_free(yuv);
    esp_h264_free(out_buf);
}
