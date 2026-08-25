/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

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
    uint32_t encode_us;        /**< mean time the encoder alone took per frame */
    uint32_t ppa_us;           /**< mean PPA colour-conversion time per frame (H.264) */
    uint32_t encoder_busy_pct; /**< share of wall clock spent in conversion + encode */
    uint8_t sys_status;    /**< raw TC358743 SYS_STATUS, for diagnostics */
    /**
     * How long the picture has been one flat colour, in ms; 0 when it is not.
     *
     * The text reader covers screens drawn from a character generator. This
     * covers the other kind of bad news - a Windows stop screen, a blanked
     * output, a desktop that died into its background - which has no grid to
     * read and is nearly all one colour. See capture_flat.c.
     */
    uint32_t flat_ms;
} kvm_video_status_t;

void capture_start(void);

void capture_status_get(kvm_video_status_t *out);

/**
 * The I2C master bus the capture bridge lives on (I2C_NUM_0, the board's
 * TC358743 SDA/SCL). Shared so an optional status OLED wired to the same two
 * lines can be probed and driven without any dedicated pins. NULL before the
 * capture hardware has been brought up.
 */
/**
 * Create the I2C bus the capture chip and the status OLED share, if it does not
 * exist yet. Safe to call more than once; call it early if anything other than
 * the capture path needs the bus before capture starts.
 */
esp_err_t capture_i2c_bus_init(void);

i2c_master_bus_handle_t capture_i2c_bus(void);

#ifdef __cplusplus
}
#endif
