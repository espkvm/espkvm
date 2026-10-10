/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "capture_pixfmt.h"
#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "kvm_bridge.h"
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
 * one to fill, so it stalls (drops to drop_fb) and the encoder has to
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
#elif CAPTURE_YUV_SWAP
/* An LT6911D sends packed YUV422 whatever the revision, so the ring is two
 * bytes a pixel here as well - sizing it for RGB888 would hold 4 MB of
 * contiguous PSRAM that this board does not have to spare, and it showed:
 * the JPEG encoder could not get its own buffers. No padding, because the
 * rearrangement for H.264 writes into a buffer of its own. */
#define CAPTURE_MAX_V_ALLOC CAPTURE_MAX_V_RES
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
    /** Where the DMA writes a frame that is being dropped. Ours, allocated once:
     *  the driver's own backup buffer is 6 MB freed and asked for again on every
     *  capture restart, and fragmented PSRAM stops handing it out. */
    void *drop_fb;
    void *volatile done_fb;
    volatile int ping_fb_idx;
    /*
     * Keep the free-running CSI DMA off the frame being encoded. The receiver
     * ping-pongs continuously and does not know a codec is still reading a buffer,
     * so at high resolution (encode >> one frame period) it would overwrite the
     * frame mid-read and tear it. The producer therefore only ever writes a buffer
     * that is none of: the one it is already filling, the newest completed one a
     * consumer may be about to take, or the one a consumer holds. When none is
     * free it writes drop_fb and that frame is simply dropped -
     * exactly the "keep the latest, skip the rest" behaviour we want under load.
     * fb_lock guards the three indices; it is taken from the DMA ISR and the loop.
     */
    portMUX_TYPE fb_lock;
    volatile int write_fb_idx; /* buffer the DMA is filling now, -1 = drop_fb */
    volatile int ready_fb_idx; /* newest completed buffer, -1 = none yet */
    volatile int held_fb_idx;  /* buffer the encode is reading, -1 = none */
    SemaphoreHandle_t csi_done_sem;
    kvm_bridge_t bridge;
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
    /** The input mode needs more than the CSI lanes carry: no frames will come. */
    volatile bool mode_too_fast;
} capture_ctx_t;

/**
 * LDO, I2C, TC358743, frame buffers, CSI, ISP bypass, then HDMI lock and esp_cam start.
 * Returns a pointer to internal storage; valid until the task exits, NULL when
 * no capture card answered.
 */
capture_ctx_t *capture_hw_init_start(void);

/** Stop the CSI receiver so its DMA is idle. See capture_hw.c. */
void capture_hw_quiesce(void);

/** Around each frame's DMA work (capture loop, H.264 encoder task). begin
 *  returns false once a restart is parking the pipeline; skip the frame then. */
bool capture_park_frame_begin(void);
void capture_park_frame_end(void);

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

/* A screenshot asked for while H.264 runs is encoded from this frame. Capture
 * task only; costs nothing unless one is waiting. See capture_snapshot.c. */
void capture_snapshot_tick(capture_ctx_t *c, const void *frame);

/** Is a still picture waiting to be taken from the next frame? */
bool capture_snapshot_wanted(void);

/** Forget it, the way capture_screentext_forget() does when the signal goes. */
void capture_flat_forget(void);

/** How long the screen has been one colour, in ms; 0 when it is not. */
uint32_t capture_flat_ms(void);

/** Ask whoever holds spare PSRAM to give it back. False when nobody listens. */
bool capture_release_memory(bool failed);

/** Whether the flat colour is black or nearly. Meaningful while flat. */
bool capture_flat_dark(void);

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
/** Frames arrive again: the next recovery starts its count from one. */
void capture_hw_frames_flowing(void);
/** Let the next frame through the CSI bridge (M5Stack: it is closed between frames). */
void capture_hw_frame_gate_open(void);
/** esp_timer time ring slot @p idx last landed from the bridge. */
int64_t capture_hw_fb_landed_us(int idx);

/** Poll the bridge for signal state and resolution changes (200 ms cadence). */
void capture_monitor_start(capture_ctx_t *c);

/** Guard TC358743 I2C access. @return false on timeout. */
bool capture_tc_lock(capture_ctx_t *c, uint32_t timeout_ms);
void capture_tc_unlock(capture_ctx_t *c);

/** Give back the MJPEG output buffers, which outlive a close so the codec that
 *  always works can always restart. Ignored while MJPEG is running. Every board
 *  has this one: the H.264 path asks for the memory back on any of them. */
void capture_mjpeg_release_buffers(void);

#if CAPTURE_YUV_SWAP
/** Reorder a captured YUV422 frame for the JPEG engine; NULL if the pass could
 *  not run, because the captured order would encode as a green picture. */
void *capture_yuv_swap(capture_ctx_t *c, void *src);

/** Claim the reordering buffer up front, before the encoders take the PSRAM. */
esp_err_t capture_yuv_swap_reserve(size_t max_frame_bytes);

/** Give the buffer back; H.264 does not need it. MJPEG takes it again on open. */
void capture_yuv_swap_release(void);

/** True while the buffer is allocated at full size. */
bool capture_yuv_swap_held(void);
#endif

/* ---- the codec region (capture_arena.c) ---- */
size_t capture_arena_round(size_t bytes);
/** Take the region; at boot, before the capture buffers. Codecs fall back to
 *  their own allocations when this failed. */
esp_err_t capture_arena_init(void);
bool capture_arena_active(void);
/** The reorder buffer's permanent place, or NULL. */
uint8_t *capture_arena_swap(size_t *len);
/** The codec part from offset 0, for a codec that is opening; NULL when the
 *  recorder still has the tail or there is no region. */
uint8_t *capture_arena_claim(size_t bytes);
/** What each codec carves out of the region. */
size_t capture_mjpeg_arena_bytes(void);
size_t capture_h264_arena_bytes(void);
#if CAPTURE_YUV_SWAP

/** Bytes of the largest captured frame, which is what the buffer is sized for. */
size_t capture_yuv_swap_max_bytes(void);

/** Rearrange a captured YUV422 frame into the packed YUV420 the pre-3.0 H.264
 *  encoder takes. @p pad_w is the encoder picture's macroblock-aligned width. */
void capture_yuv422_to_h264(const uint8_t *src, uint8_t *dst, uint32_t hres, uint32_t vres,
                            uint32_t pad_w);
#endif

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

/** Build the H.264 encoder for @p w x @p h and park it, whatever codec runs.
 *  Call from the capture task before the first codec opens. */
void capture_h264_reserve(uint32_t w, uint32_t h);

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
/** Measured input refresh (0 unknown), and whether the mode is over the lane limit. */
void capture_status_set_input(uint8_t hz, bool too_fast);
void capture_status_add_frame(size_t bytes);
/** A frame taken by the loop @p us after it landed. */
void capture_status_add_wait(uint32_t us);
/** Share of the configured quality to use now, 25..100 (see capture.c). */
uint32_t capture_link_pct(void);
/** An encoded frame identical to the last published one. */
void capture_status_add_skipped(void);
/** Time one encode took, in microseconds (the encoder alone, not any colour
 *  conversion before it). */
void capture_status_add_encode_time(uint32_t us);
/** Time one PPA colour conversion took, in microseconds (H.264 path only). */
void capture_status_add_ppa_time(uint32_t us);
/** Recompute the rolling fps / bitrate window. Called by the monitor task. */
void capture_status_tick(void);
