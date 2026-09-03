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
#include "esp_timer.h"
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
#include "kvm_bridge.h"

static esp_cam_ctlr_handle_t s_cam;
static isp_proc_handle_t s_isp_bypass;

static capture_ctx_t s_cap;
static i2c_master_bus_handle_t s_i2c_bus; /* shared with an optional status OLED */

/*
 * Make the I2C bus, once, whoever asks first.
 *
 * It used to be created in the middle of bringing the capture chip up, which
 * meant the status OLED - which shares this bus and has no pins of its own -
 * could not attach until capture had started, some fourteen seconds into a
 * boot. Everything the panel exists to show before that was therefore invisible
 * on an OLED: most of all the reset-button window, which closes at nine seconds.
 *
 * Nothing about the bus itself needs the capture chip: it is two pins and a
 * peripheral. So it is made on demand and the capture path takes the same
 * handle when it gets there.
 */
esp_err_t capture_i2c_bus_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = KVM_BOARD_TC358743_I2C_SDA_GPIO,
        .scl_io_num = KVM_BOARD_TC358743_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1},
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

i2c_master_bus_handle_t capture_i2c_bus(void)
{
    return s_i2c_bus;
}

static void bridge_resetn_pulse(void)
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
    ESP_LOGI(CAPTURE_LOG_TAG, "capture board RESETN released on GPIO %d", rst);
#else
    ESP_LOGW(CAPTURE_LOG_TAG, "capture RESETN not wired - waiting 500 ms for internal POR");
    vTaskDelay(pdMS_TO_TICKS(500));
#endif
}

static void wait_bridge_pixel_stream(const kvm_bridge_t *b, uint32_t timeout_ms)
{
    const uint32_t step = 50;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        /* Read the decoded flags rather than picking bits out of the status
         * byte: which bit means what is the chip's business, not ours. */
        kvm_bridge_timings_t t = {0};
        if (kvm_bridge_get_timings(b, &t) == ESP_OK && t.tmds && t.sync) {
            ESP_LOGI(CAPTURE_LOG_TAG, "HDMI ready SYS_STATUS=0x%02x after %u ms", t.sys_status,
                     waited);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    ESP_LOGW(CAPTURE_LOG_TAG, "HDMI lock wait %u ms - starting CSI anyway", (unsigned)timeout_ms);
}

#if CONFIG_KVM_TC358743_ADV_DEBUG
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
#endif /* CONFIG_KVM_TC358743_ADV_DEBUG */

unsigned capture_csi_bpp(void)
{
    return capture_pixfmt()->bpp;
}

void capture_fill_esp_cam_color_types(esp_cam_ctlr_csi_config_t *csi, esp_isp_processor_cfg_t *isp)
{
    const capture_pixfmt_t *pf = capture_pixfmt();
    csi->input_data_color_type = (cam_ctlr_color_t)pf->cam_color;
    csi->output_data_color_type = (cam_ctlr_color_t)pf->cam_color;
    isp->input_data_color_type = (isp_color_t)pf->isp_color;
    isp->output_data_color_type = (isp_color_t)pf->isp_color;
}

static void capture_configure_p4_csi_bridge(uint32_t hres, uint32_t vres)
{
    MIPI_CSI_BRIDGE.frame_cfg.hadr_num = hres;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num = vres;
    MIPI_CSI_BRIDGE.frame_cfg.has_hsync_e = 0u;
    MIPI_CSI_BRIDGE.frame_cfg.vadr_num_check = 0u;
    const uint32_t csi_dt = capture_pixfmt()->csi_dt;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_min = csi_dt;
    MIPI_CSI_BRIDGE.data_type_cfg.data_type_max = csi_dt;
    MIPI_CSI_BRIDGE.int_clr.val = 0x3fu;

#if CAPTURE_DIRECT_ENCODE
    /*
     * rev >= 3.0 only: the CSI bridge has a colour-mode block (absent on rev < 3.0)
     * that sits in the DMA path when the ISP is bypassed. For a UYVY stream the
     * esp_cam driver leaves it at its reset defaults - input RGB888, YUV422 packing
     * YVYU, bypass on - because it early-returns once input==output colour type
     * (esp_cam_ctlr_csi.c). A UYVY stream then gets lane-routed as YVYU/RGB, which
     * shows as neon/inverted colour with chroma-period vertical banding. Program the
     * block explicitly for a YUV422 UYVY identity so native UYVY lands in DRAM - the
     * order both encoders consume. rev < 3.0 has no such block, hence the gate.
     */
    if (capture_pixfmt()->csi_dt == 0x1eu) {
        /* Keep the converter in bypass - passthrough preserves full resolution
         * (the active YUV422->YUV422 path halves horizontal res). Bypass ignores
         * the packing hint and this CSI's DMA packs bytes reversed within the
         * 32-bit word (the same reason RGB888 lands as BGR), so the wire's U Y V Y
         * arrives as Y V Y U -> acid. An 8-bit (full 32-bit byte) swap turns it
         * back into native UYVY, which is what both encoders read. */
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_en = 1;
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_bypass = 1;
        MIPI_CSI_BRIDGE.host_cm_ctrl.csi_host_cm_8bit_swap = 1;
    }
#endif
}

static bool IRAM_ATTR cam_on_get_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)ud;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_get_new_irqs, 1);
    /* Pick a buffer that is not the one we are already filling, not the newest
     * completed one (a consumer may be about to take it), and not one a consumer
     * holds. If none qualifies, leave trans->buffer NULL: the driver writes its
     * backup buffer and drops the frame. See capture_priv.h. */
    portENTER_CRITICAL_ISR(&c->fb_lock);
    int pick = -1;
    for (int k = 0; k < CAPTURE_FB_COUNT; k++) {
        if (k != c->write_fb_idx && k != c->ready_fb_idx && k != c->held_fb_idx) {
            pick = k;
            break;
        }
    }
    c->write_fb_idx = pick;
    portEXIT_CRITICAL_ISR(&c->fb_lock);
    if (pick >= 0) {
        trans->buffer = c->fb[pick];
        trans->buflen = c->frame_bytes;
    }
    return false;
}

static bool IRAM_ATTR cam_on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    capture_ctx_t *c = (capture_ctx_t *)ud;
    (void)__sync_add_and_fetch(&c->csi_dma_done_irqs, 1);
    /* on_trans_finished never fires for the backup buffer (the driver guards it),
     * so trans->buffer is always one of ours: mark it the newest completed frame. */
    if (trans && trans->buffer) {
        int idx = -1;
        for (int k = 0; k < CAPTURE_FB_COUNT; k++) {
            if (trans->buffer == c->fb[k]) {
                idx = k;
                break;
            }
        }
        if (idx >= 0) {
            portENTER_CRITICAL_ISR(&c->fb_lock);
            c->ready_fb_idx = idx;
            portEXIT_CRITICAL_ISR(&c->fb_lock);
            c->done_fb = trans->buffer;
        }
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
        /* Keep the driver's backup buffer: cam_on_get_new hands back no buffer
         * when every frame buffer is spoken for (one filling, one just completed,
         * one held by the encoder), and the DMA then lands the dropped frame here
         * instead of asserting. */
        .bk_buffer_dis = false,
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

    bridge_resetn_pulse();

    /* Usually already made - the display asks for it seconds before this. */
    ESP_ERROR_CHECK(capture_i2c_bus_init());
    i2c_master_bus_handle_t i2c_bus = s_i2c_bus;

    /* A missing capture card must not take the whole device down: without it the
     * KVM still serves HID, and the web UI explains what is wrong. */
    esp_err_t probe_err = kvm_bridge_detect(i2c_bus, &s_cap.bridge);
    if (probe_err != ESP_OK) {
        /* Nothing answered, so say the thing an operator can act on: the board
           or its ribbon, not an error code. Any other failure keeps the code. */
        if (probe_err == ESP_ERR_NOT_FOUND) {
            kvm_cap_report(KVM_CAP_VIDEO, false,
                           "no capture board found - check the ribbon between it and the device");
        } else {
            kvm_cap_report(KVM_CAP_VIDEO, false, "capture bridge not responding on I2C (%s)",
                           esp_err_to_name(probe_err));
        }
        ESP_LOGE(CAPTURE_LOG_TAG, "bridge detect failed: %s", esp_err_to_name(probe_err));
        return NULL;
    }
    /* The setting's choices are in the same order as the driver's enum. */
    const int32_t edid_choice = kvm_setting_int("edid_prof");
    (void)kvm_bridge_set_edid_profile(&s_cap.bridge, (kvm_bridge_edid_profile_t)edid_choice);

    probe_err = kvm_bridge_init_streaming(&s_cap.bridge);
    if (probe_err != ESP_OK) {
        kvm_cap_report(KVM_CAP_VIDEO, false, "%s init failed (%s)", s_cap.bridge.name,
                       esp_err_to_name(probe_err));
        ESP_LOGE(CAPTURE_LOG_TAG, "%s init failed: %s", s_cap.bridge.name, esp_err_to_name(probe_err));
        return NULL;
    }

    /* Select the bridge's CSI-2 pixel packing to match the active profile. init
     * leaves it at RGB888; the direct-encode profile wants native UYVY422 (0x1e).
     * The bridge remembers this and re-applies it after every HDMI recovery. On
     * the RGB888 profile this is a no-op, so the rev < 3.0 board is unaffected. */
    kvm_bridge_set_csi_uyvy422(&s_cap.bridge, capture_pixfmt()->csi_dt == 0x1eu);

    s_cap.tc_mu = xSemaphoreCreateMutex();
    if (!s_cap.tc_mu) {
        ESP_LOGE(CAPTURE_LOG_TAG, "bridge mutex");
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
    s_cap.fb_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_cap.write_fb_idx = -1;
    s_cap.ready_fb_idx = -1;
    s_cap.held_fb_idx = -1;
    s_cap.csi_dma_done_irqs = 0;
    s_cap.csi_get_new_irqs = 0;

    ESP_LOGI(CAPTURE_LOG_TAG, "CSI %s %ubpp ring %ux%zu bytes for up to %ux%u (align %zu)",
             capture_pixfmt()->name, capture_pixfmt()->bpp, CAPTURE_FB_COUNT, CAPTURE_MAX_FRAME_BYTES,
             CAPTURE_MAX_H_RES, CAPTURE_MAX_V_RES, align);

    s_cap.csi_done_sem = xSemaphoreCreateCounting(32, 0);
    if (!s_cap.csi_done_sem) {
        ESP_LOGE(CAPTURE_LOG_TAG, "CSI done sem");
        return NULL;
    }

    ESP_ERROR_CHECK(kvm_bridge_enable_hdmi_output(&s_cap.bridge));
    wait_bridge_pixel_stream(&s_cap.bridge, 5000);

    /* Start in whatever mode the source is actually sending. A machine that is
     * still in its firmware screens will not be at 1080p. */
    uint32_t hres = CAPTURE_MAX_H_RES;
    uint32_t vres = CAPTURE_MAX_V_RES;
    kvm_bridge_timings_t t = {0};
    if (kvm_bridge_get_timings(&s_cap.bridge, &t) == ESP_OK && kvm_bridge_timings_valid(&t)) {
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
    s_cap.frame_bytes = (size_t)hres * (size_t)vres * capture_pixfmt_bytes();
    capture_status_set_mode(hres, vres, t.interlaced);

    ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam));
    ESP_LOGI(CAPTURE_LOG_TAG, "capture running at %ux%u", hres, vres);

    kvm_cap_report(KVM_CAP_VIDEO, true, NULL);
    return &s_cap;
}

/* Reset the frame-ring bookkeeping and adopt a new mode. CSI is torn down before
 * this runs, so no ISR races these resets. */
static void capture_ctx_reset_ring(capture_ctx_t *c, uint32_t hres, uint32_t vres)
{
    c->ping_fb_idx = 0;
    c->done_fb = NULL;
    c->write_fb_idx = -1;
    c->ready_fb_idx = -1;
    c->held_fb_idx = -1;
    c->csi_dma_done_irqs = 0;
    c->csi_get_new_irqs = 0;
    c->hres = hres;
    c->vres = vres;
    c->frame_bytes = (size_t)hres * (size_t)vres * capture_pixfmt_bytes();
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
    capture_ctx_reset_ring(c, hres, vres);

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
    ESP_RETURN_ON_FALSE(c && c->bridge.ops && c->csi_done_sem, ESP_ERR_INVALID_ARG, CAPTURE_LOG_TAG, "ctx");

    ESP_LOGW(CAPTURE_LOG_TAG, "recovering: CSI teardown -> HDMI hotplug -> MIPI reapply -> restart");

    csi_destroy();
    capture_drain_csi_done_sem(c->csi_done_sem);

    if (!capture_tc_lock(c, 2000)) {
        ESP_LOGW(CAPTURE_LOG_TAG, "recover: bridge busy");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t er = kvm_bridge_hotplug_reset(&c->bridge);
    if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "hotplug reset: %s", esp_err_to_name(er));
    }
    wait_bridge_pixel_stream(&c->bridge, 5000);
    er = kvm_bridge_reapply_csi_path(&c->bridge);
    if (er != ESP_OK) {
        ESP_LOGW(CAPTURE_LOG_TAG, "reapply CSI path: %s", esp_err_to_name(er));
    }

    /* The source may well have come back in a different mode than it left in. */
    uint32_t hres = c->hres;
    uint32_t vres = c->vres;
    kvm_bridge_timings_t t = {0};
    if (kvm_bridge_get_timings(&c->bridge, &t) == ESP_OK && kvm_bridge_timings_valid(&t)) {
        hres = t.hact;
        vres = t.vact;
    }
    capture_tc_unlock(c);

    capture_ctx_reset_ring(c, hres, vres);

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
 * How long a source that is plainly there gets to start on its own before the
 * bridge pretends to be unplugged and plugged back in, and how many times.
 *
 * The case this exists for is the order things are switched on. The bridge
 * holds HPD low until this firmware has booted and started the capture, which
 * is some seconds in; a machine that boots faster looks at the input, finds no
 * monitor, and configures no output. Plenty of them never look again - it is
 * common on single-board computers, whose display drivers probe once at start.
 * Raising HPD later is not always enough either; what such a source acts on is
 * the edge, which is what unplugging a real monitor gives it.
 *
 * It happens at ten seconds of silence, then twenty, then forty, and then it
 * stops. The first one is the likeliest to work, and a source that is on and
 * deliberately quiet should not be poked for ever. The count starts again when
 * the picture comes back, and when the input is unplugged or plugged in - both
 * mean the situation is a different one.
 */
#define HDMI_NUDGE_FIRST_MS 10000
#define HDMI_NUDGE_MAX 3

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
    /* Nudging state, all of it reset the moment the picture or the cable does
     * something. -1 means "not counting": there is nothing to wait for. */
    int64_t quiet_since_us = had_signal ? -1 : (int64_t)esp_timer_get_time();
    int nudges = 0;
    bool had_ddc5v = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        capture_status_tick();

        kvm_bridge_timings_t t = {0};
        if (!capture_tc_lock(c, 100)) {
            continue;
        }
        esp_err_t er = kvm_bridge_get_timings(&c->bridge, &t);
        capture_tc_unlock(c);
        if (er != ESP_OK) {
            continue;
        }

        const bool valid = kvm_bridge_timings_valid(&t);
        c->signal_present = valid;
        capture_status_set_signal(valid, t.sys_status);

        if (!valid) {
            candidate_hits = 0;
            if (had_signal) {
                ESP_LOGW(CAPTURE_LOG_TAG, "HDMI signal lost (SYS_STATUS=0x%02x)", t.sys_status);
                had_signal = false;
                quiet_since_us = (int64_t)esp_timer_get_time();
                nudges = 0;
                /* Whatever was read off the screen describes a picture that is
                 * gone. Keeping it would let the console offer a copy of a
                 * screen the target is no longer showing. */
                capture_screentext_forget();
                capture_flat_forget();
            }

            /*
             * DDC5V is the one thing that tells the two silences apart. It is
             * the source's own +5 V on the input, so with it there is a machine
             * on the other end of the cable with the power on - and no picture
             * from it is worth doing something about. Without it the target is
             * off or nothing is plugged in, and hotplug cycles would be shouting
             * at an empty room.
             */
            const bool ddc5v = t.ddc5v;
            if (ddc5v != had_ddc5v) {
                had_ddc5v = ddc5v;
                quiet_since_us = (int64_t)esp_timer_get_time();
                nudges = 0;
            }
            if (!ddc5v || quiet_since_us < 0 || nudges >= HDMI_NUDGE_MAX) {
                continue;
            }
            const int64_t due_ms = (int64_t)HDMI_NUDGE_FIRST_MS << nudges;
            if ((int64_t)esp_timer_get_time() - quiet_since_us < due_ms * 1000) {
                continue;
            }
            nudges++;
            ESP_LOGW(CAPTURE_LOG_TAG,
                     "powered source, no picture for %lld s - offering it a fresh hotplug (%d/%d)",
                     (long long)(due_ms / 1000), nudges, HDMI_NUDGE_MAX);
            if (capture_tc_lock(c, 1000)) {
                esp_err_t ner = kvm_bridge_hotplug_reset(&c->bridge);
                capture_tc_unlock(c);
                if (ner != ESP_OK) {
                    ESP_LOGW(CAPTURE_LOG_TAG, "hotplug: %s", esp_err_to_name(ner));
                }
            }
            continue;
        }
        quiet_since_us = -1;
        nudges = 0;
        had_ddc5v = true;

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