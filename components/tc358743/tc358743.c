/*
 * SPDX-FileCopyrightText: 2026 Jonathan Rowny
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the p4kvm project: https://github.com/jrowny/p4kvm
 */
#include "tc358743.h"

#include "sdkconfig.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_board.h"
#include "tc358743_edid.h"
#include "tc358743_hdmi_debug.h"

static const char *TAG = "tc358743";

#define CHIPID 0x0000
#define SYSCTL 0x0002
#define MASK_IRRST 0x0800
#define MASK_CECRST 0x0400
#define MASK_CTXRST 0x0200
#define MASK_HDMIRST 0x0100
#define MASK_SLEEP 0x0001

#define CONFCTL 0x0004
#define MASK_YCBCRFMT 0x00c0
#define MASK_YCBCRFMT_422_8_BIT 0x00c0
#define MASK_VBUFEN 0x0001
#define MASK_ABUFEN 0x0002
#define MASK_AUDCHNUM_2 0x0c00
#define MASK_AUDOUTSEL_I2S 0x0010
#define MASK_AUTOINDEX 0x0004

#define FIFOCTL 0x0006

#define PLLCTL0 0x0020
#define MASK_PLL_PRD 0xf000
#define SET_PLL_PRD(p) ((((p)-1) << 12) & MASK_PLL_PRD)
#define MASK_PLL_FBD 0x01ff
#define SET_PLL_FBD(f) (((f)-1) & MASK_PLL_FBD)

#define PLLCTL1 0x0022
#define MASK_PLL_FRS 0x0c00
#define SET_PLL_FRS(x) (((x) << 10) & MASK_PLL_FRS)
#define MASK_PLL_EN 0x0001
#define MASK_RESETB 0x0002
#define MASK_CKEN 0x0010

#define CLW_CNTRL 0x0140
#define MASK_CLW_LANEDISABLE 0x0001
#define D0W_CNTRL 0x0144
#define MASK_D0W_LANEDISABLE 0x0001
#define D1W_CNTRL 0x0148
#define MASK_D1W_LANEDISABLE 0x0001
#define D2W_CNTRL 0x014c
#define MASK_D2W_LANEDISABLE 0x0001
#define D3W_CNTRL 0x0150
#define MASK_D3W_LANEDISABLE 0x0001

#define STARTCNTRL 0x0204
#define MASK_START 0x00000001

#define LINEINITCNT 0x0210
#define LPTXTIMECNT 0x0214
#define TCLK_HEADERCNT 0x0218
#define TCLK_TRAILCNT 0x021c
#define THS_HEADERCNT 0x0220
#define TWAKEUP 0x0224
#define TCLK_POSTCNT 0x0228
#define THS_TRAILCNT 0x022c
#define HSTXVREGCNT 0x0230
#define HSTXVREGEN 0x0234
#define MASK_CLM_HSTXVREGEN 0x0001
#define MASK_D0M_HSTXVREGEN 0x0002
#define MASK_D1M_HSTXVREGEN 0x0004
#define MASK_D2M_HSTXVREGEN 0x0008
#define MASK_D3M_HSTXVREGEN 0x0010

#define TXOPTIONCNTRL 0x0238

#define CSI_START 0x0518
#define MASK_STRT 0x00000001

#define CSI_CONFW 0x0500
#define MASK_MODE_SET 0xa0000000
#define MASK_MODE_CLEAR 0xc0000000
#define MASK_ADDRESS_CSI_CONTROL 0x03000000
#define MASK_ADDRESS_CSI_ERR_INTENA 0x14000000
#define MASK_ADDRESS_CSI_ERR_HALT 0x15000000
#define MASK_ADDRESS_CSI_INT_ENA 0x06000000
#define MASK_CSI_MODE 0x8000
#define MASK_TXHSMD 0x0080
#define MASK_NOL_1 0x0000
#define MASK_NOL_2 0x0002
#define MASK_NOL_3 0x0004
#define MASK_NOL_4 0x0006

#define MASK_INTER 0x00000004
#define MASK_INER 0x00000200
#define MASK_WCER 0x00000100
#define MASK_QUNK 0x00000010
#define MASK_TXBRK 0x00000002

#define EDID_MODE 0x85c7
#define MASK_EDID_MODE 0x03
#define MASK_EDID_MODE_E_DDC 0x02
#define EDID_LEN1 0x85ca
#define EDID_LEN2 0x85cb
#define EDID_RAM 0x8c00
#define DDC_CTL 0x8543
#define MASK_DDC5V_MODE 0x03
#define HPD_CTL 0x8544
#define MASK_HPD_OUT0 0x01

#define SYS_STATUS 0x8520
#define VI_STATUS1 0x8522
/** Linux tc358743_regs.h: interlace flag lives in VI_STATUS1, not VI_STATUS3. */
#define MASK_S_V_INTERLACE 0x10
#define VI_STATUS2 0x8525
#define CLK_STATUS 0x8526
#define PHYERR_STATUS 0x8527
#define VI_STATUS3 0x8528
#define CSI_STATUS 0x0410
#define CSI_CONTROL 0x040c
#define CSI_INT 0x0414
#define CSI_ERR 0x044c
#define HDMI_DVI 0x8550
/** Captured AVI InfoFrame RAM (Linux tc358743_regs.h). */
#define PK_AVI_0HEAD 0x8710
#define PK_AVI_16BYTE 0x8723
#define PK_AVI_LEN ((PK_AVI_16BYTE) - (PK_AVI_0HEAD) + 1)
/** Measured input timing (TC9590XBG sec 6.7 same map as TC358743 on common boards). */
#define HACT0 0x8582
#define HACT1 0x8583
#define VACT0 0x8588
#define VACT1 0x8589
#define HTOTAL0 0x858a
#define HTOTAL1 0x858b
#define VTOTAL0 0x858c
#define VTOTAL1 0x858d
#define PHY_EN 0x8534
#define MASK_ENABLE_PHY 0x01
#define PHY_CTL0 0x8531
#define MASK_PHY_SYSCLK_IND 0x02
#define PHY_CTL1 0x8532
#define MASK_PHY_AUTO_RST1 0xf0
#define SET_PHY_AUTO_RST1_US(us) ((((us) / 200) << 4) & MASK_PHY_AUTO_RST1)
#define MASK_FREQ_RANGE_MODE 0x0f
#define SET_FREQ_RANGE_MODE_CYCLES(c) (((c)-1) & MASK_FREQ_RANGE_MODE)
#define PHY_CTL2 0x8533
#define MASK_PHY_AUTO_RST4 0x04
#define MASK_PHY_AUTO_RST3 0x02
#define MASK_PHY_AUTO_RST2 0x01
#define MASK_PHY_AUTO_RSTn (MASK_PHY_AUTO_RST4 | MASK_PHY_AUTO_RST3 | MASK_PHY_AUTO_RST2)
#define PHY_BIAS 0x8536
#define PHY_CSQ 0x853f
#define MASK_CSQ_CNT 0x0f
#define SET_CSQ_CNT_LEVEL(n) ((n) & MASK_CSQ_CNT)
#define PHY_RST 0x8535
#define MASK_RESET_CTRL 0x01
#define HDMI_DET 0x8552
#define MASK_HDMI_DET_V 0x30
#define HV_RST 0x85af
#define MASK_H_PI_RST 0x01
#define MASK_V_PI_RST 0x02
#define AVM_CTL 0x8546

#define SYS_FREQ0 0x8540
#define SYS_FREQ1 0x8541
#define FH_MIN0 0x85aa
#define FH_MIN1 0x85ab
#define FH_MAX0 0x85ac
#define FH_MAX1 0x85ad
#define LOCKDET_REF0 0x8630
#define LOCKDET_REF1 0x8631
#define LOCKDET_REF2 0x8632
#define NCO_F0_MOD 0x8670
#define MASK_NCO_F0_MOD 0x03
#define MASK_NCO_F0_MOD_27MHZ 0x01

#define HDCP_MODE 0x8560
#define MASK_MANUAL_AUTHENTICATION 0x02

#define VI_MODE 0x8570
#define MASK_RGB_DVI 0x08
#define VOUT_SET2 0x8573
#define MASK_SEL422 0x80
#define MASK_VOUT_422FIL_100 0x40
#define MASK_VOUTCOLORMODE 0x03
#define MASK_VOUTCOLORMODE_AUTO 0x01
#define VOUT_SET3 0x8574
#define MASK_VOUT_EXTCNT 0x08

#define VI_REP 0x8576
#define MASK_VOUT_COLOR_SEL 0xe0
#define MASK_VOUT_COLOR_RGB_FULL 0x00
#define MASK_VOUT_COLOR_601_YCBCR_LIMITED 0x60
#define MASK_VOUT_COLOR_709_YCBCR_LIMITED 0xa0

#define INTSTATUS 0x0014
#define INTMASK 0x0016
#define MASK_HDMI_MSK 0x0200
#define MASK_CSI_MSK 0x0100

#define VI_MUTE 0x857f
#define MASK_AUTO_MUTE 0xc0
#define MASK_VI_MUTE 0x10

#define PK_INT_MODE 0x8709
#define MASK_ISRC2_INT_MODE 0x80
#define MASK_ISRC_INT_MODE 0x40
#define MASK_ACP_INT_MODE 0x20
#define MASK_VS_INT_MODE 0x10
#define MASK_SPD_INT_MODE 0x08
#define MASK_MS_INT_MODE 0x04
#define MASK_AUD_INT_MODE 0x02
#define MASK_AVI_INT_MODE 0x01
#define NO_PKT_LIMIT 0x870b
#define NO_PKT_CLR 0x870c
#define ERR_PK_LIMIT 0x870d
#define NO_PKT_LIMIT2 0x870e
#define NO_GDB_LIMIT 0x9007

#define FORCE_MUTE 0x8602
#define AUTO_CMD0 0x8603
#define MASK_AUTO_MUTE7 0x80
#define MASK_AUTO_MUTE6 0x40
#define MASK_AUTO_MUTE5 0x20
#define MASK_AUTO_MUTE4 0x10
#define MASK_AUTO_MUTE1 0x02
#define MASK_AUTO_MUTE0 0x01
#define AUTO_CMD1 0x8604
#define MASK_AUTO_MUTE9 0x02
#define AUTO_CMD2 0x8605
#define MASK_AUTO_PLAY3 0x08
#define MASK_AUTO_PLAY2 0x04
#define BUFINIT_START 0x8606
#define SET_BUFINIT_START_MS(ms) ((ms) & 0xff)
#define FS_MUTE 0x8607
#define FS_IMODE 0x8608
#define MASK_NLPCM_SMODE 0x02
#define MASK_FS_SMODE 0x01
#define ACR_MODE 0x8609
#define MASK_CTS_MODE 0x02
#define ACR_MDF0 0x860a
#define MASK_ACR_L2MDF_1976_PPM 0x20
#define MASK_ACR_L1MDF_976_PPM 0x10
#define ACR_MDF1 0x860b
#define MASK_ACR_L3MDF_3906_PPM 0x07
#define SDO_MODE1 0x860d
#define MASK_SDO_FMT_I2S 0x02
#define DIV_MODE 0x8612
#define SET_DIV_DLY_MS(ms) ((ms) & 0xff)

struct tc358743 {
    i2c_master_dev_handle_t i2c;
    tc358743_cfg_t cfg;
    bool csi_uyvy422;
    tc358743_edid_profile_t edid_profile;
};

void tc358743_cfg_defaults_waveshare_pi(tc358743_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->refclk_hz = KVM_BOARD_TC358743_REFCLK_HZ;
    c->pll_prd = c->refclk_hz / 6000000u;
    /* Must match esp_cam CSI lane_bit_rate_mbps (KVM_BOARD_MIPI_LANE_MBPS). */
    const uint32_t bps_per_lane = (uint32_t)KVM_BOARD_MIPI_LANE_MBPS * 1000000u;
    c->pll_fbd = (uint16_t)(bps_per_lane / c->refclk_hz * c->pll_prd);
    c->fifo_level = 374;
    c->lanes = 2;
    c->ddc5v_mode = 0x02;
    c->lineinitcnt = 0x1b58;
    c->lptxtimecnt = 0x007;
    c->tclk_headercnt = 0x2806;
    c->tclk_trailcnt = 0x00;
    c->ths_headercnt = 0x0806;
    c->twakeup = 0x4268;
    c->tclk_postcnt = 0x008;
    c->ths_trailcnt = 0x5;
    c->hstxvregcnt = 0;
    c->enable_hdcp = false;
    c->hdmi_detection_delay = 0;
}

static esp_err_t i2c_write_reg(tc358743_t *d, uint16_t reg, const void *data, size_t len)
{
    uint8_t buf[2 + len];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xff);
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(d->i2c, buf, sizeof(buf), -1);
}

static esp_err_t i2c_read_reg(tc358743_t *d, uint16_t reg, void *data, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    return i2c_master_transmit_receive(d->i2c, addr, sizeof(addr), data, len, -1);
}

static void wr8(tc358743_t *d, uint16_t r, uint8_t v)
{
    i2c_write_reg(d, r, &v, 1);
}

static void wr16(tc358743_t *d, uint16_t r, uint16_t v)
{
    uint8_t le[2] = {(uint8_t)(v & 0xff), (uint8_t)(v >> 8)};
    i2c_write_reg(d, r, le, 2);
}

static void wr32(tc358743_t *d, uint16_t r, uint32_t v)
{
    uint8_t le[4] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff), (uint8_t)((v >> 16) & 0xff),
                     (uint8_t)((v >> 24) & 0xff)};
    i2c_write_reg(d, r, le, 4);
}

static uint8_t rd8(tc358743_t *d, uint16_t r)
{
    uint8_t v = 0;
    i2c_read_reg(d, r, &v, 1);
    return v;
}

static uint16_t rd16(tc358743_t *d, uint16_t r)
{
    uint8_t b[2] = {0};
    i2c_read_reg(d, r, b, 2);
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

#if CONFIG_KVM_TC358743_ADV_DEBUG
static uint32_t rd32(tc358743_t *d, uint16_t r)
{
    uint8_t b[4] = {0};
    i2c_read_reg(d, r, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
#endif

static void wr16_and_or(tc358743_t *d, uint16_t r, uint16_t mask, uint16_t val)
{
    wr16(d, r, (rd16(d, r) & mask) | val);
}

static void wr8_and_or(tc358743_t *d, uint16_t r, uint8_t mask, uint8_t val)
{
    wr8(d, r, (rd8(d, r) & mask) | val);
}

static void reset_blocks(tc358743_t *d, uint16_t mask)
{
    uint16_t sysctl = rd16(d, SYSCTL);
    wr16(d, SYSCTL, sysctl | mask);
    wr16(d, SYSCTL, sysctl & ~mask);
}

static void sleep_mode(tc358743_t *d, bool enable)
{
    wr16_and_or(d, SYSCTL, (uint16_t)~MASK_SLEEP, enable ? MASK_SLEEP : 0);
}

static void enable_stream(tc358743_t *d, bool enable)
{
    if (enable) {
        /* Non-continuous MIPI clock: leave TXOPTIONCNTRL as set_csi() left it (0); matches p4kvm / Linux. */
        wr8(d, VI_MUTE, MASK_AUTO_MUTE);
    } else {
        wr8(d, VI_MUTE, MASK_AUTO_MUTE | MASK_VI_MUTE);
    }
    wr16_and_or(d, CONFCTL, (uint16_t) ~(MASK_VBUFEN | MASK_ABUFEN),
                enable ? (MASK_VBUFEN | MASK_ABUFEN) : 0);
}

static void set_ref_clk(tc358743_t *d)
{
    tc358743_cfg_t *pdata = &d->cfg;
    uint32_t sys_freq = pdata->refclk_hz / 10000;
    wr8(d, SYS_FREQ0, sys_freq & 0x00ff);
    wr8(d, SYS_FREQ1, (sys_freq & 0xff00) >> 8);

    wr8_and_or(d, PHY_CTL0, (uint8_t)~MASK_PHY_SYSCLK_IND, (pdata->refclk_hz == 42000000) ? MASK_PHY_SYSCLK_IND : 0);

    uint16_t fh_min = pdata->refclk_hz / 100000;
    wr8(d, FH_MIN0, fh_min & 0x00ff);
    wr8(d, FH_MIN1, (fh_min & 0xff00) >> 8);

    uint16_t fh_max = (uint16_t)((fh_min * 66) / 10);
    wr8(d, FH_MAX0, fh_max & 0x00ff);
    wr8(d, FH_MAX1, (fh_max & 0xff00) >> 8);

    uint32_t lockdet_ref = pdata->refclk_hz / 100;
    wr8(d, LOCKDET_REF0, lockdet_ref & 0x0000ff);
    wr8(d, LOCKDET_REF1, (lockdet_ref & 0x00ff00) >> 8);
    wr8(d, LOCKDET_REF2, (lockdet_ref & 0x0f0000) >> 16);

    wr8_and_or(d, NCO_F0_MOD, (uint8_t)~MASK_NCO_F0_MOD, (pdata->refclk_hz == 27000000) ? MASK_NCO_F0_MOD_27MHZ : 0);

    uint32_t cec_freq = (656u * sys_freq) / 4200u;
    wr16(d, 0x0028, (uint16_t)cec_freq);
    wr16(d, 0x002a, (uint16_t)cec_freq);
}

static void set_pll(tc358743_t *d)
{
    tc358743_cfg_t *pdata = &d->cfg;
    uint16_t pllctl0 = rd16(d, PLLCTL0);
    uint16_t pllctl1 = rd16(d, PLLCTL1);
    uint16_t pllctl0_new = SET_PLL_PRD(pdata->pll_prd) | SET_PLL_FBD(pdata->pll_fbd);
    uint32_t hsck = (pdata->refclk_hz / pdata->pll_prd) * pdata->pll_fbd;

    if ((pllctl0 != pllctl0_new) || ((pllctl1 & MASK_PLL_EN) == 0)) {
        uint16_t pll_frs;
        if (hsck > 500000000) {
            pll_frs = 0x0;
        } else if (hsck > 250000000) {
            pll_frs = 0x1;
        } else if (hsck > 125000000) {
            pll_frs = 0x2;
        } else {
            pll_frs = 0x3;
        }

        sleep_mode(d, true);
        wr16(d, PLLCTL0, pllctl0_new);
        wr16_and_or(d, PLLCTL1, (uint16_t) ~(MASK_PLL_FRS | MASK_RESETB | MASK_PLL_EN),
                    SET_PLL_FRS(pll_frs) | MASK_RESETB | MASK_PLL_EN);
        vTaskDelay(pdMS_TO_TICKS(1));
        wr16_and_or(d, PLLCTL1, (uint16_t)~MASK_CKEN, MASK_CKEN);
        sleep_mode(d, false);
    }
}

static void set_hdmi_hdcp(tc358743_t *d, bool enable)
{
    if (enable) {
        return;
    }
    wr8_and_or(d, HDCP_MODE, (uint8_t)~MASK_MANUAL_AUTHENTICATION, MASK_MANUAL_AUTHENTICATION);
}

static void set_hdmi_phy(tc358743_t *d)
{
    tc358743_cfg_t *pdata = &d->cfg;

    wr8_and_or(d, PHY_EN, (uint8_t)~MASK_ENABLE_PHY, 0);
    wr8(d, PHY_CTL1, SET_PHY_AUTO_RST1_US(1600) | SET_FREQ_RANGE_MODE_CYCLES(1));
    wr8_and_or(d, PHY_CTL2, (uint8_t)~MASK_PHY_AUTO_RSTn,
               (pdata->hdmi_phy_auto_reset_tmds_detected ? MASK_PHY_AUTO_RST2 : 0) |
                   (pdata->hdmi_phy_auto_reset_tmds_in_range ? MASK_PHY_AUTO_RST3 : 0) |
                   (pdata->hdmi_phy_auto_reset_tmds_valid ? MASK_PHY_AUTO_RST4 : 0));
    wr8(d, PHY_BIAS, 0x40);
    wr8(d, PHY_CSQ, SET_CSQ_CNT_LEVEL(0x0a));
    wr8(d, AVM_CTL, 45);
    wr8_and_or(d, HDMI_DET, (uint8_t)~MASK_HDMI_DET_V, (uint8_t)(pdata->hdmi_detection_delay << 4));
    wr8_and_or(d, HV_RST, (uint8_t) ~(MASK_H_PI_RST | MASK_V_PI_RST),
               (pdata->hdmi_phy_auto_reset_hsync_out_of_range ? MASK_H_PI_RST : 0) |
                   (pdata->hdmi_phy_auto_reset_vsync_out_of_range ? MASK_V_PI_RST : 0));
    wr8_and_or(d, PHY_EN, (uint8_t)~MASK_ENABLE_PHY, MASK_ENABLE_PHY);
}

static void set_hdmi_audio(tc358743_t *d)
{
    (void)d;
    wr8(d, FORCE_MUTE, 0x00);
    wr8(d, AUTO_CMD0,
        MASK_AUTO_MUTE7 | MASK_AUTO_MUTE6 | MASK_AUTO_MUTE5 | MASK_AUTO_MUTE4 | MASK_AUTO_MUTE1 | MASK_AUTO_MUTE0);
    wr8(d, AUTO_CMD1, MASK_AUTO_MUTE9);
    wr8(d, AUTO_CMD2, MASK_AUTO_PLAY3 | MASK_AUTO_PLAY2);
    wr8(d, BUFINIT_START, SET_BUFINIT_START_MS(500));
    wr8(d, FS_MUTE, 0x00);
    wr8(d, FS_IMODE, MASK_NLPCM_SMODE | MASK_FS_SMODE);
    wr8(d, ACR_MODE, MASK_CTS_MODE);
    wr8(d, ACR_MDF0, MASK_ACR_L2MDF_1976_PPM | MASK_ACR_L1MDF_976_PPM);
    wr8(d, ACR_MDF1, MASK_ACR_L3MDF_3906_PPM);
    wr8(d, SDO_MODE1, MASK_SDO_FMT_I2S);
    wr8(d, DIV_MODE, SET_DIV_DLY_MS(100));
    wr16_and_or(d, CONFCTL, 0xffff, MASK_AUDCHNUM_2 | MASK_AUDOUTSEL_I2S | MASK_AUTOINDEX);
}

static void set_hdmi_info_frame(tc358743_t *d)
{
    wr8(d, PK_INT_MODE,
        MASK_ISRC2_INT_MODE | MASK_ISRC_INT_MODE | MASK_ACP_INT_MODE | MASK_VS_INT_MODE | MASK_SPD_INT_MODE |
            MASK_MS_INT_MODE | MASK_AUD_INT_MODE | MASK_AVI_INT_MODE);
    wr8(d, NO_PKT_LIMIT, 0x2c);
    wr8(d, NO_PKT_CLR, 0x53);
    wr8(d, ERR_PK_LIMIT, 0x01);
    wr8(d, NO_PKT_LIMIT2, 0x30);
    wr8(d, NO_GDB_LIMIT, 0x10);
}

static void initial_setup(tc358743_t *d)
{
    tc358743_cfg_t *pdata = &d->cfg;

    wr16_and_or(d, SYSCTL, (uint16_t) ~(MASK_IRRST | MASK_CECRST), MASK_IRRST | MASK_CECRST);
    reset_blocks(d, MASK_CTXRST | MASK_HDMIRST);
    sleep_mode(d, false);

    wr16(d, FIFOCTL, pdata->fifo_level);
    set_ref_clk(d);
    wr8_and_or(d, DDC_CTL, (uint8_t)~MASK_DDC5V_MODE, pdata->ddc5v_mode & MASK_DDC5V_MODE);
    wr8_and_or(d, EDID_MODE, (uint8_t)~MASK_EDID_MODE, MASK_EDID_MODE_E_DDC);

    set_hdmi_phy(d);
    set_hdmi_hdcp(d, pdata->enable_hdcp);
    set_hdmi_audio(d);
    set_hdmi_info_frame(d);

    wr8_and_or(d, VI_MODE, (uint8_t)~MASK_RGB_DVI, 0);
    wr8_and_or(d, VOUT_SET2, (uint8_t)~MASK_VOUTCOLORMODE, MASK_VOUTCOLORMODE_AUTO);
    wr8(d, VOUT_SET3, MASK_VOUT_EXTCNT);
}

/** RGB888 Linux tc358743_set_csi_color_space(RGB888_1X24). */
static void set_csi_color_space_rgb888_regs(tc358743_t *d)
{
    wr8_and_or(d, VOUT_SET2, (uint8_t) ~(MASK_SEL422 | MASK_VOUT_422FIL_100), 0);
    wr8_and_or(d, VI_REP, (uint8_t)~MASK_VOUT_COLOR_SEL, MASK_VOUT_COLOR_RGB_FULL);
    wr16_and_or(d, CONFCTL, (uint16_t)~MASK_YCBCRFMT, 0);
}

/** UYVY 16-bit Linux tc358743_set_csi_color_space(MEDIA_BUS_FMT_UYVY8_1X16). */
static void set_csi_color_space_uyvy422_regs(tc358743_t *d)
{
    wr8_and_or(d, VOUT_SET2, (uint8_t) ~(MASK_SEL422 | MASK_VOUT_422FIL_100),
               (uint8_t)(MASK_SEL422 | MASK_VOUT_422FIL_100));
    wr8_and_or(d, VI_REP, (uint8_t)~MASK_VOUT_COLOR_SEL, MASK_VOUT_COLOR_601_YCBCR_LIMITED);
    wr16_and_or(d, CONFCTL, (uint16_t)~MASK_YCBCRFMT, MASK_YCBCRFMT_422_8_BIT);
}

static void apply_csi_color_space(tc358743_t *d)
{
    if (d->csi_uyvy422) {
        set_csi_color_space_uyvy422_regs(d);
    } else {
        set_csi_color_space_rgb888_regs(d);
    }
}

void tc358743_set_csi_uyvy422(tc358743_t *d, bool uyvy422)
{
    if (!d) {
        return;
    }
    d->csi_uyvy422 = uyvy422;
    apply_csi_color_space(d);
}

static void set_csi_lanes(tc358743_t *d, unsigned lanes)
{
    tc358743_cfg_t *pdata = &d->cfg;

    reset_blocks(d, MASK_CTXRST);

    if (lanes < 1) {
        wr32(d, CLW_CNTRL, MASK_CLW_LANEDISABLE);
    }
    if (lanes < 1) {
        wr32(d, D0W_CNTRL, MASK_D0W_LANEDISABLE);
    }
    if (lanes < 2) {
        wr32(d, D1W_CNTRL, MASK_D1W_LANEDISABLE);
    }
    if (lanes < 3) {
        wr32(d, D2W_CNTRL, MASK_D2W_LANEDISABLE);
    }
    if (lanes < 4) {
        wr32(d, D3W_CNTRL, MASK_D3W_LANEDISABLE);
    }

    wr32(d, LINEINITCNT, pdata->lineinitcnt);
    wr32(d, LPTXTIMECNT, pdata->lptxtimecnt);
    wr32(d, TCLK_HEADERCNT, pdata->tclk_headercnt);
    wr32(d, TCLK_TRAILCNT, pdata->tclk_trailcnt);
    wr32(d, THS_HEADERCNT, pdata->ths_headercnt);
    wr32(d, TWAKEUP, pdata->twakeup);
    wr32(d, TCLK_POSTCNT, pdata->tclk_postcnt);
    wr32(d, THS_TRAILCNT, pdata->ths_trailcnt);
    wr32(d, HSTXVREGCNT, pdata->hstxvregcnt);

    wr32(d, HSTXVREGEN,
         ((lanes > 0) ? MASK_CLM_HSTXVREGEN : 0) | ((lanes > 0) ? MASK_D0M_HSTXVREGEN : 0) |
             ((lanes > 1) ? MASK_D1M_HSTXVREGEN : 0) | ((lanes > 2) ? MASK_D2M_HSTXVREGEN : 0) |
             ((lanes > 3) ? MASK_D3M_HSTXVREGEN : 0));

    /* Linux tc358743 set_csi(): TXOPTIONCNTRL = 0 (non-continuous MIPI clock). */
    wr32(d, TXOPTIONCNTRL, 0);
    wr32(d, STARTCNTRL, MASK_START);
    wr32(d, CSI_START, MASK_STRT);

    uint32_t nol = (lanes == 4) ? MASK_NOL_4 : (lanes == 3) ? MASK_NOL_3 : (lanes == 2) ? MASK_NOL_2 : MASK_NOL_1;

    wr32(d, CSI_CONFW, MASK_MODE_SET | MASK_ADDRESS_CSI_CONTROL | MASK_CSI_MODE | MASK_TXHSMD | nol);
    wr32(d, CSI_CONFW, MASK_MODE_SET | MASK_ADDRESS_CSI_ERR_INTENA | MASK_TXBRK | MASK_QUNK | MASK_WCER | MASK_INER);

    wr32(d, CSI_CONFW, MASK_MODE_CLEAR | MASK_ADDRESS_CSI_ERR_HALT | MASK_TXBRK | MASK_QUNK);

    wr32(d, CSI_CONFW, MASK_MODE_SET | MASK_ADDRESS_CSI_INT_ENA | MASK_INTER);
}

static void hpd_set(tc358743_t *d, bool on)
{
    wr8_and_or(d, HPD_CTL, (uint8_t)~MASK_HPD_OUT0, on ? MASK_HPD_OUT0 : 0);
}

/**
 * Load EDID into internal RAM (HPD must stay low, source must not DDC during this).
 * Caller raises HPD after PLL/CSI and any other sink setup (Linux: delayed hotplug ~143 ms).
 */
static void edid_write_builtin(tc358743_t *d)
{
    const uint8_t *bin = (d->edid_profile == TC358743_EDID_1080P30) ? tc358743_edid_1080p30
                                                                   : tc358743_edid_full;
    ESP_LOGI(TAG, "EDID profile: %s",
             (d->edid_profile == TC358743_EDID_1080P30) ? "1080p30 only" : "full mode list");
    const uint16_t edid_len = TC358743_EDID_TOTAL_LEN;
    wr8(d, EDID_LEN1, edid_len & 0xff);
    wr8(d, EDID_LEN2, edid_len >> 8);
    for (uint16_t i = 0; i < edid_len; i += 128) {
        i2c_write_reg(d, EDID_RAM + i, bin + i, 128);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t tc358743_probe(i2c_master_bus_handle_t bus, const tc358743_cfg_t *cfg, tc358743_t **out_dev)
{
    ESP_RETURN_ON_FALSE(bus && out_dev, ESP_ERR_INVALID_ARG, TAG, "args");
    tc358743_t *d = calloc(1, sizeof(tc358743_t));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "calloc");
    if (cfg) {
        d->cfg = *cfg;
    } else {
        tc358743_cfg_defaults_waveshare_pi(&d->cfg);
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TC358743_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &d->i2c);
    if (err != ESP_OK) {
        free(d);
        ESP_LOGE(TAG, "i2c add device fail %s", esp_err_to_name(err));
        return err;
    }
    *out_dev = d;
    return ESP_OK;
}

void tc358743_remove(tc358743_t *d)
{
    if (!d) {
        return;
    }
    i2c_master_bus_rm_device(d->i2c);
    free(d);
}

esp_err_t tc358743_read_chip_id(tc358743_t *d, uint16_t *chip_id)
{
    ESP_RETURN_ON_FALSE(d && chip_id, ESP_ERR_INVALID_ARG, TAG, "args");
    /* Same as Linux / older p4kvm: 0x0000 is common and not used for probe. */
    *chip_id = rd16(d, CHIPID);
    return ESP_OK;
}

esp_err_t tc358743_sys_status(tc358743_t *d, uint8_t *out_st)
{
    ESP_RETURN_ON_FALSE(d && out_st, ESP_ERR_INVALID_ARG, TAG, "args");
    *out_st = rd8(d, SYS_STATUS);
    return ESP_OK;
}

esp_err_t tc358743_set_edid_profile(tc358743_t *d, tc358743_edid_profile_t profile)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "dev");
    d->edid_profile = profile;
    return ESP_OK;
}

esp_err_t tc358743_init_streaming(tc358743_t *d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "dev");

    /* HPD low so the source does not DDC/EDID until we are ready (matches old bridge_init + Linux). */
    hpd_set(d, false);
    vTaskDelay(pdMS_TO_TICKS(20));

    initial_setup(d);
    edid_write_builtin(d);

    uint16_t id = 0;
    tc358743_read_chip_id(d, &id);
    ESP_LOGI(TAG, "CHIPID 0x%04x", id);

    enable_stream(d, false);
    set_pll(d);
    set_csi_lanes(d, d->cfg.lanes);
    d->csi_uyvy422 = false;
    apply_csi_color_space(d);

    wr16(d, INTSTATUS, 0xffff);
    wr16(d, INTMASK, (uint16_t)(~(MASK_HDMI_MSK | MASK_CSI_MSK) & 0xffff));

    /* HPD and enable_stream(true): call tc358743_enable_hdmi_output() before esp_cam_ctlr_start() so MIPI is active. */
    tc358743_debug_status(d);
    return ESP_OK;
}

esp_err_t tc358743_enable_hdmi_output(tc358743_t *d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "dev");
    /* Same tail as old bridge_init: video FIFO on, then delayed hotplug edge. */
    enable_stream(d, true);
    vTaskDelay(pdMS_TO_TICKS(150));
    hpd_set(d, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* STRT after CONFCTL enables video, some boards leave CSI TX idle until this is rewritten. */
    wr32(d, CSI_START, MASK_STRT);
    tc358743_debug_status(d);
    return ESP_OK;
}

esp_err_t tc358743_hdmi_hotplug_reset(tc358743_t *d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "dev");
    enable_stream(d, false);
    hpd_set(d, false);
    vTaskDelay(pdMS_TO_TICKS(150));
    return tc358743_enable_hdmi_output(d);
}

esp_err_t tc358743_reapply_csi_path_after_hdmi(tc358743_t *d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "dev");
    /*
     * Programming CSI before TMDS can leave MIPI idle; reapply after lock.
     * Note: VI_STATUS1==0 on TC9590-class maps means 444/24p/no GBD, not "no video"; use HAct/VAct.
     */
    apply_csi_color_space(d);
    set_csi_lanes(d, d->cfg.lanes);
    wr32(d, CSI_START, MASK_STRT);
    return ESP_OK;
}

static uint16_t tc358743_read_hact_vact_htotal(tc358743_t *d, uint16_t *vact, uint16_t *htotal,
                                               uint16_t *vtotal)
{
    uint8_t h0 = rd8(d, HACT0);
    uint8_t h1 = rd8(d, HACT1);
    uint8_t v0 = rd8(d, VACT0);
    uint8_t v1 = rd8(d, VACT1);
    uint8_t ht0 = rd8(d, HTOTAL0);
    uint8_t ht1 = rd8(d, HTOTAL1);
    uint8_t vt0 = rd8(d, VTOTAL0);
    uint8_t vt1 = rd8(d, VTOTAL1);
    *vact = (uint16_t)v0 | (uint16_t)((v1 & 0x1fu) << 8);
    *htotal = (uint16_t)ht0 | (uint16_t)((ht1 & 0x1fu) << 8);
    *vtotal = (uint16_t)vt0 | (uint16_t)((vt1 & 0x3fu) << 8);
    return (uint16_t)h0 | (uint16_t)((h1 & 0x1fu) << 8);
}

esp_err_t tc358743_get_timings(tc358743_t *d, tc358743_timings_t *out)
{
    ESP_RETURN_ON_FALSE(d && out, ESP_ERR_INVALID_ARG, TAG, "args");

    memset(out, 0, sizeof(*out));
    out->hact = tc358743_read_hact_vact_htotal(d, &out->vact, &out->htotal, &out->vtotal);

    const uint8_t st = rd8(d, SYS_STATUS);
    out->sys_status = st;
    out->ddc5v = (st & 0x01u) != 0u;
    out->tmds = (st & 0x02u) != 0u;
    out->hdmi_mode = (st & 0x10u) != 0u;
    out->sync = (st & 0x80u) != 0u;
    out->interlaced = (rd8(d, VI_STATUS1) & MASK_S_V_INTERLACE) != 0u;
    return ESP_OK;
}

bool tc358743_timings_valid(const tc358743_timings_t *t)
{
    if (!t || !t->tmds || !t->sync) {
        return false;
    }
    /* Guard against half-latched counters while the source retrains: a mode is
     * only believable if the active area fits inside the total. */
    return t->hact >= 320u && t->vact >= 200u && t->htotal > t->hact && t->vtotal > t->vact;
}

esp_err_t tc358743_get_avi_color_format(tc358743_t *d, uint8_t *out_y)
{
    if (!d || !out_y) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t avi[32];
    memset(avi, 0, sizeof(avi));
    esp_err_t er = i2c_read_reg(d, PK_AVI_0HEAD, avi, PK_AVI_LEN);
    if (er != ESP_OK) {
        return er;
    }
    if (avi[0] != 0x82u || avi[2] < 2u || PK_AVI_LEN < 6u) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_y = (uint8_t)((avi[5] >> 5) & 7u);
    return ESP_OK;
}

void tc358743_debug_stall_extras(tc358743_t *d)
{
#if !CONFIG_KVM_TC358743_ADV_DEBUG
    (void)d;
#else
    if (!d) {
        return;
    }
    uint16_t conf = rd16(d, CONFCTL);
    unsigned yfmt = (unsigned)((conf >> 6) & 3u);
    ESP_LOGW(TAG,
             "stall CONFCTL=0x%04x YCbCrFmt=%u (0=444 1=422_12 2=colorbar 3=422_8) VBUFEN:%u ABUFEN:%u",
             conf, yfmt, (unsigned)(conf & 1u), (unsigned)((conf >> 1) & 1u));

    uint32_t csi_err = rd32(d, CSI_ERR);
    ESP_LOGW(TAG,
             "stall CSI_ERR=0x%08" PRIx32 " (Linux: INER=0x200 WCER=0x100 QUNK=0x10 TXBRK=0x2)", csi_err);

    /*
     * TC9590XBG Table 4-2 / sec 6.8: contiguous read from PK_AVI_0HEAD (0x8710) yields
     * avi[0..2]=HB0..2, avi[3]=checksum, avi[4]=PB0, avi[5]=PB1, ... (CEA-861 payload).
     * sec 4.2: YCbCr444 24bpp uses the same CSI-2 DataType as RGB888 (0x24); sec 4.3 Y->G Cr->R Cb->B.
     */
    uint8_t avi[24];
    memset(avi, 0, sizeof(avi));
    esp_err_t er = i2c_read_reg(d, PK_AVI_0HEAD, avi, PK_AVI_LEN);
    if (er != ESP_OK) {
        ESP_LOGW(TAG, "stall AVI read %s", esp_err_to_name(er));
        return;
    }
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, avi, PK_AVI_LEN, ESP_LOG_WARN);
    if (PK_AVI_LEN >= 4u) {
        ESP_LOGW(TAG,
                 "stall AVI layout (Table 4-2): HB=[%02x %02x %02x] chksum=%02x PB0=%02x PB1=%02x PB2=%02x PB3=%02x",
                 avi[0], avi[1], avi[2], avi[3], avi[4], avi[5], avi[6], avi[7]);
    }
    ESP_LOGW(TAG,
             "stall MIPI note (sec 4.2): RGB888 and HDMI YCbCr444 24bpp both use DT 0x24: esp_cam RGB888 matches 444-out");

    /* CEA-861 AVI v2: PB1 Y2:Y1:Y0 = bits 7..5 of avi[5]; PB4 VIC = avi[8] bits 6..0 */
    if (avi[0] == 0x82u && avi[2] >= 2u && PK_AVI_LEN >= 6u) {
        uint8_t pb1 = avi[5];
        unsigned y = (unsigned)(pb1 >> 5) & 7u;
        const char *ys =
            (y == 0) ? "RGB" : (y == 1) ? "YCbCr422" : (y == 2) ? "YCbCr444" : (y == 3) ? "YCbCr420" : "other/RSVD";
        ESP_LOGW(TAG, "stall AVI CEA: PB1 Y=%u (%s)", y, ys);
        if (PK_AVI_LEN > 8u) {
            unsigned vic = (unsigned)(avi[8] & 0x7fu);
            ESP_LOGW(TAG, "stall AVI CEA: PB4 VIC=%u (0=unspecified per packet)", vic);
        }
    }
#endif /* CONFIG_KVM_TC358743_ADV_DEBUG */
}

void tc358743_debug_bridge(tc358743_t *d)
{
#if !CONFIG_KVM_TC358743_ADV_DEBUG
    (void)d;
#else
    if (!d) {
        return;
    }
    uint8_t sys = rd8(d, SYS_STATUS);
    uint8_t vi = rd8(d, VI_STATUS1);
    uint8_t vi2 = rd8(d, VI_STATUS2);
    uint8_t vi3 = rd8(d, VI_STATUS3);
    uint8_t clkst = rd8(d, CLK_STATUS);
    uint8_t phyerr = rd8(d, PHYERR_STATUS);
    uint8_t hdmi_dvi = rd8(d, HDMI_DVI);
    uint16_t csi = rd16(d, CSI_STATUS);
    uint16_t csi_ctl = rd16(d, CSI_CONTROL);
    uint16_t csi_int = rd16(d, CSI_INT);
    uint16_t intst = rd16(d, INTSTATUS);
    uint16_t conf = rd16(d, CONFCTL);
    uint8_t vout2 = rd8(d, VOUT_SET2);
    uint8_t vimute = rd8(d, VI_MUTE);
    uint16_t vact = 0, htotal = 0, vtotal = 0;
    uint16_t hact = tc358743_read_hact_vact_htotal(d, &vact, &htotal, &vtotal);

    /* Bit layout per Linux tc358743_regs.h (matches Toshiba REF_01). */
    unsigned csi_hlt = (unsigned)(csi & 1u);
    unsigned csi_rxact = (unsigned)((csi >> 8) & 1u);
    unsigned csi_txact = (unsigned)((csi >> 9) & 1u);
    unsigned csi_wsync = (unsigned)((csi >> 10) & 1u);
    unsigned csi_int_hlt = (unsigned)((csi_int >> 3) & 1u);
    unsigned csi_inter = (unsigned)((csi_int >> 2) & 1u);

    ESP_LOGW(TAG,
             "bridge SYS=0x%02x VI1=0x%02x VI2=0x%02x VI3=0x%02x CONFCTL=0x%04x VOUT2=0x%02x VI_MUTE=0x%02x",
             sys, vi, vi2, vi3, conf, vout2, vimute);
    ESP_LOGW(TAG,
             "  timing HAct=%u VAct=%u HTot=%u VTot=%u | HDMI_DVI=0x%02x CLK_ST=0x%02x PHYERR=0x%02x INTSTATUS=0x%04x",
             (unsigned)hact, (unsigned)vact, (unsigned)htotal, (unsigned)vtotal, hdmi_dvi, clkst, phyerr, intst);
    ESP_LOGW(TAG,
             " CSI_STATUS=0x%04x (Hlt:%u RxAct:%u TxAct:%u WSync:%u) CSIctl=0x%04x CSIint=0x%04x (IntHlt:%u INTER:%u)",
             csi, csi_hlt, csi_rxact, csi_txact, csi_wsync, csi_ctl, csi_int, csi_int_hlt, csi_inter);
#endif /* CONFIG_KVM_TC358743_ADV_DEBUG */
}

esp_err_t tc358743_set_streaming(tc358743_t *d, bool on)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "dev");
    enable_stream(d, on);
    if (!on) {
        set_csi_lanes(d, d->cfg.lanes);
    }
    return ESP_OK;
}
