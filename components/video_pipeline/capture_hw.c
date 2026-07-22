/*
 * SPDX-FileCopyrightText: 2026 Jonathan Rowny
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the p4kvm project: https://github.com/jrowny/p4kvm
 */
#include "capture_priv.h"

#include <inttypes.h>

#include "kvm_board.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_SPIRAM
#error "Enable CONFIG_SPIRAM (PSRAM): 1080p frame buffers need external RAM. See sdkconfig.defaults."
#endif

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/isp_core.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_ldo_regulator.h"
#include "hal/mipi_csi_types.h"
#include "soc/clk_tree_defs.h"
#include "soc/isp_struct.h"
#include "soc/mipi_csi_bridge_struct.h"

#include "kvm_caps.h"
#include "kvm_settings.h"
#include "tc358743.h"
#include "tc358743_hdmi_debug.h"

static esp_cam_ctlr_handle_t s_cam;
static isp_proc_handle_t s_isp_bypass;

static const uint32_t s_csi_expected_dt = 0x24u;

static capture_ctx_t s_cap;

static void tc358743_resetn_pulse(void)
{
#if CONFIG_KVM_TC358743_RST_GPIO >= 0
    const int rst = CONFIG_KVM_TC358743_RST_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(CAPTURE_LOG_TAG, "TC358743 RESETN released on GPIO %d", rst);
#else
    ESP_LOGW(CAPTURE_LOG_TAG, "TC358743 RESETN not wired - waiting 500 ms for internal POR");
    vTaskDelay(pdMS_TO_TICKS(500));
#endif
}

static void wait_tc358743_pixel_stream(tc358743_t *tc, uint32_t timeout_ms)
{
    const uint32_t step = 50;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        uint8_t st = 0;
        if (tc358743_sys_status(tc, &st) == ESP_OK && (st & 0x02) != 0 && (st & 0x80) != 0) {
            ESP_LOGI(CAPTURE_LOG_TAG, "HDMI ready SYS_STATUS=0x%02x after %u ms", st, waited);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    ESP_LOGW(CAPTURE_LOG_TAG, "HDMI lock wait %u ms - starting CSI anyway", (unsigned)timeout_ms);
}

void capture_debug_csi_timeout(capture_ctx_t *c, unsigned bpp, size_t fb_bytes)
{
    const uint32_t gdma_64b = (uint32_t)(c->hres * c->vres * bpp / 64);
    uint32_t brg_fc = MIPI_CSI_BRIDGE.frame_cfg.val;
    uint32_t isp_fc = ISP.frame_cfg.val;
    unsigned brg_has_hsync = (unsigned)((brg_fc >> 24) & 1u);
    unsigned brg_vcheck = (unsigned)((brg_fc >> 25) & 1u);
    unsigned isp_ls = (unsigned)((isp_fc >> 29) & 1u);
    unsigned isp_le = (unsigned)((isp_fc >> 30) & 1u);
    ESP_LOGW(CAPTURE_LOG_TAG,
             "CSI stall: fb=%zu B expect, GDMA size=%" PRIu32 "x64b for %ux%u@%ubpp | get_new=%" PRIu32 " done=%" PRIu32
             " ping=%d done_fb=%p",
             fb_bytes, gdma_64b, (unsigned)c->hres, (unsigned)c->vres, bpp, c->csi_get_new_irqs,
             c->csi_dma_done_irqs, c->ping_fb_idx, (void *)c->done_fb);
    ESP_LOGW(CAPTURE_LOG_TAG, "  esp_cam: csi_transfer_size=%" PRIu32 "x64b (=hxvxin_bpp/64); RGB888 in_bpp=24, wire datatype must match", gdma_64b);
    {
        uint32_t dtc = MIPI_CSI_BRIDGE.data_type_cfg.val;
        unsigned lo = (unsigned)(dtc & 0x3fu);
        unsigned hi = (unsigned)((dtc >> 8) & 0x3fu);
        ESP_LOGW(CAPTURE_LOG_TAG, "  BRG data_type filter min=0x%02x max=0x%02x (CSI-2 user: RGB888=0x24 YUV422_8b=0x1E ...)", lo, hi);
    }
    ESP_LOGW(CAPTURE_LOG_TAG, "  BRG frame_cfg=0x%08" PRIx32 " (vadr=%" PRIu32 " hadr=%" PRIu32 ") host_ctrl=0x%08" PRIx32
             " dtype_reg=0x%08" PRIx32,
             brg_fc, brg_fc & 0xfffu, (brg_fc >> 12) & 0xfffu, MIPI_CSI_BRIDGE.host_ctrl.val, MIPI_CSI_BRIDGE.data_type_cfg.val);
    ESP_LOGW(CAPTURE_LOG_TAG, "  BRG line packets: has_hsync=%u vadr_check=%u | ISP line packets: start=%u end=%u"
                            " (bridge counts exact rows/pixels, ISP counts row-1/pixel-1)",
             brg_has_hsync, brg_vcheck, isp_ls, isp_le);
    ESP_LOGW(CAPTURE_LOG_TAG, "  BRG csi_en=0x%08" PRIx32 " buf_flow=0x%08" PRIx32, MIPI_CSI_BRIDGE.csi_en.val,
             MIPI_CSI_BRIDGE.buf_flow_ctl.val);
    ESP_LOGW(CAPTURE_LOG_TAG, "  ISP frame_cfg=0x%08" PRIx32 " (vadr=%" PRIu32 " hadr=%" PRIu32 ") cntl=0x%08" PRIx32, isp_fc,
             isp_fc & 0xfffu, (isp_fc >> 12) & 0xfffu, ISP.cntl.val);
    {
        uint32_t ir = MIPI_CSI_BRIDGE.int_raw.val;
        uint32_t ist = MIPI_CSI_BRIDGE.int_st.val;
        uint32_t iena = MIPI_CSI_BRIDGE.int_ena.val;
        uint32_t m = ist & 0x3fu;
        ESP_LOGW(CAPTURE_LOG_TAG, "  BRG int raw=0x%08" PRIx32 " st=0x%08" PRIx32 " ena=0x%08" PRIx32
                                 " | st: vadr_gt:%u vadr_lt:%u discard:%u overrun:%u fifo_ovf:%u dma_upd:%u",
                 ir, ist, iena, (unsigned)(m >> 0) & 1u, (unsigned)(m >> 1) & 1u, (unsigned)(m >> 2) & 1u, (unsigned)(m >> 3) & 1u,
                 (unsigned)(m >> 4) & 1u, (unsigned)(m >> 5) & 1u);
    }
    {
        uint32_t bfc = MIPI_CSI_BRIDGE.buf_flow_ctl.val;
        uint32_t drc = MIPI_CSI_BRIDGE.dma_req_cfg.val;
        ESP_LOGW(
            CAPTURE_LOG_TAG, "  BRG endian=0x%08" PRIx32 " dmablk_size=%" PRIu32 " dma_burst_len=%" PRIu32
                            " dma_upd_by_blk:%u buf_depth=%" PRIu32 " afull_th=%" PRIu32 " dma_interval=%" PRIu32,
            MIPI_CSI_BRIDGE.endian_mode.val, (uint32_t)MIPI_CSI_BRIDGE.dmablk_size.dmablk_size, drc & 0xfffu,
            (unsigned)((drc >> 12) & 1u), (bfc >> 16) & 0x3fffu, bfc & 0x3fffu,
            (uint32_t)MIPI_CSI_BRIDGE.dma_req_interval.dma_req_interval);
    }
}

unsigned capture_csi_bpp(void)
{
    return 24u;
}

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp)
{
    csi->input_data_color_type = CAM_CTLR_COLOR_RGB888;
    csi->output_data_color_type = CAM_CTLR_COLOR_RGB888;
    isp->input_data_color_type = ISP_COLOR_RGB888;
    isp->output_data_color_type = ISP_COLOR_RGB888;
}

static void capture_configure_p4_csi_bridge(uint32_t hres, uint32_t vres)
{
    MIPI_CSI_BRIDGE.frame_cfg.hadr_num = hres;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num = vres;
    MIPI_CSI_BRIDGE.frame_cfg.has_hsync_e = 0u;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num_check = 0u;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = s_csi_expected_dt;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = s_csi_expected_dt;
    MIPI_CSI_BRIDGE.int_clr.val = 0x3fu;
}

static bool IRAM_ATTR cam_on_get_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)ud;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_get_new_irqs, 1);
    int i = c->ping_fb_idx % CAPTURE_FB_COUNT;
    trans->buffer = c->fb[i];
    trans->buflen = c->frame_bytes;
    c->ping_fb_idx = (c->ping_fb_idx + 1) % CAPTURE_FB_COUNT;
    return false;
}

static bool IRAM_ATTR cam_on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_dma_done_irqs, 1);
    if (trans && trans->buffer) {
        c->done_fb = trans->buffer;
    }
    BaseType_t high_task_woken = pdFALSE;
    if (c->csi_done_sem) {
        (void)xSemaphoreGiveFromISR(c->csi_done_sem, &high_task_woken);
    }
    return high_task_woken;
}

/*
 * esp_cam_ctlr derives its GDMA transfer length from h_res * v_res * bpp at
 * creation time, so a resolution change means building a new controller - only
 * rewriting the bridge registers would leave the DMA expecting the old frame
 * size and every capture would time out.
 */
static esp_err_t csi_create(capture_ctx_t *c, uint32_t hres, uint32_t vres)
{
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = hres,
        .v_res = vres,
        .data_lane_num = 2,
        .lane_bit_rate_mbps = KVM_BOARD_MIPI_LANE_MBPS,
        .queue_items = CAPTURE_FB_COUNT,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
    };
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80 * 1000000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .yuv_range = ISP_COLOR_RANGE_LIMIT,
        .yuv_std = ISP_YUV_CONV_STD_BT709,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = hres,
        .v_res = vres,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
        .intr_priority = 0,
        .flags = {.bypass_isp = true, .byte_swap_en = false},
    };
    capture_fill_esp_cam_color_types(&csi_cfg, &isp_cfg);

    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &s_cam), CAPTURE_LOG_TAG, "csi ctlr");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = cam_on_get_new,
        .on_trans_finished = cam_on_done,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, c), CAPTURE_LOG_TAG, "cbs");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_cam), CAPTURE_LOG_TAG, "cam enable");
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_cfg, &s_isp_bypass), CAPTURE_LOG_TAG, "isp");
    ISP.cntl.isp_en = 0;
    capture_configure_p4_csi_bridge(hres, vres);
    return ESP_OK;
}

static void csi_destroy(void)
{
    if (s_cam) {
        (void)esp_cam_ctlr_stop(s_cam);
        (void)esp_cam_ctlr_disable(s_cam);
        (void)esp_cam_ctlr_del(s_cam);
        s_cam = NULL;
    }
    if (s_isp_bypass) {
        (void)esp_isp_del_processor(s_isp_bypass);
        s_isp_bypass = NULL;
    }
}

bool capture_tc_lock(capture_ctx_t *c, uint32_t timeout_ms)
{
    if (!c || !c->tc_mu) {
        return false;
    }
    return xSemaphoreTake(c->tc_mu, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void capture_tc_unlock(capture_ctx_t *c)
{
    if (c && c->tc_mu) {
        xSemaphoreGive(c->tc_mu);
    }
}

capture_ctx_t *capture_hw_init_start(void)
{
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = KVM_BOARD_MIPI_LDO_CHAN_ID,
        .voltage_mv = KVM_BOARD_MIPI_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    tc358743_resetn_pulse();

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = KVM_BOARD_TC358743_I2C_SDA_GPIO,
        .scl_io_num = KVM_BOARD_TC358743_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));

    /* A missing capture card must not take the whole device down: without it the
     * KVM still serves HID, and the web UI explains what is wrong. */
    esp_err_t probe_err = tc358743_probe(i2c_bus, NULL, &s_cap.tc);
    if (probe_err != ESP_OK) {
        kvm_cap_report(KVM_CAP_VIDEO, false, "TC358743 not responding on I2C (%s)",
                       esp_err_to_name(probe_err));
        ESP_LOGE(CAPTURE_LOG_TAG, "TC358743 probe failed: %s", esp_err_to_name(probe_err));
        return NULL;
    }
    /* "custom" is not implemented yet and falls back to the full list. */
    const int32_t edid_choice = kvm_setting_int("edid_prof");
    (void)tc358743_set_edid_profile(s_cap.tc, edid_choice == 1 ? TC358743_EDID_1080P30
                                                              : TC358743_EDID_FULL);

    probe_err = tc358743_init_streaming(s_cap.tc);
    if (probe_err != ESP_OK) {
        kvm_cap_report(KVM_CAP_VIDEO, false, "TC358743 init failed (%s)", esp_err_to_name(probe_err));
        ESP_LOGE(CAPTURE_LOG_TAG, "TC358743 init failed: %s", esp_err_to_name(probe_err));
        return NULL;
    }

    s_cap.tc_mu = xSemaphoreCreateMutex();
    if (!s_cap.tc_mu) {
        ESP_LOGE(CAPTURE_LOG_TAG, "TC358743 mutex");
        return NULL;
    }

    size_t align = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align));
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *blk = heap_caps_aligned_calloc(align, CAPTURE_FB_COUNT, CAPTURE_MAX_FRAME_BYTES, caps);
    if (!blk) {
        ESP_LOGE(CAPTURE_LOG_TAG, "CSI frame buffer alloc failed (%ux%zu bytes) - PSRAM exhausted",
                 CAPTURE_FB_COUNT, CAPTURE_MAX_FRAME_BYTES);
        kvm_cap_report(KVM_CAP_VIDEO, false, "no PSRAM for %ux%u frame buffers", CAPTURE_MAX_H_RES,
                       CAPTURE_MAX_V_RES);
        return NULL;
    }
    for (int i = 0; i < CAPTURE_FB_COUNT; i++) {
        s_cap.fb[i] = blk + ((size_t)i * CAPTURE_MAX_FRAME_BYTES);
    }
    s_cap.ping_fb_idx = 0;
    s_cap.done_fb = NULL;
    s_cap.csi_dma_done_irqs = 0;
    s_cap.csi_get_new_irqs = 0;

    ESP_LOGI(CAPTURE_LOG_TAG, "CSI 24bpp BGR ring %ux%zu bytes for up to %ux%u (align %zu)",
             CAPTURE_FB_COUNT, CAPTURE_MAX_FRAME_BYTES, CAPTURE_MAX_H_RES, CAPTURE_MAX_V_RES, align);

    s_cap.csi_done_sem = xSemaphoreCreateCounting(32, 0);
    if (!s_cap.csi_done_sem) {
        ESP_LOGE(CAPTURE_LOG_TAG, "CSI done sem");
        return NULL;
    }

    ESP_ERROR_CHECK(tc358743_enable_hdmi_output(s_cap.tc));
    wait_tc358743_pixel_stream(s_cap.tc, 5000);

    /* Start in whatever mode the source is actually sending. A machine that is
     * still in its firmware screens will not be at 1080p. */
    uint32_t hres = CAPTURE_MAX_H_RES;
    uint32_t vres = CAPTURE_MAX_V_RES;
    tc358743_timings_t t = {0};
    if (tc358743_get_timings(s_cap.tc, &t) == ESP_OK && tc358743_timings_valid(&t)) {
        hres = t.hact;
        vres = t.vact;
        s_cap.signal_present = true;
        capture_status_set_signal(true, t.sys_status);
        ESP_LOGI(CAPTURE_LOG_TAG, "input %ux%u%s (htotal %u vtotal %u)", t.hact, t.vact,
                 t.interlaced ? "i" : "p", t.htotal, t.vtotal);
    } else {
        ESP_LOGW(CAPTURE_LOG_TAG, "no HDMI signal yet - starting at %ux%u", hres, vres);
    }

    esp_err_t er = csi_create(&s_cap, hres, vres);
    if (er != ESP_OK) {
        kvm_cap_report(KVM_CAP_VIDEO, false, "CSI receiver setup failed (%s)", esp_err_to_name(er));
        return NULL;
    }
    s_cap.hres = hres;
    s_cap.vres = vres;
    s_cap.frame_bytes = (size_t)hres * (size_t)vres * 3u;
    capture_status_set_mode(hres, vres, t.interlaced);

    ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam));
    ESP_LOGI(CAPTURE_LOG_TAG, "capture running at %ux%u", hres, vres);

    kvm_cap_report(KVM_CAP_VIDEO, true, NULL);
    return &s_cap;
}

esp_err_t capture_hw_apply_mode(capture_ctx_t *c, uint32_t hres, uint32_t vres)
{
    ESP_RETURN_ON_FALSE(c, ESP_ERR_INVALID_ARG, CAPTURE_LOG_TAG, "ctx");
    ESP_RETURN_ON_FALSE(hres >= 320u && vres >= 200u && hres <= CAPTURE_MAX_H_RES &&
                            vres <= CAPTURE_MAX_V_RES,
                        ESP_ERR_INVALID_ARG, CAPTURE_LOG_TAG, "mode %ux%u out of range", hres, vres);

    const bool same_mode = (hres == c->hres && vres == c->vres);
    ESP_LOGI(CAPTURE_LOG_TAG, "%s %ux%u", same_mode ? "restarting capture at" : "switching to", hres,
             vres);

    csi_destroy();

    while (xSemaphoreTake(c->csi_done_sem, 0) == pdTRUE) {
        /* Completions from the previous mode describe frames of the wrong size. */
    }
    c->ping_fb_idx = 0;
    c->done_fb = NULL;
    c->csi_dma_done_irqs = 0;
    c->csi_get_new_irqs = 0;
    c->hres = hres;
    c->vres = vres;
    c->frame_bytes = (size_t)hres * (size_t)vres * 3u;

    esp_err_t er = csi_create(c, hres, vres);
    if (er != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "csi_create for %ux%u: %s", hres, vres, esp_err_to_name(er));
        return er;
    }
    er = esp_cam_ctlr_start(s_cam);
    if (er != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "esp_cam_ctlr_start: %s", esp_err_to_name(er));
        return er;
    }
    capture_status_set_mode(hres, vres, false);
    return ESP_OK;
}

static void capture_drain_csi_done_sem(SemaphoreHandle_t sem)
{
    if (!sem) {
        return;
    }
    while (xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

esp_err_t capture_hw_hdmi_recover(capture_ctx_t *c)
{
    ESP_RETURN_ON_FALSE(c && c->tc && c->csi_done_sem, ESP_ERR_INVALID_ARG, CAPTURE_LOG_TAG, "ctx");

    ESP_LOGW(CAPTURE_LOG_TAG, "recovering: CSI teardown -> HDMI hotplug -> MIPI reapply -> restart");

    csi_destroy();
    capture_drain_csi_done_sem(c->csi_done_sem);

    if (!capture_tc_lock(c, 2000)) {
        ESP_LOGW(CAPTURE_LOG_TAG, "recover: TC358743 busy");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t er = tc358743_hdmi_hotplug_reset(c->tc);
    if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "tc358743_hdmi_hotplug_reset: %s", esp_err_to_name(er));
    }
    wait_tc358743_pixel_stream(c->tc, 5000);
    er = tc358743_reapply_csi_path_after_hdmi(c->tc);
    if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "tc358743_reapply_csi_path_after_hdmi: %s", esp_err_to_name(er));
    }

    /* The source may well have come back in a different mode than it left in. */
    uint32_t hres = c->hres;
    uint32_t vres = c->vres;
    tc358743_timings_t t = {0};
    if (tc358743_get_timings(c->tc, &t) == ESP_OK && tc358743_timings_valid(&t)) {
        hres = t.hact;
        vres = t.vact;
    }
    capture_tc_unlock(c);

    c->ping_fb_idx = 0;
    c->done_fb = NULL;
    c->csi_dma_done_irqs = 0;
    c->csi_get_new_irqs = 0;
    c->hres = hres;
    c->vres = vres;
    c->frame_bytes = (size_t)hres * (size_t)vres * 3u;

    er = csi_create(c, hres, vres);
    if (er != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "csi_create after recover: %s", esp_err_to_name(er));
        return er;
    }
    er = esp_cam_ctlr_start(s_cam);
    if (er != ESP_OK) {
        ESP_LOGE(CAPTURE_LOG_TAG, "esp_cam_ctlr_start after recover: %s", esp_err_to_name(er));
        return er;
    }
    capture_status_set_mode(hres, vres, t.interlaced);
    ESP_LOGI(CAPTURE_LOG_TAG, "recovered at %ux%u", hres, vres);
    return ESP_OK;
}

/*
 * Polls the bridge rather than using its interrupt line: the INT pin is not
 * wired on this adapter, and 200 ms is fast enough that a mode switch is
 * invisible next to the source's own retraining time.
 */
static void capture_monitor_task(void *arg)
{
    capture_ctx_t *c = (capture_ctx_t *)arg;
    uint32_t candidate_h = 0;
    uint32_t candidate_v = 0;
    int candidate_hits = 0;
    bool had_signal = c->signal_present;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        capture_status_tick();

        tc358743_timings_t t = {0};
        if (!capture_tc_lock(c, 100)) {
            continue;
        }
        esp_err_t er = tc358743_get_timings(c->tc, &t);
        capture_tc_unlock(c);
        if (er != ESP_OK) {
            continue;
        }

        const bool valid = tc358743_timings_valid(&t);
        c->signal_present = valid;
        capture_status_set_signal(valid, t.sys_status);

        if (!valid) {
            candidate_hits = 0;
            if (had_signal) {
                ESP_LOGW(CAPTURE_LOG_TAG, "HDMI signal lost (SYS_STATUS=0x%02x)", t.sys_status);
                had_signal = false;
            }
            continue;
        }

        /* Require the same reading twice: the counters are latched live and read
         * as nonsense for a few milliseconds while a source changes mode. */
        if (t.hact == candidate_h && t.vact == candidate_v) {
            if (candidate_hits < 3) {
                candidate_hits++;
            }
        } else {
            candidate_h = t.hact;
            candidate_v = t.vact;
            candidate_hits = 1;
            continue;
        }
        if (candidate_hits < 2) {
            continue;
        }

        const bool mode_differs = (candidate_h != c->hres || candidate_v != c->vres);
        if (!had_signal) {
            ESP_LOGI(CAPTURE_LOG_TAG, "HDMI signal back: %ux%u%s", candidate_h, candidate_v,
                     t.interlaced ? "i" : "p");
            had_signal = true;
            /* Restart the receiver even at an unchanged size: while the source
             * was away the CSI side stopped delivering. */
            c->pending_hres = candidate_h;
            c->pending_vres = candidate_v;
            c->mode_change_pending = true;
            xSemaphoreGive(c->csi_done_sem);
        } else if (mode_differs && !c->mode_change_pending) {
            ESP_LOGI(CAPTURE_LOG_TAG, "input mode %ux%u -> %ux%u%s", c->hres, c->vres, candidate_h,
                     candidate_v, t.interlaced ? "i" : "p");
            c->pending_hres = candidate_h;
            c->pending_vres = candidate_v;
            c->mode_change_pending = true;
            xSemaphoreGive(c->csi_done_sem);
        }
    }
}

void capture_monitor_start(capture_ctx_t *c)
{
    if (!c) {
        return;
    }
    /* Below the capture task: telemetry must never delay an encode. */
    xTaskCreatePinnedToCore(capture_monitor_task, "cam_mon", 4096, c, 4, NULL, 0);
}