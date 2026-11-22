/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip RK628F/H - the bridge on the Waveshare HDMI to CSI Adapter (B), the
 * Radxa 4K HDMI IN to MIPI CSI-2 adapter and the WarpKVM boards.
 *
 * NOT RUN ON HARDWARE. Nobody has put one of these on an ESP-KVM yet. It is
 * written from the facts in Rockchip's Linux driver (see rk628_regs.h for where
 * they came from) and does what that driver does, in the same order, for the
 * one path a KVM needs: HDMI in, two CSI lanes of UYVY out.
 *
 * Unlike the TC358743 or the LT6911D, this chip is told everything by its host:
 * the receiver PHY, the EDID and hotplug, the scaler that times the output, the
 * colour conversion, the MIPI PLL. Even in its "MCU + I2C" mode (both adapters
 * run it so), the chip's own firmware only sets up the PLLs at power-on.
 *
 * What it does:
 *   - finds the chip by its version register (GRF 0x0200);
 *   - writes the EDID of the chosen profile and raises hotplug itself (the Linux
 *     driver leaves hotplug low after an EDID change on this chip generation);
 *   - polls for a signal (the adapters wire no interrupt line), locks the
 *     receiver PHY, reads the mode until it holds still;
 *   - sends it on two lanes at 1296 Mbit/s, the one rate the Linux driver uses
 *     for two lanes (with its hand-set D-PHY timings). That carries up to
 *     1080p60. A faster source is scaled to 1080p on the chip, keeping its frame
 *     rate; one that still does not fit is refused, rather than sent and torn;
 *   - converts whatever colour the source sends to BT.709 full-range YCbCr;
 *   - passes HDMI audio out as I2S, 48 kHz and up, 32-bit slots (what kvm_audio
 *     already takes from the TC358743).
 *
 * What it leaves for later: CEC (the chip has a controller), the audio clock's
 * fine trim (the Linux driver nudges it by 10 Hz to keep the FIFO half full;
 * here a FIFO that runs over or dry is reset instead), deep-colour correctness.
 *
 * NOT LINKED. Without the -u line in CMakeLists.txt the linker drops this object
 * and the driver is absent. README.md in this folder says what else connecting
 * it takes.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_bridge.h"
#include "rk628_regs.h"
#include "sdkconfig.h"

static const char *TAG = "rk628";

#define RK628_I2C_ADDR 0x50
#define LANES 2
/* The lane rate the Linux driver uses for two lanes, and its PLL word. The
 * capture takes it from kvm_bridge_t.lane_mbps, whatever the board's default. */
#define LANE_MBPS 1296
#define TXPHY_CON5_1296 0x00003600u
/* The hsfreqrange code for 1300 Mbit/s, shifted as test register 0x44 wants it. */
#define DPHY_HSFREQ_1300 0x76u
/* Two lanes at 1296 carry 1296 * 2 / 16 = 162 MHz of UYVY. A little under, for
 * the packet headers and the line-to-line LP states. 1080p60 is 148.5. */
#define MAX_OUT_PCLK_HZ 150000000u
/* How often to look for a source while there is none, and re-read the mode
 * while there is one. Each look for a source can take ~0.4 s of I2C. */
#define SEEK_EVERY_US (2 * 1000 * 1000)
#define CHECK_EVERY_US (500 * 1000)

typedef struct {
    uint16_t hact, vact, htotal, vtotal;
    uint16_t hs, hbp, hfp, vs, vbp, vfp;
    uint32_t pclk; /* Hz */
} rk_mode_t;

typedef struct {
    i2c_master_dev_handle_t i2c;
    esp_err_t err; /* the first failure of a sequence; later writes are skipped */
    bool ready;    /* chip set up, EDID written */
    bool locked;   /* receiver locked and the output running */
    kvm_bridge_edid_profile_t profile;
    kvm_bridge_edid_id_t edid_id;
    int64_t next_look_us;
    rk_mode_t in, out;
    uint8_t avi_y; /* 0 RGB, 1 YUV422, 2 YUV444, 3 YUV420 */
    bool dvi;
    uint32_t audio_hz; /* the sample rate the audio clock is set for, 0 none */
    bool cec_on;    /* CEC asked for; set up again after every receiver reset */
    uint8_t cec_la; /* our logical address, 15 for none */
    kvm_bridge_timings_t last;
} rk628_t;

static void cec_restore(rk628_t *d); /* the CEC section, after the receiver code */

/* ---- I2C: 32-bit address and value, both little-endian ------------------------- */

static esp_err_t raw_write(rk628_t *d, uint32_t addr, uint32_t val)
{
    const uint8_t b[8] = {
        (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
        (uint8_t)val,  (uint8_t)(val >> 8),  (uint8_t)(val >> 16),  (uint8_t)(val >> 24),
    };
    return i2c_master_transmit(d->i2c, b, sizeof(b), 100);
}

static esp_err_t raw_read(rk628_t *d, uint32_t addr, uint32_t *val)
{
    const uint8_t a[4] = {(uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24)};
    uint8_t v[4] = {0};
    esp_err_t err = i2c_master_transmit_receive(d->i2c, a, sizeof(a), v, sizeof(v), 100);
    *val = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
    return err;
}

/* Sequences are long; the first failure is kept and the rest skipped, so each
 * step does not need its own check. The caller looks at d->err at the end. */
static void wr(rk628_t *d, uint32_t addr, uint32_t val)
{
    if (d->err == ESP_OK) {
        d->err = raw_write(d, addr, val);
    }
}

static uint32_t rd(rk628_t *d, uint32_t addr)
{
    uint32_t v = 0;
    if (d->err == ESP_OK) {
        d->err = raw_read(d, addr, &v);
    }
    return v;
}

static void upd(rk628_t *d, uint32_t addr, uint32_t mask, uint32_t val)
{
    const uint32_t v = rd(d, addr);
    wr(d, addr, (v & ~mask) | (val & mask));
}

/* A HIWORD register: bits 31:16 enable the write of bits 15:0. */
static void hiw(rk628_t *d, uint32_t addr, uint16_t mask, uint16_t val)
{
    wr(d, addr, (uint32_t)mask << 16 | (val & mask));
}

static void reset_pulse(rk628_t *d, uint32_t con, int bit)
{
    wr(d, RK_CRU + con, 1u << (bit + 16) | 1u << bit);
    esp_rom_delay_us(10);
    wr(d, RK_CRU + con, 1u << (bit + 16));
    esp_rom_delay_us(10);
}

static void sleep_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
}

/* ---- clocks -------------------------------------------------------------------- */

/* A fractional divider word m << 16 | n with rate = parent * m / n, the closest
 * 16-bit pair (continued fractions). */
static uint32_t frac_word(uint32_t rate, uint32_t parent)
{
    uint64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    uint64_t a = rate, b = parent;
    while (b) {
        const uint64_t k = a / b;
        const uint64_t p2 = k * p1 + p0, q2 = k * q1 + q0;
        if (p2 > 0xffff || q2 > 0xffff) {
            break;
        }
        p0 = p1;
        q0 = q1;
        p1 = p2;
        q1 = q2;
        const uint64_t t = a - k * b;
        a = b;
        b = t;
    }
    return (uint32_t)(p1 << 16 | q1);
}

/* The receiver's read clock (the source's pixel clock) and the VOP clock (the
 * output's), both from the CPLL, each followed by its reset. */
static void set_pixel_clocks(rk628_t *d)
{
    reset_pulse(d, CRU_SOFTRST_CON02, RST2_DECODER);
    hiw(d, RK_CRU + CRU_CLKSEL_CON02, 0x0300, 0x0000); /* both from the CPLL */
    wr(d, RK_CRU + CRU_CLKSEL_CON14, frac_word(d->in.pclk, RK_CPLL_HZ));
    reset_pulse(d, CRU_SOFTRST_CON00, RST0_CLK_RX);
    wr(d, RK_CRU + CRU_CLKSEL_CON13, frac_word(d->out.pclk, RK_CPLL_HZ));
    reset_pulse(d, CRU_SOFTRST_CON00, RST0_VOP);
}

/* The APLL at @p hz: 24 MHz / refdiv * (fbdiv + frac / 2^24) / pd1 / pd2, with
 * the VCO kept at 600-1600 MHz. */
static void set_apll(rk628_t *d, uint32_t hz)
{
    int pd1 = 7, pd2 = 7;
    for (int a = 7; a >= 1; a--) {
        for (int b = a; b >= 1; b--) {
            const uint64_t vco = (uint64_t)hz * (uint64_t)(a * b);
            if (vco >= 600000000ull && vco <= 1600000000ull) {
                pd1 = a;
                pd2 = b;
                goto found;
            }
        }
    }
found:;
    const uint64_t vco = (uint64_t)hz * (uint64_t)(pd1 * pd2);
    const uint32_t fbdiv = (uint32_t)(vco / 24000000u);
    const uint32_t frac = (uint32_t)(((vco % 24000000u) << 24) / 24000000u);
    wr(d, RK_CRU + CRU_APLL_CON0, 0xffff0000u | (uint32_t)pd1 << 12 | (fbdiv & 0xfff));
    wr(d, RK_CRU + CRU_APLL_CON1, 0xffff0000u | (frac ? 0 : 1u << 12) | (uint32_t)pd2 << 6 | 1u);
    wr(d, RK_CRU + CRU_APLL_CON2, frac & 0xffffff);
    for (int i = 0; i < 10 && d->err == ESP_OK; i++) {
        if (rd(d, RK_CRU + CRU_APLL_CON1) & PLL_LOCK) {
            return;
        }
        sleep_ms(1);
    }
    ESP_LOGW(TAG, "APLL did not lock at %" PRIu32 " Hz", hz);
}

/* The audio clock is 128 times the sample rate: the APLL at four times that,
 * divided by four. */
static void set_audio_clock(rk628_t *d, uint32_t fs)
{
    const uint32_t mclk = fs * 128u;
    hiw(d, RK_CRU + CRU_MODE_CON00, 0x0010, 0x0010);
    set_apll(d, mclk * 4u);
    hiw(d, RK_CRU + CRU_CLKSEL_CON05, 0xffc0, (uint16_t)(2u << 14 | (4u - 1u) << 6));
}

/* ---- EDID and hotplug ------------------------------------------------------------ */

static void hpd(rk628_t *d, bool on)
{
    upd(d, RK_HDMIRX + RX_SETUP_CTRL, 1u, on ? 1u : 0u);
}

/*
 * Hotplug low, the EDID into the chip's RAM (one byte per 32-bit word, while
 * the RAM is switched to the host), back to the DDC, hotplug high again. The
 * Linux driver does not raise hotplug again on this chip generation after an
 * EDID write; a source then never reads the new EDID. Half a second low is
 * what a source reliably notices.
 */
static void write_edid(rk628_t *d)
{
    uint8_t e[KVM_BRIDGE_EDID_LEN];
    const char *what = NULL;
    kvm_bridge_edid_build(d->profile, &d->edid_id, e, &what);
    ESP_LOGI(TAG, "EDID profile: %s", what);
    hpd(d, false);
    upd(d, RK_GRF + GRF_SYSTEM_CON0, SYS0_ADAPTER_ADDR_MASK | SYS0_EDID_ON_APB, SYS0_EDID_ON_APB);
    for (size_t i = 0; i < sizeof(e) && d->err == ESP_OK; i++) {
        wr(d, RK_EDID + 4u * i, e[i]);
    }
    upd(d, RK_GRF + GRF_SYSTEM_CON0, SYS0_ADAPTER_ADDR_MASK | SYS0_EDID_ON_APB, 0);
    sleep_ms(500);
    hpd(d, true);
}

/* ---- the receiver --------------------------------------------------------------- */

static void rx_controller_reset(rk628_t *d)
{
    wr(d, RK_CRU + CRU_SOFTRST_CON02, 1u << (RST2_HDMIRX + 16) | 1u << RST2_HDMIRX | 1u << (RST2_HDMIRX_PON + 16) |
                                         1u << RST2_HDMIRX_PON);
    esp_rom_delay_us(10);
    wr(d, RK_CRU + CRU_SOFTRST_CON02, 1u << (RST2_HDMIRX + 16) | 1u << (RST2_HDMIRX_PON + 16));
    esp_rom_delay_us(10);
    wr(d, RK_HDMIRX + RX_DMI_SW_RST, 0x000101ffu);
    wr(d, RK_HDMIRX + RX_DMI_DISABLE_IF, 0);
    wr(d, RK_HDMIRX + RX_DMI_DISABLE_IF, 0x17fu);
    wr(d, RK_HDMIRX + RX_DMI_DISABLE_IF, 0x1017fu);
}

/* The receiver's fixed setup, in the order the Linux driver writes it. */
static void rx_controller_setup(rk628_t *d)
{
    static const uint32_t seq[][2] = {
        {RX_HDMI20_CONTROL, 0x10000011u}, {RX_MODE_RECOVER, 0x21u},       {RX_PDEC_CTRL, 0xbfff8011u},
        {RX_PDEC_ASP_CTRL, 0x40u},        {RX_RESMPL_CTRL, 0},            {RX_SYNC_CTRL, 0x14u},
        {RX_PDEC_ERR_FILTER, 0x8u},       {RX_SCDC_I2CCONFIG, 0x01000000u}, {RX_SCDC_CONFIG, 0x1u},
        {RX_SCDC_WRDATA0, 0xabcdef01u},   {RX_CHLOCK_CONFIG, 0x0030c15cu}, {RX_ERROR_PROTECT, 0x000d0c98u},
        {RX_MD_HCTRL1, 0x10u},            {RX_MD_HCTRL2, 0x173au},         {RX_MD_VCTRL, 0x2u},
        {RX_MD_VTH, 0x73au},              {RX_MD_IL_POL, 0x4u},            {RX_PDEC_ACRM_CTRL, 0},
        {RX_DCM_CTRL, 0x00040414u},       {RX_CKM_EVLTM, 0x00103e70u},     {RX_CKM_F, 0x0c1c0b54u},
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        wr(d, RK_HDMIRX + seq[i][0], seq[i][1]);
    }
    upd(d, RK_HDMIRX + RX_TIMER_CTRL, 1u << 11, 1u << 11);
    upd(d, RK_HDMIRX + RX_HDCP_SETTINGS, 1u << 13 | 1u << 12 | 1u << 9 | 1u << 8, 1u << 13);
    /* No HDCP keys on a KVM: HDCP and encryption off. */
    upd(d, RK_HDMIRX + RX_HDCP_CTRL, 1u << 24 | 1u, 0);
}

static void rx_audio_setup(rk628_t *d)
{
    set_audio_clock(d, 44100); /* the Linux driver's start value; corrected once the rate is known */
    d->audio_hz = 0;
    wr(d, RK_HDMIRX + RX_AUDPLL_GEN_CTS, 148500u);
    wr(d, RK_HDMIRX + RX_AUDPLL_GEN_N, 5644u);
    upd(d, RK_HDMIRX + RX_AUD_CLK_CTRL, 1u << 4, 1u << 4);
    upd(d, RK_HDMIRX + RX_AUD_PLL_CTRL, 0xfu << 24, 0);
    upd(d, RK_HDMIRX + RX_AUD_FIFO_TH, 0x7ffffffu, 64u << 18 | 8u << 9 | 8u);
    upd(d, RK_HDMIRX + RX_AUD_FIFO_CTRL, 0xfu << 24 | 1u << 16, 1u << 16);
    wr(d, RK_HDMIRX + RX_AUD_SAO_CTRL, AUD_SAO_OFF);
    wr(d, RK_HDMIRX + RX_AUD_MUTE_CTRL, 0x00700488u);
    wr(d, RK_HDMIRX + RX_AUD_PAO_CTRL, 0);
    wr(d, RK_HDMIRX + RX_AUD_CHEXTR_CTRL, 1u);
    upd(d, RK_HDMIRX + RX_DMI_DISABLE_IF, DMI_AUDIO_EN, DMI_AUDIO_EN);
}

/* A register of the receiver PHY, through the controller's own I2C master. */
static void phy_write(rk628_t *d, uint8_t reg, uint16_t val)
{
    wr(d, RK_HDMIRX + RX_I2CM_PHY_ADDR, reg);
    wr(d, RK_HDMIRX + RX_I2CM_PHY_DATAO, val);
    wr(d, RK_HDMIRX + RX_I2CM_PHY_OP, 1);
}

static void rx_phy_off(rk628_t *d)
{
    wr(d, RK_GRF + GRF_RXPHY_CTRL, 0x07u);
}

/* The F/H receiver PHY: its power and reset steps through GRF 0x00f4, and fixed
 * settings, two of which follow the source (HDMI 2.0 ratio, scrambling). */
static void rx_phy_on(rk628_t *d)
{
    bool hdmi20 = false;
    for (int i = 0; i < 50 && d->err == ESP_OK; i++) {
        if (rd(d, RK_HDMIRX + RX_SCDC_REGS0) & (1u << 17)) {
            hdmi20 = true;
            break;
        }
        sleep_ms(20);
    }
    const bool scrambled = rd(d, RK_HDMIRX + RX_HDMI20_STATUS) & 1u;
    wr(d, RK_GRF + GRF_RXPHY_CTRL, 0x17u);
    esp_rom_delay_us(30);
    wr(d, RK_GRF + GRF_RXPHY_CTRL, 0x15u);
    wr(d, RK_HDMIRX + RX_PHYG3_CTRL, 0);
    wr(d, RK_HDMIRX + RX_I2CM_PHY_SS_CNTS, 0x018c01d2u);
    wr(d, RK_HDMIRX + RX_I2CM_PHY_FS_HCNT, 0x003c0081u);
    wr(d, RK_HDMIRX + RX_I2CM_PHY_MODE, 1u);
    wr(d, RK_GRF + GRF_RXPHY_CTRL, 0x11u);
    phy_write(d, 0x02, 0x1860);
    phy_write(d, 0x03, 0x0060);
    phy_write(d, 0x0d, (scrambled && !hdmi20) ? 0x00c0 : 0x0000);
    phy_write(d, 0x27, 0x1c94);
    phy_write(d, 0x28, 0x3713);
    phy_write(d, 0x29, 0x24da);
    phy_write(d, 0x2a, 0x5492);
    phy_write(d, 0x2b, 0x4b0d);
    phy_write(d, 0x2d, 0x008c);
    phy_write(d, 0x2e, 0x0001);
    phy_write(d, 0x0e, hdmi20 ? 0x0108 : 0x0008);
    wr(d, RK_GRF + GRF_RXPHY_CTRL, 0x14u);
    sleep_ms(20);
}

static bool rx_locked(rk628_t *d)
{
    return (rd(d, RK_HDMIRX + RX_SCDC_REGS1) & 0xf00u) == 0xf00u;
}

/* The TMDS clock: cycles per 1000 cycles of the 49.5 MHz measure clock. The
 * counter jitters by a count or two, so the most common of a few reads. */
static uint32_t tmds_hz(rk628_t *d)
{
    uint16_t v[9];
    for (int i = 0; i < 9; i++) {
        v[i] = (uint16_t)rd(d, RK_HDMIRX + RX_CKM_RESULT);
    }
    uint16_t best = v[0];
    int best_n = 0;
    for (int i = 0; i < 9; i++) {
        int n = 0;
        for (int j = 0; j < 9; j++) {
            n += v[j] == v[i];
        }
        if (n > best_n) {
            best_n = n;
            best = v[i];
        }
    }
    return (uint32_t)best * 49500u;
}

/* One reading of the source's mode. False when it does not hold together. */
static bool read_mode(rk628_t *d, rk_mode_t *m)
{
    memset(m, 0, sizeof(*m));
    const uint32_t tmds = tmds_hz(d);
    const uint32_t ht1 = rd(d, RK_HDMIRX + RX_MD_HT1);
    const uint32_t ht0 = rd(d, RK_HDMIRX + RX_MD_HT0) & 0xffffu;
    const uint32_t vsc = rd(d, RK_HDMIRX + RX_MD_VSC) & 0xffffu;
    const uint32_t hact = rd(d, RK_HDMIRX + RX_MD_HACT_PX) & 0xffffu;
    const uint32_t vact = rd(d, RK_HDMIRX + RX_MD_VAL) & 0xffffu;
    const uint32_t vtotal = rd(d, RK_HDMIRX + RX_MD_VTL) & 0xffffu;
    const uint32_t vbp = (rd(d, RK_HDMIRX + RX_MD_VOL) & 0xffffu) + 1u;
    const bool interlaced = rd(d, RK_HDMIRX + RX_MD_STS) & (1u << 3);
    const uint32_t avi = rd(d, RK_HDMIRX + RX_PDEC_AVI_PB);
    d->dvi = rd(d, RK_HDMIRX + RX_PDEC_STS) & (1u << 28);
    if (d->err != ESP_OK) {
        return false;
    }
    uint32_t htotal = ht1 >> 16;
    const uint32_t hofs = ht1 & 0xffffu;
    const uint32_t count = tmds / 49500u;
    uint32_t hs = (count * ht0 + 500u) / 1000u;
    const uint32_t vs_px = (count * vsc + 500u) / 1000u;
    const uint32_t vs = htotal ? (vs_px + htotal / 2u) / htotal : 0;
    /* The same checks the Linux driver makes, and interlaced is not a mode a
     * capture of progressive frames can take either. */
    if (!htotal || !vtotal || interlaced || vtotal > 3000u || hofs < hs || htotal < hact + hofs ||
        vtotal < vact + vs + vbp || !vs || (vact == 1080u && vtotal > 1500u) || !hact || !vact) {
        return false;
    }
    uint32_t hbp = hofs - hs, hfp = htotal - hact - hofs;
    uint32_t pclk = tmds;
    d->avi_y = (uint8_t)((avi >> 5) & 3u);
    if (d->avi_y == 3) {
        /* 4:2:0 is measured two pixels at a time. */
        htotal *= 2;
        hs *= 2;
        hbp *= 2;
        hfp *= 2;
        pclk *= 2;
        m->hact = (uint16_t)(hact * 2);
    } else {
        m->hact = (uint16_t)hact;
    }
    m->vact = (uint16_t)vact;
    m->htotal = (uint16_t)htotal;
    m->vtotal = (uint16_t)vtotal;
    m->hs = (uint16_t)hs;
    m->hbp = (uint16_t)hbp;
    m->hfp = (uint16_t)hfp;
    m->vs = (uint16_t)vs;
    m->vbp = (uint16_t)vbp;
    m->vfp = (uint16_t)(vtotal - vact - vs - vbp);
    m->pclk = pclk;
    return true;
}

/* The mode, once it reads the same eight times running (10 ms apart). */
static bool stable_mode(rk628_t *d, rk_mode_t *out)
{
    rk_mode_t prev = {0}, cur;
    int same = 0;
    for (int round = 0; round < 20 && d->err == ESP_OK; round++) {
        if (!read_mode(d, &cur)) {
            same = 0;
        } else if (cur.hact == prev.hact && cur.vact == prev.vact && cur.htotal == prev.htotal &&
                   cur.vtotal == prev.vtotal) {
            if (++same >= 7) {
                *out = cur;
                return true;
            }
        } else {
            same = 0;
        }
        prev = cur;
        sleep_ms(10);
    }
    return false;
}

/* ---- the output: scaler, colour, CSI, PHYs ---------------------------------------- */

/* What leaves the chip for a given source: the same mode while two lanes carry
 * it, else CEA 1080p at the source's frame rate. */
static bool choose_output(rk628_t *d)
{
    d->out = d->in;
    if (d->in.pclk <= MAX_OUT_PCLK_HZ) {
        return true;
    }
    rk_mode_t o = {.hact = 1920, .vact = 1080, .htotal = 2200, .vtotal = 1125,
                 .hs = 44, .hbp = 148, .hfp = 88, .vs = 5, .vbp = 36, .vfp = 4};
    o.pclk = (uint32_t)((uint64_t)d->in.pclk * o.vact * o.htotal / ((uint64_t)d->in.vact * d->in.htotal));
    if (o.pclk > MAX_OUT_PCLK_HZ || d->in.hact < o.hact || d->in.vact < o.vact) {
        return false;
    }
    d->out = o;
    return true;
}

static uint32_t scale_factor(uint32_t in, uint32_t out, uint32_t *mode)
{
    if (in > out) {
        *mode = 2;
        return ((in - 1) / (out - 1) > 2) ? ((in - 1) << 14) / (out - 1) : ((in - 2) << 14) / (out - 1);
    }
    if (in < out) {
        *mode = 1;
        return ((in - 1) << 16) / (out - 1);
    }
    *mode = 0;
    return 0;
}

/*
 * The scaler times the output the CSI transmitter sends, so it runs even 1:1.
 * Where the output frame starts inside the input frame is worked out in
 * picoseconds, as the Linux driver does.
 */
static void program_scaler(rk628_t *d)
{
    const rk_mode_t *i = &d->in, *o = &d->out;
    uint32_t hmode, vmode;
    const uint32_t hf = scale_factor(i->hact, o->hact, &hmode);
    const uint32_t vf = scale_factor(i->vact, o->vact, &vmode);

    const uint64_t p_in = 1000000000000ull / i->pclk, p_out = 1000000000000ull / o->pclk;
    const uint64_t bp_in = (uint64_t)(i->vbp + i->vs) * i->htotal + i->hs + i->hbp;
    const uint64_t bp_out = (uint64_t)(o->vbp + o->vs) * o->htotal + o->hs + o->hbp;
    const uint64_t t_in = bp_in * p_in, t_out = bp_out * p_out;
    const uint64_t frame = (uint64_t)i->vtotal * i->htotal * p_in;
    const uint64_t delta = ((i->vact / o->vact) <= 2 ? 5u : 12u) * (uint64_t)i->htotal * p_in;
    const uint64_t t = (t_in + delta > t_out) ? t_in + delta - t_out : frame - (t_out - (t_in + delta));
    const uint64_t n = t / p_in;
    const uint32_t hst = (uint32_t)(n % i->htotal);
    uint32_t vst = (uint32_t)(n / i->htotal);
    if (vst > i->vfp) {
        vst = i->vfp;
    }

    const uint32_t hact_st = o->hs + o->hbp, hact_end = hact_st + o->hact;
    const uint32_t vact_st = o->vs + o->vbp, vact_end = vact_st + o->vact;
    upd(d, RK_GRF + GRF_RGB_DEC_CON0, 0x1fffu << 16, (uint32_t)i->hact << 16);
    hiw(d, RK_GRF + GRF_SCALER_CON0, 0x01ff, (uint16_t)(vmode << 3 | hmode << 1 | 1u));
    wr(d, RK_GRF + GRF_SCALER_CON1, vf << 16 | (hf & 0xffffu));
    wr(d, RK_GRF + GRF_SCALER_CON2, vst << 16 | hst);
    wr(d, RK_GRF + GRF_SCALER_CON3, (uint32_t)o->hs << 16 | o->htotal);
    wr(d, RK_GRF + GRF_SCALER_CON4, hact_st << 16 | hact_end);
    wr(d, RK_GRF + GRF_SCALER_CON5, (uint32_t)o->vs << 16 | o->vtotal);
    wr(d, RK_GRF + GRF_SCALER_CON6, vact_st << 16 | vact_end);
    wr(d, RK_GRF + GRF_SCALER_CON7, hact_st << 16 | hact_end);
    wr(d, RK_GRF + GRF_SCALER_CON8, vact_st << 16 | vact_end);
}

/*
 * Whatever the source sends, to BT.709 full-range YCbCr. A 3x3 matrix in
 * 10-bit fixed point (1024 = 1.0) and an offset per row, on 10-bit codes.
 */
static void program_csc(rk628_t *d, uint32_t avi)
{
    typedef struct {
        int16_t c[9];
        int16_t in_dc[3];
    } csc_t;
    static const csc_t rgb_full = {{218, 732, 74, -117, -395, 512, 512, -465, -47}, {0, 0, 0}};
    static const csc_t rgb_limited = {{254, 855, 86, -137, -461, 598, 598, -543, -55}, {-64, -64, -64}};
    static const csc_t yuv709_limited = {{1196, 0, 0, 0, 1169, 0, 0, 0, 1169}, {-64, -512, -512}};
    static const csc_t yuv709_full = {{1024, 0, 0, 0, 1024, 0, 0, 0, 1024}, {0, -512, -512}};
    static const csc_t yuv601_limited = {{1196, -138, -249, 0, 1191, 134, 0, 88, 1199}, {-64, -512, -512}};
    static const csc_t yuv601_full = {{1024, -121, -218, 0, 1043, 117, 0, 77, 1050}, {0, -512, -512}};
    static const int16_t out_dc[3] = {0, 512, 512};

    const uint32_t q = (avi >> 18) & 3u, vic = (avi >> 24) & 0x7fu, c = (avi >> 14) & 3u, ec = (avi >> 20) & 7u;
    const csc_t *m;
    if (d->avi_y == 0 || d->dvi) {
        /* RGB: limited for a CEA mode unless the source says otherwise. */
        const bool limited = q == 1 || (q == 0 && vic != 0 && !d->dvi);
        m = limited ? &rgb_limited : &rgb_full;
    } else {
        const bool full = q == 2;
        const bool is601 = c == 1 || (c == 3 && (ec == 0 || ec == 2)); /* xvYCC601, sYCC601 */
        m = is601 ? (full ? &yuv601_full : &yuv601_limited) : (full ? &yuv709_full : &yuv709_limited);
    }
    int32_t off[3];
    for (int r = 0; r < 3; r++) {
        int32_t s = m->c[3 * r] * m->in_dc[0] + m->c[3 * r + 1] * m->in_dc[1] + m->c[3 * r + 2] * m->in_dc[2] +
                    out_dc[r] * 1024;
        off[r] = s >= 0 ? (s + 512) / 1024 : -((-s + 512) / 1024);
    }
    hiw(d, RK_GRF + GRF_CSC_CTRL, 0x0100, 0x0100);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x00, (uint32_t)(uint16_t)m->c[1] << 16 | (uint16_t)m->c[0]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x04, (uint32_t)(uint16_t)m->c[3] << 16 | (uint16_t)m->c[2]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x08, (uint32_t)(uint16_t)m->c[5] << 16 | (uint16_t)m->c[4]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x0c, (uint32_t)(uint16_t)m->c[7] << 16 | (uint16_t)m->c[6]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x10, (uint16_t)m->c[8]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x14, (uint32_t)off[0]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x18, (uint32_t)off[1]);
    wr(d, RK_GRF + GRF_CSC_MATRIX + 0x1c, (uint32_t)off[2]);
    hiw(d, RK_GRF + GRF_CSC_CTRL, 0x0800, 0x0800);
    hiw(d, RK_GRF + GRF_SCALER_CON0, 0x0001, 0x0001);
}

/* One D-PHY test register: the code on a falling test clock with enable high,
 * the data on a rising one. */
static void dphy_test(rk628_t *d, uint32_t con, uint8_t code, uint8_t data)
{
    upd(d, RK_GRF + con, TX0_TEST_CLK, TX0_TEST_CLK);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + con, TX0_TEST_DIN_MASK, code);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + con, TX0_TEST_EN, TX0_TEST_EN);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + con, TX0_TEST_CLK, 0);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + con, TX0_TEST_EN, 0);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + con, TX0_TEST_DIN_MASK, data);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + con, TX0_TEST_CLK, TX0_TEST_CLK);
    esp_rom_delay_us(1);
}

static void txphy_off(rk628_t *d)
{
    upd(d, RK_TXPHY + TXPHY_CON0,
        TXPHY_IDLE_MASK | TXPHY_PD_MASK | TXPHY_PLL_PD | TXPHY_MODULE_A | TXPHY_MODULE_B,
        TXPHY_IDLE_MASK | TXPHY_PD_MASK | TXPHY_PLL_PD);
}

/* The MIPI D-PHY with the hand-set timings the Linux driver uses at 1300 Mbit/s
 * on PHY 0, then the combo PHY's PLL that clocks it. */
static void phys_on(rk628_t *d)
{
    static const uint8_t timing[][2] = {
        {0x70, 0x4b}, {0x71, 0xd3}, {0x72, 0x50}, {0x73, 0xdb}, {0x60, 0x4b},
        {0x61, 0xc3}, {0x62, 0x6c}, {0x63, 0xd0}, {0x65, 0x1f},
    };
    const uint32_t lanes_en = ((1u << (LANES + 1)) - 1u) << 3;
    wr(d, RK_CSI0 + CSI_SYS_CTRL0, 1);
    wr(d, RK_CSI1 + CSI_SYS_CTRL0, 1);
    upd(d, RK_CSI0 + CSI_DPHY_CTRL, 0x1fu << 3, 0);
    upd(d, RK_CSI1 + CSI_DPHY_CTRL, 0x1fu << 3, 0);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + GRF_MIPI_TX0_CON, TX0_SHUTDOWNZ, 0);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + GRF_MIPI_TX0_CON, TX0_RSTZ, 0);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + GRF_MIPI_TX0_CON, TX0_TEST_CLR, TX0_TEST_CLR);
    upd(d, RK_GRF + GRF_MIPI_TX1_CON, TX0_TEST_CLR, TX0_TEST_CLR);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + GRF_MIPI_TX0_CON, TX0_FORCE_STOP_MASK | TX0_FORCE_RX_MASK, 0);
    upd(d, RK_GRF + GRF_MIPI_TX1_CON, TX0_FORCE_STOP_MASK | TX0_FORCE_RX_MASK, 0);
    esp_rom_delay_us(1);
    upd(d, RK_GRF + GRF_MIPI_TX0_CON, TX0_TEST_CLR, 0);
    upd(d, RK_GRF + GRF_MIPI_TX1_CON, TX0_TEST_CLR, 0);
    upd(d, RK_CSI0 + CSI_DPHY_CTRL, 0x1fu << 3, lanes_en);
    upd(d, RK_CSI1 + CSI_DPHY_CTRL, 0x1fu << 3, lanes_en);

    dphy_test(d, GRF_MIPI_TX0_CON, 0x44, DPHY_HSFREQ_1300);
    dphy_test(d, GRF_MIPI_TX1_CON, 0x44, DPHY_HSFREQ_1300);
    for (size_t i = 0; i < sizeof(timing) / sizeof(timing[0]); i++) {
        dphy_test(d, GRF_MIPI_TX0_CON, timing[i][0], timing[i][1]);
        sleep_ms(2);
    }
    /* PHY 1 on automatic timing, as the Linux driver leaves it in single mode. */
    for (size_t i = 0; i < sizeof(timing) / sizeof(timing[0]); i++) {
        dphy_test(d, GRF_MIPI_TX1_CON, timing[i][0], 0);
    }
    upd(d, RK_GRF + GRF_MIPI_TX0_CON, TX0_SHUTDOWNZ | TX0_RSTZ, TX0_SHUTDOWNZ | TX0_RSTZ);
    wr(d, RK_CSI0 + CSI_SYS_CTRL0, 0);
    wr(d, RK_CSI1 + CSI_SYS_CTRL0, 0);
    sleep_ms(12);

    reset_pulse(d, CRU_SOFTRST_CON02, RST2_TXPHY);
    upd(d, RK_TXPHY + TXPHY_CON0, TXPHY_IDLE_MASK | TXPHY_PD_MASK | TXPHY_PLL_PD,
        TXPHY_IDLE_MASK | TXPHY_PD_MASK | TXPHY_PLL_PD);
    upd(d, RK_GRF + GRF_POST_PROC_CON, 0x3u << 5, 0);
    upd(d, RK_TXPHY + TXPHY_CON0, TXPHY_WIDTH_MASK | TXPHY_MIPI_EN | TXPHY_GVI_LVDS_EN,
        TXPHY_WIDTH_8 | TXPHY_MIPI_EN);
    upd(d, RK_TXPHY + TXPHY_CON0, TXPHY_MODULE_A | TXPHY_MODULE_B, TXPHY_MODULE_A | TXPHY_MODULE_B);
    wr(d, RK_TXPHY + TXPHY_CON5, TXPHY_CON5_1296);
    upd(d, RK_TXPHY + TXPHY_CON0, TXPHY_PLL_PD, 0);
    sleep_ms(1);
    upd(d, RK_TXPHY + TXPHY_CON9, 1u << 29 | 1u << 28 | 0x7u << 4, 1u << 29 | 1u << 28 | 0x7u << 4);
    sleep_ms(3);
    if (!(rd(d, RK_CSI0 + CSI_PHY_STATUS) & 1u)) {
        ESP_LOGW(TAG, "MIPI PHY PLL not locked");
    }
}

static void csi_configure(rk628_t *d, uint32_t base)
{
    const uint32_t wc = (uint32_t)d->out.hact * 2u;
    upd(d, base + CSI_CTRL,
        CSI_UV_SWAP | CSI_YUV422_EN | CSI_YUV422_MODE_MASK | CSI_P2_EN | CSI_LANES_MASK | CSI_DPHY_EN | CSI_TX_EN,
        CSI_YUV422_EN | CSI_YUV422_MODE_2 | CSI_P2_EN | (uint32_t)(LANES - 1) << 4);
    upd(d, base + CSI_SYS_CTRL1, 1u, 0);
    wr(d, base + CSI_CONFIG_DONE, 0x10);
    wr(d, base + CSI_SYS_CTRL2, 0x21);
    upd(d, base + CSI_SYS_CTRL3, 1u << 8 | 1u << 4 | 1u, 1u); /* non-continuous clock */
    wr(d, base + CSI_VOP_PATH_CTRL, wc << 16 | (uint32_t)CSI_DT_YUV422_8 << 8 | 0x0bu);
    upd(d, base + CSI_DPHY_CTRL, 0x1fu << 3, ((1u << (LANES + 1)) - 1u) << 3);
    upd(d, base + CSI_VOP_FILTER, 0xff01u, 0x0101u);
    wr(d, base + CSI_CONFIG_DONE, 0x10);
}

static void stream_off(rk628_t *d)
{
    upd(d, RK_HDMIRX + RX_DMI_DISABLE_IF, DMI_VIDEO_EN, 0);
    wr(d, RK_HDMIRX + RX_AUD_SAO_CTRL, AUD_SAO_OFF);
    const uint32_t bases[] = {RK_CSI0, RK_CSI1};
    for (int i = 0; i < 2; i++) {
        upd(d, bases[i] + CSI_CTRL, CSI_DPHY_EN | CSI_TX_EN, 0);
        upd(d, bases[i] + CSI_SYS_CTRL3, 1u << 8, 0);
        wr(d, bases[i] + CSI_CONFIG_DONE, 0x10);
    }
    txphy_off(d);
}

/* The output, for the mode in d->in: the Linux driver's stream-on order. */
static void stream_on(rk628_t *d, uint32_t avi)
{
    upd(d, RK_GRF + GRF_SYSTEM_CON3, 1u << 1, 0);   /* one D-PHY */
    upd(d, RK_GRF + GRF_POST_PROC_CON, 1u, 0);      /* no split */
    stream_off(d);
    sleep_ms(6);
    reset_pulse(d, CRU_SOFTRST_CON02, RST2_CSI);
    reset_pulse(d, CRU_SOFTRST_CON02, RST2_CSI1);
    set_pixel_clocks(d);
    program_scaler(d);
    phys_on(d);
    sleep_ms(2);
    csi_configure(d, RK_CSI0);
    csi_configure(d, RK_CSI1);
    program_csc(d, avi);

    wr(d, RK_CSI0 + CSI_SYS_CTRL0, 1);
    wr(d, RK_CSI1 + CSI_SYS_CTRL0, 1);
    wr(d, RK_CRU + CRU_SOFTRST_CON02, 1u << (RST2_TXDATA + 16) | 1u << RST2_TXDATA | 1u << (RST2_TXBYTEHS + 16) |
                                         1u << RST2_TXBYTEHS);
    sleep_ms(2);
    wr(d, RK_CRU + CRU_SOFTRST_CON02, 1u << (RST2_TXDATA + 16) | 1u << (RST2_TXBYTEHS + 16));
    wr(d, RK_CSI0 + CSI_SYS_CTRL0, 0);
    wr(d, RK_CSI1 + CSI_SYS_CTRL0, 0);
    sleep_ms(6);
    const uint32_t bases[] = {RK_CSI0, RK_CSI1};
    for (int i = 0; i < 2; i++) {
        wr(d, bases[i] + CSI_INTR_EN, 0x0fff0000u);
        upd(d, bases[i] + CSI_CTRL, CSI_DPHY_EN | CSI_TX_EN, CSI_DPHY_EN | CSI_TX_EN);
        wr(d, bases[i] + CSI_CONFIG_DONE, 0x10);
        wr(d, bases[i] + CSI_ERR_INTR_CLR, 0xffffffffu);
        wr(d, bases[i] + CSI_CONFIG_DONE, 0x10);
    }
    /* Sound with the picture: the data lines go on with the video. */
    wr(d, RK_HDMIRX + RX_AUD_SAO_CTRL, AUD_SAO_ON);
    upd(d, RK_HDMIRX + RX_DMI_DISABLE_IF, DMI_VIDEO_EN, DMI_VIDEO_EN);
}

/* ---- finding a source and following it ----------------------------------------- */

static void lose_signal(rk628_t *d, const char *why)
{
    if (d->locked) {
        ESP_LOGI(TAG, "signal lost: %s", why);
    }
    d->locked = false;
    d->err = ESP_OK;
    stream_off(d);
    rx_phy_off(d);
    d->err = ESP_OK;
}

/* Bring the receiver up and, if a source locks and holds still, the output.
 * The Linux driver's hotplug worker, with the cable always taken as present:
 * neither adapter wires the 5 V detect. */
static bool try_signal(rk628_t *d)
{
    d->err = ESP_OK;
    rx_audio_setup(d);
    rx_controller_setup(d);
    cec_restore(d);
    hpd(d, true);
    for (int attempt = 0; attempt < 3 && d->err == ESP_OK; attempt++) {
        rx_phy_on(d);
        bool locked = false;
        for (int i = 0; i < 15 && d->err == ESP_OK; i++) {
            if (rx_locked(d)) {
                locked = true;
                break;
            }
            sleep_ms(20);
        }
        if (!locked) {
            rx_phy_off(d);
            sleep_ms(50);
            continue;
        }
        /* 10-bit and 4:2:0 sources want one PHY setting changed after lock. */
        sleep_ms(25);
        const uint32_t avi = rd(d, RK_HDMIRX + RX_PDEC_AVI_PB);
        const uint32_t depth = (rd(d, RK_HDMIRX + RX_PDEC_GCP) >> 4) & 0xfu;
        phy_write(d, 0x03, (((avi >> 5) & 3u) == 3u || depth == 5u) ? 0x0000 : 0x0060);
        if (!stable_mode(d, &d->in)) {
            break;
        }
        if (!choose_output(d)) {
            ESP_LOGW(TAG, "%ux%u at %" PRIu32 " kHz does not fit two lanes, even scaled to 1080p", d->in.hact,
                     d->in.vact, d->in.pclk / 1000u);
            break;
        }
        stream_on(d, avi);
        if (d->err != ESP_OK) {
            break;
        }
        d->locked = true;
        ESP_LOGI(TAG, "source %ux%u (total %ux%u) %" PRIu32 " kHz %s, out %ux%u", d->in.hact, d->in.vact,
                 d->in.htotal, d->in.vtotal, d->in.pclk / 1000u, d->dvi ? "DVI" : "HDMI", d->out.hact, d->out.vact);
        return true;
    }
    if (d->err != ESP_OK) {
        ESP_LOGW(TAG, "I2C failed bringing the receiver up: %s", esp_err_to_name(d->err));
    }
    lose_signal(d, "no lock");
    return false;
}

/* The audio sample rate from the clock-regeneration numbers: TMDS * N / CTS / 128. */
static uint32_t audio_fs(rk628_t *d)
{
    static const uint32_t valid[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};
    const uint64_t tmds = tmds_hz(d);
    const uint32_t n = rd(d, RK_HDMIRX + RX_PDEC_ACR_N), cts = rd(d, RK_HDMIRX + RX_PDEC_ACR_CTS);
    if (!n || !cts || d->err != ESP_OK) {
        return 0;
    }
    uint32_t fs = (uint32_t)(tmds * n / cts / 128u);
    fs = (fs + 50u) / 100u * 100u;
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (fs == valid[i]) {
            return fs;
        }
    }
    return 0;
}

static void audio_follow(rk628_t *d)
{
    const uint32_t fs = audio_fs(d);
    const uint32_t fifo = rd(d, RK_HDMIRX + RX_AUD_FIFO_ISTS);
    const bool broke = fifo & (3u << 3);
    if (fs && (broke || fs > d->audio_hz + 1000u || fs + 1000u < d->audio_hz)) {
        set_audio_clock(d, fs);
        d->audio_hz = fs;
        wr(d, RK_HDMIRX + RX_AUD_FIFO_ICLR, 0x1fu);
        wr(d, RK_HDMIRX + RX_AUD_FIFO_CTRL, 0x10001u);
        wr(d, RK_HDMIRX + RX_AUD_FIFO_CTRL, 0x10000u);
    } else if (!fs) {
        d->audio_hz = 0;
    }
}

/* ---- CEC ----------------------------------------------------------------------- */

/*
 * The receiver's own CEC controller, on GPIO1_B3 (the pin mux in
 * op_init_streaming). Whether a board wires the HDMI socket's CEC pin there is
 * up to the board: the Waveshare overlay for Linux leaves CEC off.
 *
 * These are called from the CEC task while the capture side drives the chip,
 * so they use raw reads and writes and leave d->err to the capture side.
 */
static esp_err_t cec_write_addr(rk628_t *d)
{
    const uint32_t bits = d->cec_la < 15 ? (1u << d->cec_la | 1u << 15) : 0;
    esp_err_t err = raw_write(d, RK_HDMIRX + RX_CEC_ADDR_L, bits & 0xffu);
    return err == ESP_OK ? raw_write(d, RK_HDMIRX + RX_CEC_ADDR_H, bits >> 8) : err;
}

/* Clock, buffers, interrupts and address, in the order the Linux driver uses
 * after each bring-up of the receiver, which resets the block. */
static esp_err_t cec_setup(rk628_t *d)
{
    const uint32_t seq[][2] = {
        {RK_CRU + CRU_CLKSEL_CON12, frac_word(32768u, 24000000u)},
        {RK_HDMIRX + RX_CEC_MASK, 0},
        {RK_HDMIRX + RX_CEC_TX_CNT, 0},
        {RK_HDMIRX + RX_AUD_CEC_IEN_CLR, CEC_I_ALL},
        {RK_HDMIRX + RX_AUD_CEC_ICLR, CEC_I_ALL},
        {RK_HDMIRX + RX_CEC_CTRL, 0},
        {RK_HDMIRX + RX_CEC_LOCK, 0},
        {RK_HDMIRX + RX_AUD_CEC_IEN_SET, CEC_I_ALL & ~CEC_I_ARB_LOST},
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]) && err == ESP_OK; i++) {
        err = raw_write(d, seq[i][0], seq[i][1]);
    }
    uint32_t dmi = 0;
    if (err == ESP_OK) {
        err = raw_read(d, RK_HDMIRX + RX_DMI_DISABLE_IF, &dmi);
    }
    if (err == ESP_OK) {
        err = raw_write(d, RK_HDMIRX + RX_DMI_DISABLE_IF, dmi | DMI_CEC_ON);
    }
    return err == ESP_OK ? cec_write_addr(d) : err;
}

/* After a receiver reset: CEC back as it was, if it is on. */
static void cec_restore(rk628_t *d)
{
    if (d->cec_on && cec_setup(d) != ESP_OK) {
        ESP_LOGW(TAG, "CEC not restored after the receiver reset");
    }
}

static esp_err_t op_cec_enable(void *dev, bool on, uint8_t logical_addr)
{
    rk628_t *d = dev;
    d->cec_la = logical_addr;
    d->cec_on = on;
    if (on) {
        return cec_setup(d);
    }
    uint32_t dmi = 0;
    esp_err_t err = raw_write(d, RK_HDMIRX + RX_AUD_CEC_IEN_CLR, CEC_I_ALL);
    if (err == ESP_OK) {
        err = raw_read(d, RK_HDMIRX + RX_DMI_DISABLE_IF, &dmi);
    }
    return err == ESP_OK ? raw_write(d, RK_HDMIRX + RX_DMI_DISABLE_IF, dmi & ~DMI_CEC_ON) : err;
}

static esp_err_t op_cec_transmit(void *dev, const kvm_bridge_cec_frame_t *f, uint8_t free_bits)
{
    rk628_t *d = dev;
    if (!d->cec_on || f->len < 1 || f->len > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The wait before sending, in bit periods: 3 to follow our own frame, 5 as
     * a new initiator (the default), 7 to retry. */
    const uint32_t wait = free_bits == 0 ? 1u : free_bits <= 3 ? 2u : free_bits <= 5 ? 1u : 0u;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < f->len && err == ESP_OK; i++) {
        err = raw_write(d, RK_HDMIRX + RX_CEC_TX_DATA + 4u * (uint32_t)i, f->data[i]);
    }
    if (err == ESP_OK) {
        err = raw_write(d, RK_HDMIRX + RX_CEC_TX_CNT, f->len);
    }
    return err == ESP_OK ? raw_write(d, RK_HDMIRX + RX_CEC_CTRL, wait << 1 | 1u) : err;
}

static esp_err_t op_cec_poll(void *dev, kvm_bridge_cec_frame_t *rx, bool *got_rx, kvm_bridge_cec_tx_t *tx)
{
    rk628_t *d = dev;
    *got_rx = false;
    *tx = KVM_BRIDGE_CEC_TX_NONE;
    if (!d->cec_on) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t is = 0;
    ESP_RETURN_ON_ERROR(raw_read(d, RK_HDMIRX + RX_AUD_CEC_ISTS, &is), TAG, "CEC status");
    is &= CEC_I_ALL;
    if (!is) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(raw_write(d, RK_HDMIRX + RX_AUD_CEC_ICLR, is), TAG, "CEC clear");
    if (is & CEC_I_NACK) {
        *tx = KVM_BRIDGE_CEC_TX_NACK;
    } else if (is & CEC_I_ARB_LOST) {
        *tx = KVM_BRIDGE_CEC_TX_ARB_LOST;
    } else if (is & CEC_I_ERROR) {
        *tx = KVM_BRIDGE_CEC_TX_ERROR;
    } else if (is & CEC_I_TX_DONE) {
        *tx = KVM_BRIDGE_CEC_TX_OK;
    }
    if (is & CEC_I_RX_EOM) {
        uint32_t n = 0;
        esp_err_t err = raw_read(d, RK_HDMIRX + RX_CEC_RX_CNT, &n);
        n &= 0x1fu;
        if (n > 16) {
            n = 16;
        }
        for (uint32_t i = 0; i < n && err == ESP_OK; i++) {
            uint32_t v = 0;
            err = raw_read(d, RK_HDMIRX + RX_CEC_RX_DATA + 4u * i, &v);
            rx->data[i] = (uint8_t)v;
        }
        (void)raw_write(d, RK_HDMIRX + RX_CEC_LOCK, 0);
        if (err == ESP_OK && n > 0) {
            rx->len = (uint8_t)n;
            *got_rx = true;
        }
    }
    return ESP_OK;
}

/* ---- kvm_bridge operations -------------------------------------------------------- */

static esp_err_t op_init_streaming(void *dev)
{
    rk628_t *d = dev;
    d->err = ESP_OK;
    const uint32_t status = rd(d, RK_GRF + GRF_SYSTEM_STATUS0);
    ESP_LOGI(TAG, "%s mode", (status & STATUS0_I2C_ONLY) ? "I2C-only (clocks not set up by the chip!)" : "MCU + I2C");
    /* Pins: the receiver's hotplug, DDC and CEC (GPIO1_B0..B3), I2S SCK, LRCK
     * and SD0 (GPIO0_A2..A4). */
    hiw(d, RK_GRF + GRF_GPIO1AB_SEL, 0x0f00, 0x0f00);
    hiw(d, RK_GRF + GRF_GPIO0AB_SEL, 0x0004, 0x0004);
    hiw(d, RK_GRF + GRF_GPIO0AB_SEL, 0x0008, 0x0008);
    hiw(d, RK_GRF + GRF_GPIO0AB_SEL, 0x0030, 0x0010);
    esp_rom_delay_us(10);
    const int resets[] = {RST2_HDMIRX, RST2_HDMIRX_PON, RST2_CSI, RST2_CSI1};
    for (int i = 0; i < 4; i++) {
        wr(d, RK_CRU + CRU_SOFTRST_CON02, 1u << (resets[i] + 16) | 1u << resets[i]);
    }
    esp_rom_delay_us(10);
    for (int i = 0; i < 4; i++) {
        wr(d, RK_CRU + CRU_SOFTRST_CON02, 1u << (resets[i] + 16));
    }
    esp_rom_delay_us(10);
    upd(d, RK_GRF + GRF_SYSTEM_CON0,
        SYS0_INPUT_MASK | SYS0_OUTPUT_FH_MASK | SYS0_HDCP_EFUSE | SYS0_I2S_DATA_OEN | SYS0_HSYNC_POL | SYS0_VSYNC_POL,
        3u << SYS0_OUTPUT_FH_SHIFT | SYS0_HSYNC_POL | SYS0_VSYNC_POL);
    rx_controller_reset(d);
    write_edid(d);
    ESP_RETURN_ON_ERROR(d->err, TAG, "chip setup");
    cec_restore(d);
    d->ready = true;
    d->next_look_us = 0;
    return ESP_OK;
}

static esp_err_t op_set_edid_profile(void *dev, kvm_bridge_edid_profile_t p)
{
    rk628_t *d = dev;
    d->profile = p;
    if (!d->ready) {
        return ESP_OK;
    }
    d->err = ESP_OK;
    lose_signal(d, "new EDID");
    write_edid(d);
    d->next_look_us = 0;
    return d->err;
}

static esp_err_t op_set_edid_id(void *dev, const kvm_bridge_edid_id_t *id)
{
    rk628_t *d = dev;
    d->edid_id = *id;
    return d->ready ? op_set_edid_profile(dev, d->profile) : ESP_OK;
}

/* The chip only sends UYVY; the capture picks a YUV422 path for it
 * (yuv422_only), so a request for RGB888 is a bug. */
static void op_set_csi_uyvy422(void *dev, bool uyvy422)
{
    (void)dev;
    if (!uyvy422) {
        ESP_LOGE(TAG, "the capture wants RGB888, and this chip sends only UYVY (see components/rk628/README.md)");
    }
}

/* Hotplug down long enough for a source to notice, up, and look again. */
static esp_err_t op_hotplug_reset(void *dev)
{
    rk628_t *d = dev;
    d->err = ESP_OK;
    lose_signal(d, "hotplug reset");
    hpd(d, false);
    sleep_ms(500);
    hpd(d, true);
    d->next_look_us = 0;
    return d->err;
}

static esp_err_t op_reapply_csi_path(void *dev)
{
    rk628_t *d = dev;
    if (!d->locked) {
        return ESP_OK;
    }
    d->err = ESP_OK;
    stream_on(d, rd(d, RK_HDMIRX + RX_PDEC_AVI_PB));
    return d->err;
}

/*
 * Called by the capture's monitor several times a second. Without a source it
 * looks for one every two seconds (that look takes a few hundred ms of I2C);
 * with one it checks twice a second that the lock and the mode still hold.
 */
static esp_err_t op_get_timings(void *dev, kvm_bridge_timings_t *out)
{
    rk628_t *d = dev;
    const int64_t now = esp_timer_get_time();
    if (!d->ready || now < d->next_look_us) {
        *out = d->last;
        return ESP_OK;
    }
    d->next_look_us = now + (d->locked ? CHECK_EVERY_US : SEEK_EVERY_US);
    d->err = ESP_OK;
    if (!d->locked) {
        try_signal(d);
    } else {
        rk_mode_t m;
        if (!rx_locked(d)) {
            lose_signal(d, "receiver lock dropped");
        } else if (read_mode(d, &m) &&
                   (m.hact != d->in.hact || m.vact != d->in.vact || m.htotal != d->in.htotal ||
                    m.vtotal != d->in.vtotal)) {
            lose_signal(d, "mode changed");
            d->next_look_us = now + 1100 * 1000; /* the Linux driver waits 1.1 s too */
        } else {
            audio_follow(d);
        }
    }
    memset(out, 0, sizeof(*out));
    if (d->locked) {
        out->hact = d->out.hact;
        out->vact = d->out.vact;
        out->htotal = d->out.htotal;
        out->vtotal = d->out.vtotal;
        out->tmds = true;
        out->sync = true;
        out->hdmi_mode = !d->dvi;
        out->hz = (uint8_t)((d->out.pclk + (uint32_t)d->out.htotal * d->out.vtotal / 2u) /
                            ((uint32_t)d->out.htotal * d->out.vtotal));
        out->audio_hz = d->audio_hz;
    }
    d->last = *out;
    return d->err == ESP_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void op_debug_bridge(void *dev)
{
    rk628_t *d = dev;
    d->err = ESP_OK;
    const uint32_t scdc = rd(d, RK_HDMIRX + RX_SCDC_REGS1);
    const uint32_t tmds = tmds_hz(d);
    const uint32_t ht1 = rd(d, RK_HDMIRX + RX_MD_HT1), hact = rd(d, RK_HDMIRX + RX_MD_HACT_PX);
    const uint32_t vact = rd(d, RK_HDMIRX + RX_MD_VAL), vtl = rd(d, RK_HDMIRX + RX_MD_VTL);
    const uint32_t phy = rd(d, RK_CSI0 + CSI_PHY_STATUS), errs = rd(d, RK_CSI0 + CSI_ERR_RAW);
    const uint32_t sys0 = rd(d, RK_GRF + GRF_SYSTEM_CON0), avi = rd(d, RK_HDMIRX + RX_PDEC_AVI_PB);
    ESP_LOGI(TAG,
             "lock 0x%03" PRIx32 " tmds %" PRIu32 " kHz, %" PRIu32 "x%" PRIu32 " total %" PRIu32 "x%" PRIu32
             ", avi 0x%08" PRIx32 ", sys0 0x%08" PRIx32 ", csi phy 0x%02" PRIx32 " err 0x%03" PRIx32 "%s",
             scdc & 0xfffu, tmds / 1000u, hact & 0xffffu, vact & 0xffffu, ht1 >> 16, vtl & 0xffffu, avi, sys0,
             phy & 0xffu, errs & 0xfffu, d->err ? " (I2C failed)" : "");
    if (errs) {
        wr(d, RK_CSI0 + CSI_ERR_INTR_CLR, 0xffffffffu);
    }
}

static void op_remove(void *dev)
{
    rk628_t *d = dev;
    if (!d) {
        return;
    }
    d->err = ESP_OK;
    stream_off(d);
    rx_phy_off(d);
    hpd(d, false);
    if (d->i2c) {
        (void)i2c_master_bus_rm_device(d->i2c);
    }
    free(d);
}

static const kvm_bridge_ops_t s_ops = {
    .init_streaming = op_init_streaming,
    .set_edid_profile = op_set_edid_profile,
    .set_edid_id = op_set_edid_id,
    .set_csi_uyvy422 = op_set_csi_uyvy422,
    .hotplug_reset = op_hotplug_reset,
    .reapply_csi_path = op_reapply_csi_path,
    .get_timings = op_get_timings,
    .debug_status = op_debug_bridge,
    .debug_bridge = op_debug_bridge,
    .remove = op_remove,
    .cec_enable = op_cec_enable,
    .cec_transmit = op_cec_transmit,
    .cec_poll = op_cec_poll,
};

/*
 * Looking at 0x50 is safe even where an EEPROM sits there instead. The ID read
 * is one transaction: the 4 address bytes, a repeated START, then the read. An
 * EEPROM takes the first two as its address and the rest as data to write, but
 * it only starts a write on a STOP, and there is none before the read.
 */
static esp_err_t rk628_detect(i2c_master_bus_handle_t bus, kvm_bridge_t *out)
{
    if (i2c_master_probe(bus, RK628_I2C_ADDR, 100) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    rk628_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    d->cec_la = 15;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RK628_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &d->i2c);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    /* Something answers at 0x50 - an EEPROM often does. Only a chip whose
     * version register says RK628F/H is taken. */
    uint32_t id = 0;
    err = raw_read(d, RK_GRF + GRF_SOC_VERSION, &id);
    if (err != ESP_OK || id != RK628FH_ID) {
        if (err == ESP_OK && id == RK628D_ID) {
            ESP_LOGW(TAG, "RK628D found: only the F/H generation is supported (its receiver PHY differs)");
        }
        (void)i2c_master_bus_rm_device(d->i2c);
        free(d);
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t ver = 0;
    (void)raw_read(d, RK_CSI0 + CSI_VERSION, &ver);
    ESP_LOGI(TAG, "RK628F/H (0x%08" PRIx32 "), CSI TX version 0x%08" PRIx32 " - this driver is untested on hardware",
             id, ver);
    out->name = "RK628H";
    out->ops = &s_ops;
    out->dev = d;
    out->knows_ddc5v = false; /* no 5 V detect wired on the adapters */
    out->i2s_audio = true;
    out->lane_mbps = LANE_MBPS;
    out->yuv422_only = true; /* rev 1.x: MJPEG only, see capture_pixfmt.c */
    return ESP_OK;
}

KVM_BRIDGE_DRIVER(rk628, rk628_detect)
