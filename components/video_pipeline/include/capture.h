/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Live state of the capture path, for the REST API and the status bar. */
typedef struct {
    bool signal;           /**< HDMI is locked and delivering pixels */
    uint32_t hres;         /**< active mode, 0 until the first lock */
    uint32_t vres;
    bool interlaced;
    uint32_t fps_x100;     /**< encoded frames per second, hundredths */
    uint32_t kbps;         /**< encoded bitrate, kbit/s */
    uint32_t mode_changes; /**< resolution switches handled since boot */
    uint32_t skipped_fps_x100; /**< frames dropped as unchanged, per second */
    uint32_t encode_us;        /**< mean time one frame took to encode */
    uint32_t encoder_busy_pct; /**< share of wall clock spent encoding */
    uint8_t sys_status;    /**< raw TC358743 SYS_STATUS, for diagnostics */
} kvm_video_status_t;

void capture_start(void);

void capture_status_get(kvm_video_status_t *out);

#ifdef __cplusplus
}
#endif
