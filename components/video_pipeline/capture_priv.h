/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "tc358743.h"
#include "video_frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CAPTURE_LOG_TAG "video"

#define CAPTURE_FB_COUNT 2

/*
 * Frame buffers are allocated once for the largest mode the bridge can deliver;
 * smaller modes use the leading part of the same allocation. Reallocating on
 * every resolution change would fragment PSRAM and can fail exactly when a
 * machine switches from its BIOS mode to the desktop.
 */
#define CAPTURE_MAX_H_RES 1920u
#define CAPTURE_MAX_V_RES 1080u
#define CAPTURE_MAX_FRAME_BYTES ((size_t)CAPTURE_MAX_H_RES * (size_t)CAPTURE_MAX_V_RES * 3u)

/** Shared CSI / ISP / HDMI state for codec tasks (lives in capture_hw.c). */
typedef struct {
    /** Mode currently programmed into the CSI bridge. */
    uint32_t hres;
    uint32_t vres;
    size_t frame_bytes;
    void *fb[CAPTURE_FB_COUNT];
    void *volatile done_fb;
    volatile int ping_fb_idx;
    SemaphoreHandle_t csi_done_sem;
    tc358743_t *tc;
    /** Serialises TC358743 I2C between the monitor task and the capture task. */
    SemaphoreHandle_t tc_mu;
    volatile uint32_t csi_dma_done_irqs;
    volatile uint32_t csi_get_new_irqs;

    /** Set by the monitor task from SYS_STATUS; false means nothing to encode. */
    volatile bool signal_present;
    /**
     * Mode the monitor wants applied. The capture task performs the switch, so
     * the CSI receiver is never reconfigured underneath an in-flight encode.
     */
    volatile bool mode_change_pending;
    volatile uint32_t pending_hres;
    volatile uint32_t pending_vres;
} capture_ctx_t;

/**
 * LDO, I2C, TC358743, frame buffers, CSI, ISP bypass, then HDMI lock and esp_cam start.
 * Returns a pointer to internal storage; valid until the task exits, NULL when
 * no capture card answered.
 */
capture_ctx_t *capture_hw_init_start(void);

/**
 * Reprogram the CSI bridge for a new active size and restart the receiver.
 * Call from the capture task only. @p hres / @p vres must fit the buffers.
 */
esp_err_t capture_hw_apply_mode(capture_ctx_t *c, uint32_t hres, uint32_t vres);

/**
 * After HDMI loss (host sleep): stop CSI, HDMI HPD cycle, re-kick TC358743 MIPI, P4 bridge regs, esp_cam start.
 * Safe to call from the capture task when frames have stalled; throttled by the caller.
 */
esp_err_t capture_hw_hdmi_recover(capture_ctx_t *c);

/** Poll the bridge for signal state and resolution changes (200 ms cadence). */
void capture_monitor_start(capture_ctx_t *c);

/** Guard TC358743 I2C access. @return false on timeout. */
bool capture_tc_lock(capture_ctx_t *c, uint32_t timeout_ms);
void capture_tc_unlock(capture_ctx_t *c);

void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes);

/** CSI bits per pixel for debug logs (RGB888 -> 24 bpp BGR order in DRAM). */
unsigned capture_csi_bpp(void);

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp);

/*
 * A codec owns its engine and its output buffers, and publishes into the frame
 * store. Only one runs at a time: both the JPEG and the H.264 path want most of
 * the spare PSRAM at 1080p, and the source frames can only be consumed once.
 */
typedef struct {
    const char *name;
    video_payload_t payload;
    /** Claim the engine and the buffers, then install them in the store. */
    esp_err_t (*open)(void);
    /** Release everything. The store has been quiesced by the caller. */
    void (*close)(void);
    /**
     * Encode one captured frame and publish it.
     * @param force_publish  publish even if the result is unchanged, because
     *                       what the viewers hold is no longer valid
     */
    esp_err_t (*encode)(capture_ctx_t *c, const void *src, bool force_publish);
} capture_codec_t;

const capture_codec_t *capture_codec_mjpeg(void);
const capture_codec_t *capture_codec_h264(void);

/** Follow `jpg_quality` from the settings registry. Call once at start-up. */
void capture_mjpeg_bind_settings(void);

/** Capture, encode and publish, forever. Returns only when the pipeline dies. */
void capture_loop_run(capture_ctx_t *c);

/** Open the hardware H.264 encoder once to find out whether this chip has a
 *  working one, and record the answer in the capability registry. */
void capture_h264_probe(void);

/** Predicted H.264 frame rate ceiling at a given size, 0 before probing. */
uint32_t capture_h264_estimated_fps(uint32_t w, uint32_t h);

/* ---- telemetry, owned by capture.c ------------------------------------- */

void capture_status_set_mode(uint32_t hres, uint32_t vres, bool interlaced);
void capture_status_set_signal(bool present, uint8_t sys_status);
void capture_status_add_frame(size_t bytes);
/** An encoded frame identical to the last published one. */
void capture_status_add_skipped(void);
/** Time one encode took, in microseconds. */
void capture_status_add_encode_time(uint32_t us);
/** Recompute the rolling fps / bitrate window. Called by the monitor task. */
void capture_status_tick(void);
