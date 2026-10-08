/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "kvm_bridge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

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
    uint8_t input_hz;      /**< refresh rate the source sends, 0 when unknown */
    bool too_fast;         /**< the mode needs more than the CSI lanes carry */
    /**
     * How long the picture has been one flat colour, in ms; 0 when it is not.
     *
     * The text reader covers screens drawn from a character generator. This
     * covers the other kind of bad news - a Windows stop screen, a blanked
     * output, a desktop that died into its background - which has no grid to
     * read and is nearly all one colour. See capture_flat.c.
     */
    uint32_t flat_ms;
    /** The flat colour is black or nearly: a blanked output, not a stop screen. */
    bool flat_dark;
    /** Frames the CSI delivered since boot, whether or not anyone encoded them. */
    uint32_t frames;
    /** How long ago the last one landed, in ms; UINT32_MAX before the first. */
    uint32_t frame_age_ms;
    /** H.264 frames are rearranged on the CPU here (LT6911D below rev 3.0):
     *  fine at 720p, a few fps at 1080p. */
    bool h264_cpu;
} kvm_video_status_t;

/**
 * Something else is holding PSRAM it could give back - the recorder's ring of
 * frames is the one that matters. `failed` is true when a codec has just failed
 * to get a buffer and will try once more; false when H.264 has simply closed.
 */
typedef void (*capture_memory_pressure_cb_t)(bool failed);
void capture_set_memory_pressure_cb(capture_memory_pressure_cb_t cb);

/**
 * The other way round: give back what an idle codec keeps for a quick restart
 * (MJPEG's 4.8 MB of output buffers while H.264 runs). For the recorder.
 */
void capture_release_idle_buffers(void);

/** One PSRAM block this size must stay free for screenshots (the byte-reorder
 *  buffer on pre-3.0 YUV boards, when not already held); 0 when not needed. */
size_t capture_psram_keep_block(void);

/**
 * While H.264 runs, the part of the codec region it does not use can hold the
 * recorder's ring. NULL when there is none of at least @p min bytes. Give it
 * back from the memory-pressure callback; MJPEG cannot open until it is back.
 */
void *capture_arena_borrow(size_t min, size_t *len);
void capture_arena_give_back(void *p);

/**
 * Probe the codecs and build the H.264 encoder while internal RAM is still in
 * one piece. Call early in boot, before the network starts. capture_start()
 * does it itself when this was not called.
 */
void capture_reserve_early(void);

void capture_start(void);

void capture_status_get(kvm_video_status_t *out);

/** Raw form of the two counters above: frames since boot, esp_timer time of the last. */
void capture_frame_counter(uint32_t *frames, int64_t *last_us);

/**
 * Whether the bridge can see the source's +5 V, which tells a target that is
 * off from one that is on and sending nothing. The TC358743 can; the LT6911D
 * cannot, so on it "no signal" may just be a screen gone to sleep.
 */
bool capture_source_power_known(void);

/**
 * Offer the source a fresh start, asked for by a person: a hotplug cycle where
 * the bridge has one, otherwise a pulse on its reset pin. To the target either
 * looks like a monitor unplugged and plugged back in.
 */
esp_err_t capture_reconnect_source(void);

/**
 * The screen as a JPEG, on either codec. On success @p out is a PSRAM buffer the
 * caller frees. ESP_ERR_NOT_FOUND when there is no signal, ESP_ERR_TIMEOUT when
 * no frame came in @p timeout_ms (a device too hot to encode sends none).
 */
esp_err_t capture_snapshot_jpeg(uint8_t **out, size_t *out_len, uint32_t timeout_ms);

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

/** The HDMI bridge once capture has it running, NULL before (or with no chip). */
const kvm_bridge_t *capture_bridge(void);

#ifdef __cplusplus
}
#endif
