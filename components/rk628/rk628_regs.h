/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip RK628F/H registers, as far as this driver uses them.
 *
 * There is no public register manual. Addresses, bit positions and values are
 * facts read from Rockchip's GPL-2.0 Linux driver (the copy Waveshare ships for
 * the Raspberry Pi), noted with the function each one came from. No code was
 * taken: this file names the facts, rk628.c is written from them.
 *
 * Every register is 32 bits at a 32-bit address; the I2C transfer carries the
 * full address. "HIWORD" registers take a write-enable mask in bits 31:16 for
 * bits 15:0, so a field can be set without reading it first.
 */
#pragma once

/* ---- blocks ---------------------------------------------------------------- */

#define RK_GRF 0x00000u
#define RK_HDMIRX 0x30000u
#define RK_CSI0 0x40000u
#define RK_TXPHY 0x90000u
#define RK_EDID 0xa0000u /* one EDID byte per 32-bit word */
#define RK_CRU 0xc0000u
#define RK_CSI1 0x140000u

/* ---- GRF --------------------------------------------------------------------- */

#define GRF_SYSTEM_CON0 0x0000u   /* not HIWORD: read-modify-write */
#define SYS0_INPUT_MASK 0x7u      /* [2:0] 0 = HDMI */
#define SYS0_OUTPUT_FH_SHIFT 3    /* [4:3] on F/H: 3 = CSI */
#define SYS0_OUTPUT_FH_MASK (0x3u << 3)
#define SYS0_HDCP_EFUSE (1u << 8)
#define SYS0_I2S_DATA_OEN (1u << 10) /* active low: 0 drives the I2S data pin */
#define SYS0_EDID_ON_APB (1u << 21)  /* 1: EDID RAM is the host's, 0: served on DDC */
#define SYS0_ADAPTER_ADDR_MASK (0x7u << 22)
#define SYS0_HSYNC_POL (1u << 25)
#define SYS0_VSYNC_POL (1u << 26)

#define GRF_SYSTEM_CON3 0x000cu /* bit 1: second D-PHY (dual MIPI only) */
#define GRF_SCALER_CON0 0x0010u /* HIWORD */
#define GRF_SCALER_CON1 0x0014u
#define GRF_SCALER_CON2 0x0018u
#define GRF_SCALER_CON3 0x001cu
#define GRF_SCALER_CON4 0x0020u
#define GRF_SCALER_CON5 0x0024u
#define GRF_SCALER_CON6 0x0028u
#define GRF_SCALER_CON7 0x002cu
#define GRF_SCALER_CON8 0x0030u
#define GRF_POST_PROC_CON 0x0034u /* bit 0 split (dual), [6:5] TX PHY ref clock */
#define GRF_CSC_CTRL 0x0038u      /* HIWORD: bit 8 YUV->VYU swap, bit 11 programmable matrix */
#define GRF_RGB_DEC_CON0 0x0040u  /* [28:16] input h-active */
#define GRF_MIPI_TX0_CON 0x0060u  /* D-PHY test interface and power */
#define TX0_TEST_DIN_MASK 0xffu
#define TX0_TEST_EN (1u << 8)
#define TX0_TEST_CLK (1u << 9)
#define TX0_TEST_CLR (1u << 10)
#define TX0_FORCE_RX_MASK (0xfu << 12)
#define TX0_FORCE_STOP_MASK (0xfu << 16)
#define TX0_SHUTDOWNZ (1u << 20)
#define TX0_RSTZ (1u << 21)
#define GRF_DPHY0_STATUS 0x0064u
#define GRF_MIPI_TX1_CON 0x0068u
#define GRF_GPIO0AB_SEL 0x0070u /* HIWORD pin mux */
#define GRF_GPIO1AB_SEL 0x0074u
#define GRF_GPIO3AB_SEL 0x0080u
#define GRF_BG_CTRL 0x00f0u     /* bit 31 solid colour out, [29:20] V, [19:10] Y, [9:0] U */
#define GRF_RXPHY_CTRL 0x00f4u  /* F/H RX PHY power and reset steps */
#define GRF_CSC_MATRIX 0x01a0u  /* 0x01a0..0x01b0 coefficients, 0x01b4..0x01bc offsets */
#define GRF_SYSTEM_STATUS0 0x0120u
#define STATUS0_I2C_ONLY (1u << 6) /* 0: the chip's MCU set its own clocks */
#define GRF_SOC_VERSION 0x0200u
#define RK628D_ID 0x20200326u
#define RK628FH_ID 0x20230321u

/* ---- CRU --------------------------------------------------------------------- */

#define CRU_APLL_CON0 0x0040u
#define CRU_APLL_CON1 0x0044u
#define CRU_APLL_CON2 0x0048u
#define PLL_LOCK (1u << 10) /* in CON1 */
#define CRU_MODE_CON00 0x0060u  /* HIWORD: bit 0 CPLL, 2 GPLL, 4 APLL (1 = PLL out) */
#define CRU_CLKSEL_CON02 0x0088u /* HIWORD: bit 8 RX read clock parent, 9 VOP (1 GPLL, 0 CPLL) */
#define CRU_CLKSEL_CON05 0x0094u /* HIWORD: [13:6] audio divider - 1, [15:14] parent (2 APLL) */
#define CRU_CLKSEL_CON12 0x00b0u /* CEC clock, fractional m << 16 | n of 24 MHz */
#define CRU_CLKSEL_CON13 0x00b4u /* VOP clock, fractional m << 16 | n of the parent */
#define CRU_CLKSEL_CON14 0x00b8u /* RX read clock, fractional */
#define CRU_SOFTRST_CON00 0x0200u
#define RST0_CLK_RX 11
#define RST0_VOP 13
#define CRU_SOFTRST_CON02 0x0208u
#define RST2_HDMIRX 2
#define RST2_TXPHY 3
#define RST2_CSI 8
#define RST2_TXDATA 9
#define RST2_DECODER 10
#define RST2_HDMIRX_PON 12
#define RST2_TXBYTEHS 13
#define RST2_CSI1 15
#define RK_CPLL_HZ 1188000000u /* the CPLL rate the chip's firmware leaves */

/* ---- HDMI RX ----------------------------------------------------------------- */

#define RX_SETUP_CTRL 0x0000u /* bit 0: HPD out */
#define RX_TIMER_CTRL 0x0008u
#define RX_MODE_RECOVER 0x0080u
#define RX_ERROR_PROTECT 0x0084u
#define RX_SYNC_CTRL 0x0090u
#define RX_CKM_EVLTM 0x0094u
#define RX_CKM_F 0x0098u
#define RX_CKM_RESULT 0x009cu /* [15:0] TMDS cycles per 1000 cycles of 49.5 MHz */
#define RX_RESMPL_CTRL 0x00a4u
#define RX_DCM_CTRL 0x00a8u
#define RX_VM_CFG_CH0_1 0x00b0u
#define RX_VM_CFG_CH2 0x00b4u
#define RX_HDCP_CTRL 0x00c0u
#define RX_HDCP_SETTINGS 0x00c4u
#define RX_MD_HCTRL1 0x0140u
#define RX_MD_HCTRL2 0x0144u
#define RX_MD_HT0 0x0148u
#define RX_MD_HT1 0x014cu /* [31:16] htotal, [15:0] hsync + back porch */
#define RX_MD_HACT_PX 0x0150u
#define RX_MD_VCTRL 0x0158u
#define RX_MD_VSC 0x015cu
#define RX_MD_VOL 0x0164u
#define RX_MD_VAL 0x0168u
#define RX_MD_VTH 0x016cu
#define RX_MD_VTL 0x0170u
#define RX_MD_IL_POL 0x017cu
#define RX_MD_STS 0x0180u /* bit 3 interlaced */
#define RX_AUD_PLL_CTRL 0x0208u
#define RX_AUD_CLK_CTRL 0x0214u
#define RX_AUD_FIFO_CTRL 0x0240u
#define RX_AUD_FIFO_TH 0x0244u
#define RX_AUD_CHEXTR_CTRL 0x0254u
#define RX_AUD_MUTE_CTRL 0x0258u
#define RX_AUD_FIFO_FILL 0x025cu
#define RX_AUD_SAO_CTRL 0x0260u /* bit 0 32-bit slots, [8:5] data lines off */
#define AUD_SAO_ON 0x001u
#define AUD_SAO_OFF 0x1e1u
#define RX_AUD_PAO_CTRL 0x0264u
#define RX_AUDPLL_GEN_CTS 0x0280u
#define RX_AUDPLL_GEN_N 0x0284u
#define RX_PHYG3_CTRL 0x02c0u
#define RX_I2CM_PHY_ADDR 0x02c8u
#define RX_I2CM_PHY_DATAO 0x02ccu
#define RX_I2CM_PHY_OP 0x02d4u
#define RX_I2CM_PHY_MODE 0x02d8u
#define RX_I2CM_PHY_SS_CNTS 0x02e0u
#define RX_I2CM_PHY_FS_HCNT 0x02e4u
#define RX_PDEC_CTRL 0x0300u
#define RX_PDEC_ACRM_CTRL 0x0330u
#define RX_PDEC_ERR_FILTER 0x033cu
#define RX_PDEC_ASP_CTRL 0x0340u
#define RX_PDEC_STS 0x0360u /* bit 28: DVI */
#define RX_PDEC_GCP 0x0380u /* [7:4] colour depth, 5 = 10-bit */
#define RX_PDEC_ACR_CTS 0x0390u
#define RX_PDEC_ACR_N 0x0394u
#define RX_PDEC_AVI_PB 0x03a4u /* [30:24] VIC, [22:20] EC, [19:18] Q, [15:14] C, [6:5] Y */
#define RX_HDMI20_CONTROL 0x0800u
#define RX_SCDC_I2CCONFIG 0x0804u
#define RX_SCDC_CONFIG 0x0808u
#define RX_CHLOCK_CONFIG 0x080cu
#define RX_SCDC_REGS0 0x0820u /* bit 17: TMDS ratio 1/40 (HDMI 2.0) */
#define RX_SCDC_REGS1 0x0824u /* [11:8] clock and channel locks */
#define RX_SCDC_WRDATA0 0x0860u
#define RX_HDMI20_STATUS 0x08e0u /* bit 0 scrambling */
#define RX_AUD_FIFO_ISTS 0x0fb0u /* bit 4 overflow, bit 3 underflow */
#define RX_AUD_FIFO_ICLR 0x0fb8u
#define RX_DMI_SW_RST 0x0ff0u
#define RX_DMI_DISABLE_IF 0x0ff4u /* bit 7 video out, 5 CEC, 4 audio, 2 HDMI */
#define DMI_CEC_ON (1u << 5)

/* CEC, inside the receiver. Interrupt status shared with audio; one byte per
 * 32-bit word in the frame buffers. */
#define RX_AUD_CEC_IEN_CLR 0x0f90u
#define RX_AUD_CEC_IEN_SET 0x0f94u
#define RX_AUD_CEC_ISTS 0x0f98u
#define RX_AUD_CEC_ICLR 0x0fa0u
#define CEC_I_ERROR (1u << 20)
#define CEC_I_ARB_LOST (1u << 19)
#define CEC_I_NACK (1u << 18)
#define CEC_I_RX_EOM (1u << 17)
#define CEC_I_TX_DONE (1u << 16)
#define CEC_I_ALL (CEC_I_ERROR | CEC_I_ARB_LOST | CEC_I_NACK | CEC_I_RX_EOM | CEC_I_TX_DONE)
#define RX_CEC_CTRL 0x1f00u /* bit 0 send, [2:1] wait: 0 retry, 1 new initiator, 2 same initiator */
#define RX_CEC_MASK 0x1f08u
#define RX_CEC_ADDR_L 0x1f14u /* logical address bitmap, bits 7:0 */
#define RX_CEC_ADDR_H 0x1f18u /* bits 15:8; bit 15 set once any address is held */
#define RX_CEC_TX_CNT 0x1f1cu
#define RX_CEC_RX_CNT 0x1f20u /* [4:0] */
#define RX_CEC_TX_DATA 0x1f40u
#define RX_CEC_RX_DATA 0x1f80u
#define RX_CEC_LOCK 0x1fc0u /* write 0 to free the receive buffer */
#define DMI_VIDEO_EN (1u << 7)
#define DMI_AUDIO_EN (1u << 4)

/* ---- CSI TX (same layout at RK_CSI0 and RK_CSI1) ------------------------------ */

#define CSI_CONFIG_DONE 0x0000u /* write 0x10: take the shadow registers now */
#define CSI_CTRL 0x0004u
#define CSI_UV_SWAP (1u << 13)
#define CSI_YUV422_EN (1u << 12)
#define CSI_YUV422_MODE_MASK (0x3u << 10)
#define CSI_YUV422_MODE_2 (0x2u << 10)
#define CSI_P2_EN (1u << 8)
#define CSI_LANES_MASK (0x3u << 4) /* lanes - 1 */
#define CSI_DPHY_EN (1u << 2)
#define CSI_TX_EN (1u << 0)
#define CSI_VERSION 0x0008u
#define CSI_SYS_CTRL0 0x0010u /* bit 0: hold the transmitter */
#define CSI_SYS_CTRL1 0x0014u /* bit 0: bypass select */
#define CSI_SYS_CTRL2 0x0018u /* written 0x21 */
#define CSI_SYS_CTRL3 0x001cu /* bit 0 non-continuous clock, 4 continuous, 8 clear */
#define CSI_VOP_PATH_CTRL 0x0040u
#define CSI_VOP_FILTER 0x0044u
#define CSI_PHY_STATUS 0x0074u /* bit 0 PLL lock, 1 clock lane stop, 4 lane 0 stop */
#define CSI_INTR_EN 0x0080u
#define CSI_ERR_INTR_EN 0x0090u
#define CSI_ERR_INTR_CLR 0x0094u
#define CSI_ERR_RAW 0x009cu
#define CSI_DPHY_CTRL 0x00b0u /* [7:3] lane enables: clock, D0..D3 */
#define CSI_DT_YUV422_8 0x1eu

/* ---- combo TX PHY ------------------------------------------------------------- */

#define TXPHY_CON0 0x0000u
#define TXPHY_IDLE_MASK (0x3ffu << 20)
#define TXPHY_PD_MASK (0x3ffu << 8)
#define TXPHY_WIDTH_MASK (0x3u << 5)
#define TXPHY_WIDTH_8 (0x2u << 5)
#define TXPHY_PLL_PD (1u << 4)
#define TXPHY_GVI_LVDS_EN (1u << 3)
#define TXPHY_MIPI_EN (1u << 2)
#define TXPHY_MODULE_B (1u << 1)
#define TXPHY_MODULE_A (1u << 0)
#define TXPHY_CON5 0x0014u /* PLL: [26:24] rate_div / 2, [14:10] fb int, [9:0] fb frac */
#define TXPHY_CON9 0x0024u /* bit 29 FSET, 28 R-cal, [6:4] LP slew trim */
