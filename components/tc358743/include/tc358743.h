/*
 * SPDX-FileCopyrightText: 2026 Jonathan Rowny
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the p4kvm project: https://github.com/jrowny/p4kvm
 *
 * Toshiba TC358743 HDMI -> MIPI CSI-2 bridge
 * Register sequence from drivers/media/i2c/tc358743.c in the Linux kernel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 7-bit I2C address (0x1e >> 1). */
#define TC358743_I2C_ADDR 0x0f

typedef struct tc358743 tc358743_t;

/** Which mode list the bridge advertises to the source over DDC. */
typedef enum {
    TC358743_EDID_FULL = 0,    /**< 640x480 up to 1920x1080@30 */
    TC358743_EDID_1080P30 = 1, /**< single mode, for sources that reject the full list */
} tc358743_edid_profile_t;

/** Input timing as measured by the bridge, plus the link state that qualifies it. */
typedef struct {
    uint16_t hact;   /**< active pixels per line */
    uint16_t vact;   /**< active lines per frame (per field when interlaced) */
    uint16_t htotal; /**< including blanking */
    uint16_t vtotal; /**< including blanking */
    uint8_t sys_status;
    bool ddc5v;     /**< source is powering the DDC line, i.e. a cable is attached */
    bool tmds;      /**< TMDS clock detected */
    bool hdmi_mode; /**< HDMI rather than DVI signalling */
    bool sync;      /**< horizontal and vertical sync locked */
    bool interlaced;
} tc358743_timings_t;

typedef struct {
    uint32_t refclk_hz;
    uint16_t pll_prd;
    uint16_t pll_fbd;
    uint16_t fifo_level;
    uint32_t lineinitcnt;
    uint32_t lptxtimecnt;
    uint32_t tclk_headercnt;
    uint32_t tclk_trailcnt;
    uint32_t ths_headercnt;
    uint32_t twakeup;
    uint32_t tclk_postcnt;
    uint32_t ths_trailcnt;
    uint32_t hstxvregcnt;
    uint8_t ddc5v_mode;
    unsigned lanes;
    bool enable_hdcp;
    uint8_t hdmi_detection_delay;
    bool hdmi_phy_auto_reset_tmds_detected;
    bool hdmi_phy_auto_reset_tmds_in_range;
    bool hdmi_phy_auto_reset_tmds_valid;
    bool hdmi_phy_auto_reset_hsync_out_of_range;
    bool hdmi_phy_auto_reset_vsync_out_of_range;
} tc358743_cfg_t;

/**
 * Add TC358743 to an existing I2C master bus (bus must already be created).
 * @param bus I2C master bus handle
 * @param cfg timing / PLL parameters (NULL -> Waveshare/Pi-style 27 MHz / 972 Mb/s/lane defaults)
 */
esp_err_t tc358743_probe(i2c_master_bus_handle_t bus, const tc358743_cfg_t *cfg, tc358743_t **out_dev);

void tc358743_remove(tc358743_t *dev);

/**
 * Bring-up through PLL + CSI + interrupts, with HPD low (source not signalled yet).
 * Call after probe; then start the P4 CSI receiver; then tc358743_enable_hdmi_output().
 */
esp_err_t tc358743_init_streaming(tc358743_t *dev);

/**
 * Choose the advertised mode list. Must be called before
 * @ref tc358743_init_streaming, which is what writes EDID into the bridge.
 */
esp_err_t tc358743_set_edid_profile(tc358743_t *dev, tc358743_edid_profile_t profile);

/**
 * Select MIPI CSI-2 pixel packing after init_streaming (before HDMI HPD / cam start).
 * @param uyvy422 false: RGB888_1X24 (Linux MEDIA_BUS_FMT_RGB888_1X24). true: UYVY 16-bit (UYVY8_1X16).
 */
void tc358743_set_csi_uyvy422(tc358743_t *dev, bool uyvy422);

/**
 * Linux-style hotplug finish: enable_stream(true), ~150 ms, HPD high, then CSI_START pulse.
 * Call after esp_cam_ctlr_enable(); before esp_cam_ctlr_start() once HDMI can feed the bridge.
 */
esp_err_t tc358743_enable_hdmi_output(tc358743_t *dev);

/**
 * Simulate sink disconnect/reconnect: video off, HPD low, then @ref tc358743_enable_hdmi_output.
 * Use after the host drops HDMI (sleep) to re-train the source while CSI is stopped.
 */
esp_err_t tc358743_hdmi_hotplug_reset(tc358743_t *dev);

/**
 * Re-run CSI TX setup after HDMI/TMDS is stable (MIPI may stay idle if CSI was programmed too early).
 */
esp_err_t tc358743_reapply_csi_path_after_hdmi(tc358743_t *dev);

/**
 * Log bridge status: SYS, VI/CLK/PHYERR, measured H/V active & total (TC9590-class timing regs),
 * CSI_STATUS (Hlt/TxAct/WSync per datasheet), CSI_CONTROL, CSI_INT.
 */
void tc358743_debug_bridge(tc358743_t *dev);

/** CONFCTL / CSI_ERR / AVI dump when P4 CSI times out (call from capture stall path). */
void tc358743_debug_stall_extras(tc358743_t *dev);

/** Start/stop CSI output side (mute / FIFO enables per Linux enable_stream()). */
esp_err_t tc358743_set_streaming(tc358743_t *dev, bool on);

esp_err_t tc358743_read_chip_id(tc358743_t *dev, uint16_t *chip_id);

/** SYS_STATUS register (cable / TMDS / sync bits). */
esp_err_t tc358743_sys_status(tc358743_t *dev, uint8_t *out_st);

/**
 * Read what the bridge currently measures on its HDMI input.
 *
 * The active size is only meaningful once @ref tc358743_timings_t.tmds and
 * @ref tc358743_timings_t.sync are both set: with no signal the counters hold
 * whatever they last latched.
 */
esp_err_t tc358743_get_timings(tc358743_t *dev, tc358743_timings_t *out);

/** True when the measurement is usable: TMDS locked, sync locked, sane size. */
bool tc358743_timings_valid(const tc358743_timings_t *t);

/**
 * Read CEA-861 AVI InfoFrame colorimetry (PB1 bits 7..5: Y2:Y1:Y0).
 * @param out_y 0=RGB, 1=YCbCr422, 2=YCbCr444, 3=YCbCr420, ... (CEA-861).
 * @return ESP_OK on valid AVI type 0x82; ESP_ERR_NOT_FOUND if packet missing/short.
 */
esp_err_t tc358743_get_avi_color_format(tc358743_t *dev, uint8_t *out_y);

void tc358743_cfg_defaults_waveshare_pi(tc358743_cfg_t *c);

#ifdef __cplusplus
}
#endif
