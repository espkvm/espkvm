/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "capture_pixfmt.h"
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

/*
 * Frame-buffer ring depth. With a synchronous encode the encoder holds one
 * buffer for the whole encode; two buffers then leave the free-running CSI just
 * one to fill, so it stalls (drops to the backup buffer) and the encoder has to
 * wait a full source period for a fresh frame - measured ~24 ms idle per frame
 * at 1080p. A third buffer keeps the CSI running so a just-completed frame is
 * always ready the instant the encoder finishes, making the encode time the true
 * period. The direct-encode board captures the smaller YUV422 frames, so the
 * third buffer fits; rev < 3.0 (RGB888 + a separate encode task that already
 * overlaps) keeps two.
 */
#if CAPTURE_DIRECT_ENCODE
#define CAPTURE_FB_COUNT 3
#else
#define CAPTURE_FB_COUNT 2
#endif

/*
 * Frame buffers are allocated once for the largest mode the bridge can deliver;
 * smaller modes use the leading part of the same allocation. Reallocating on
 * every resolution change would fragment PSRAM and can fail exactly when a
 * machine switches from its BIOS mode to the desktop.
 */
#define CAPTURE_MAX_H_RES 1920u
#define CAPTURE_MAX_V_RES 1080u
#if CAPTURE_DIRECT_ENCODE
/* On the direct-encode board the encoder reads the frame at macroblock-aligned
 * height, so pad the allocation up to the next multiple of 16 rows (1080 -> 1088).
 * Gated on the same rev macro as the pixel format, so the rev < 3.0 build's buffer
 * size is byte-for-byte unchanged. See capture_pixfmt.h. */
#define CAPTURE_MAX_V_ALLOC (((CAPTURE_MAX_V_RES + 15u) / 16u) * 16u)
/* rev >= 3.0 captures packed YUV422 at 2 bytes/px. */
#define CAPTURE_MAX_PIXEL_BYTES 2u
#else
#define CAPTURE_MAX_V_ALLOC CAPTURE_MAX_V_RES
/* rev < 3.0 captures RGB888 at 3 bytes/px. */
#define CAPTURE_MAX_PIXEL_BYTES 3u
#endif
#define CAPTURE_MAX_FRAME_BYTES \
    ((size_t)CAPTURE_MAX_H_RES * (size_t)CAPTURE_MAX_V_ALLOC * (size_t)CAPTURE_MAX_PIXEL_BYTES)

/** Shared CSI / ISP / HDMI state for codec tasks (lives in capture_hw.c). */
typedef struct {
    /** Mode currently programmed into the CSI bridge. */
    uint32_t hres;
    uint32_t vres;
    size_t frame_bytes;
    void *fb[CAPTURE_FB_COUNT];
    void *volatile done_fb;
    volatile int ping_fb_idx;
    /*
     * Keep the free-running CSI DMA off the frame being encoded. The receiver
     * ping-pongs continuously and does not know a codec is still reading a buffer,
     * so at high resolution (encode >> one frame period) it would overwrite the
     * frame mid-read and tear it. The producer therefore only ever writes a buffer
     * that is none of: the one it is already filling, the newest completed one a
     * consumer may be about to take, or the one a consumer holds. When none is
     * free it writes the driver's backup buffer and that frame is simply dropped -
     * exactly the "keep the latest, skip the rest" behaviour we want under load.
     * fb_lock guards the three indices; it is taken from the DMA ISR and the loop.
     */
    portMUX_TYPE fb_lock;
    volatile int write_fb_idx; /* buffer the DMA is filling now, -1 = backup */
    volatile int ready_fb_idx; /* newest completed buffer, -1 = none yet */
    volatile int held_fb_idx;  /* buffer the encode is reading, -1 = none */
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

/*
 * Offer the frame about to be encoded to the text-screen reader. Cheap and
 * silent unless the mode is a character grid and the picture has settled; see
 * capture_screentext.c. Capture task only.
 */
void capture_screentext_tick(capture_ctx_t *c, const void *frame);

/* Drop what was read: the picture it came from is gone (signal lost). */
void capture_screentext_forget(void);

/*
 * Watching for a screen that has become one flat colour - a Windows stop
 * screen, a blanked output, a desktop that died into its background. Cheap
 * enough to run on every frame in every mode, which is the point: the text
 * reader cannot see any of those. See capture_flat.c.
 */
void capture_flat_tick(capture_ctx_t *c, const void *frame);

/** Forget it, the way capture_screentext_forget() does when the signal goes. */
void capture_flat_forget(void);

/** How long the screen has been one colour, in ms; 0 when it is not. */
uint32_t capture_flat_ms(void);

/*
 * Read the screen with no viewer connected, for the watch. Cheap and silent
 * unless the operator asked for a watch and the target is showing text.
 * Capture task only.
 */
void capture_screentext_idle(capture_ctx_t *c);

/* Subscribe to the settings the watch depends on. Call once, before capture. */
void capture_screentext_init(void);

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

#if CONFIG_KVM_TC358743_ADV_DEBUG
void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes);
#endif

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

/** Report the MJPEG capability at start-up (opens and frees the JPEG engine). */
void capture_mjpeg_probe(void);
const capture_codec_t *capture_codec_h264(void);

/** True once the H.264 encoder has failed to build and retrying cannot help
 *  (it could not get its memory). The capture loop then falls back to MJPEG. */
bool capture_h264_encoder_failed(void);

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
/** Time one encode took, in microseconds (the encoder alone, not any colour
 *  conversion before it). */
void capture_status_add_encode_time(uint32_t us);
/** Time one PPA colour conversion took, in microseconds (H.264 path only). */
void capture_status_add_ppa_time(uint32_t us);
/** Recompute the rolling fps / bitrate window. Called by the monitor task. */
void capture_status_tick(void);
