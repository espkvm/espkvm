/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * GC9A01 240x240 round colour LCD over SPI - e.g. the Waveshare 1.28" module.
 * Unlike the I2C OLEDs it cannot share the capture bus, so it takes its own SPI
 * pins (Kconfig). Its render() draws a colour, centre-weighted layout with a
 * status ring near the rim - showing what a round panel can do that a rectangular
 * one cannot - proving the driver interface handles non-rectangular displays.
 *
 * The panel controller itself is driven through the battle-tested
 * espressif/esp_lcd_gc9a01 component, so only the composition lives here.
 */
#include "sdkconfig.h"

#if CONFIG_KVM_ENABLE_DISPLAY

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "font5x7.h"
#include "icons.h"
#include "kvm_display_driver.h"
#include "logo.h"
#include "kvm_settings.h"

#define TAG "gc9a01"

#define LCD_W 240
#define LCD_H 240
#define LCD_CENTER 120
#define FB_PIXELS (LCD_W * LCD_H)

#define STATUS_SCREENS 3
#define SCREEN_DWELL_TICKS 4
#define SPLASH_TICKS 4 /* first half: version card, second half: espkvm logo */

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    uint16_t *fb; /* RGB565, byte-swapped for the panel, in PSRAM */
    int bl;       /* backlight GPIO, -1 if tied to 3V3 */
    uint8_t screen;
    uint8_t tick;
    uint8_t splash;
} gc9a01_ctx_t;

/* RGB565, byte-swapped so the buffer can go straight to the panel (which wants
 * the high byte first). */
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

static void put_px(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) {
        fb[y * LCD_W + x] = c;
    }
}

static void fill(uint16_t *fb, uint16_t c)
{
    for (int i = 0; i < FB_PIXELS; i++) {
        fb[i] = c;
    }
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put_px(fb, x + i, y + j, c);
        }
    }
}

/* A rounded-ish progress bar: track @p bg, fill @p fg to @p frac (0..1). */
static void bar(uint16_t *fb, int x, int y, int w, int h, float frac, uint16_t fg, uint16_t bg)
{
    fill_rect(fb, x, y, w, h, bg);
    if (frac < 0) {
        frac = 0;
    } else if (frac > 1) {
        frac = 1;
    }
    fill_rect(fb, x, y, (int)(w * frac + 0.5f), h, fg);
}

/* A thick ring near the rim, in the given colour - a status halo around the data. */
static void ring(uint16_t *fb, int outer, int thick, uint16_t c)
{
    const int inner = outer - thick;
    const int o2 = outer * outer, i2 = inner * inner;
    for (int y = LCD_CENTER - outer; y <= LCD_CENTER + outer; y++) {
        for (int x = LCD_CENTER - outer; x <= LCD_CENTER + outer; x++) {
            const int dx = x - LCD_CENTER, dy = y - LCD_CENTER;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= o2 && d2 >= i2) {
                put_px(fb, x, y, c);
            }
        }
    }
}

/* One 5x7 glyph scaled by @p s, top-left at (x, y). */
static void draw_char(uint16_t *fb, int x, int y, char ch, int s, uint16_t c)
{
    const uint8_t *g = font5x7[(ch < 0x20 || ch > 0x7F) ? 0 : (ch - 0x20)];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) {
                for (int a = 0; a < s; a++) {
                    for (int b = 0; b < s; b++) {
                        put_px(fb, x + col * s + a, y + row * s + b, c);
                    }
                }
            }
        }
    }
}

/* A horizontally-centred string at row @p y, scale @p s. */
static void text_centered(uint16_t *fb, int y, const char *str, int s, uint16_t c)
{
    const int glyph = 6 * s; /* 5 columns + 1 gap */
    const int w = (int)strlen(str) * glyph;
    int x = LCD_CENTER - w / 2;
    for (const char *p = str; *p; p++, x += glyph) {
        draw_char(fb, x, y, *p, s, c);
    }
}

/* An 8x8 icon (column-major, bit 0 = top) scaled by @p s, top-left (x, y), tint c. */
static void draw_icon(uint16_t *fb, int x, int y, const uint8_t g[8], int s, uint16_t c)
{
    for (int col = 0; col < 8; col++) {
        for (int row = 0; row < 8; row++) {
            if (g[col] & (1u << row)) {
                for (int a = 0; a < s; a++) {
                    for (int b = 0; b < s; b++) {
                        put_px(fb, x + col * s + a, y + row * s + b, c);
                    }
                }
            }
        }
    }
}

static void render_splash(gc9a01_ctx_t *g, const kvm_display_status_t *st)
{
    fill(g->fb, rgb(8, 10, 14));
    ring(g->fb, 118, 6, rgb(90, 200, 235));
    if (g->splash > SPLASH_TICKS / 2) {
        /* Version card first. */
        text_centered(g->fb, 96, "ESP-KVM", 3, rgb(235, 238, 245));
        text_centered(g->fb, 138, st->version, 2, rgb(140, 150, 165));
    } else {
        /* Then the ESP-KVM logo from icon.svg: light ESP/KVM pixels on the dark
         * face, framed by the ring - the icon's dark tile and light wordmark. */
        const int s = 9;
        const int lw = LOGO_COLS * s, lh = LOGO_ROWS * s;
        const int x0 = (LCD_W - lw) / 2, y0 = (LCD_H - lh) / 2;
        const uint16_t fg = rgb(230, 237, 243);
        for (int row = 0; row < LOGO_ROWS; row++) {
            for (int col = 0; col < LOGO_COLS; col++) {
                if (logo_px(col, row)) {
                    fill_rect(g->fb, x0 + col * s, y0 + row * s, s, s, fg);
                }
            }
        }
    }
}

static void render_status(gc9a01_ctx_t *g, const kvm_display_status_t *st)
{
    const uint16_t bg = rgb(8, 10, 14);
    const uint16_t white = rgb(235, 238, 245);
    const uint16_t grey = rgb(140, 150, 165);
    const uint16_t cyan = rgb(90, 200, 235);
    const uint16_t green = rgb(90, 210, 120);
    const uint16_t amber = rgb(235, 190, 70);
    const uint16_t red = rgb(235, 100, 90);
    char buf[40];

    fill(g->fb, bg);

    /* Status halo: green when a target signal is up, amber when linked but no
     * signal, red when there is no network at all. */
    uint16_t halo = st->ip[0] ? (st->video_signal ? green : amber) : red;
    ring(g->fb, 118, 8, halo);

    switch (g->screen) {
    case 0: /* network */
        draw_icon(g->fb, 108, 36, icon_net, 3, cyan);
        text_centered(g->fb, 66, st->ap_mode ? "AP RESCUE" : "NETWORK", 2, cyan);
        text_centered(g->fb, 104, st->ip[0] ? st->ip : "no link", 3, white);
        if (st->ap_mode) {
            text_centered(g->fb, 150, "192.168.4.1", 2, grey);
        } else {
            snprintf(buf, sizeof(buf), "%s.local", st->hostname);
            text_centered(g->fb, 150, buf, 2, grey);
            if (st->ts_ip[0]) { /* also reachable over the tailnet */
                snprintf(buf, sizeof(buf), "TS %s", st->ts_ip);
                text_centered(g->fb, 176, buf, 2, green);
            }
        }
        break;
    case 1: /* video */
        draw_icon(g->fb, 108, 36, icon_video, 3, cyan);
        text_centered(g->fb, 66, "VIDEO", 2, cyan);
        if (st->video_signal) {
            snprintf(buf, sizeof(buf), "%ux%u", (unsigned)st->hres, (unsigned)st->vres);
            text_centered(g->fb, 104, buf, 3, white);
            snprintf(buf, sizeof(buf), "%u fps  %s", (unsigned)st->fps, st->codec);
            text_centered(g->fb, 150, buf, 2, grey);
        } else {
            text_centered(g->fb, 110, "NO SIGNAL", 3, red);
        }
        break;
    default: /* health */
        draw_icon(g->fb, 108, 36, icon_health, 3, cyan);
        text_centered(g->fb, 66, "HEALTH", 2, cyan);
        snprintf(buf, sizeof(buf), "Temp  %d C", st->temp_c);
        text_centered(g->fb, 98, buf, 2, white);
        bar(g->fb, 55, 118, 130, 12, st->temp_c / 90.0f,
            st->temp_c < 60 ? green : st->temp_c < 75 ? amber : red, rgb(38, 42, 50));
        {
            const float ram = st->psram_total_kb
                                  ? 1.0f - (float)st->psram_kb / (float)st->psram_total_kb
                                  : 0.0f;
            snprintf(buf, sizeof(buf), "RAM  %d%%", (int)(ram * 100 + 0.5f));
            text_centered(g->fb, 142, buf, 2, white);
            bar(g->fb, 55, 162, 130, 12, ram, cyan, rgb(38, 42, 50));
        }
        break;
    }
}

static esp_err_t attach(void **ctx)
{
    /* Pins come from settings so they can be changed without a rebuild. */
    const int sclk = (int)kvm_setting_int("disp_sclk");
    const int mosi = (int)kvm_setting_int("disp_mosi");
    const int cs = (int)kvm_setting_int("disp_cs");
    const int dc = (int)kvm_setting_int("disp_dc");
    const int rst = (int)kvm_setting_int("disp_rst");
    const int bl = (int)kvm_setting_int("disp_bl");
    if (sclk < 0 || mosi < 0 || cs < 0 || dc < 0) {
        ESP_LOGW(TAG, "GC9A01 needs SCLK, MOSI, CS and DC pins set");
        return ESP_ERR_INVALID_ARG;
    }

    gc9a01_ctx_t *g = calloc(1, sizeof(*g));
    if (!g) {
        return ESP_ERR_NO_MEM;
    }
    g->bl = bl;
    g->splash = SPLASH_TICKS;
    g->fb = heap_caps_malloc(FB_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!g->fb) {
        free(g);
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = sclk,
        .mosi_io_num = mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_PIXELS * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(CONFIG_KVM_GC9A01_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE /* already initialised */) {
        goto fail_fb;
    }

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = dc,
        .cs_gpio_num = cs,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_KVM_GC9A01_SPI_HOST, &io_cfg,
                                   &g->io);
    if (err != ESP_OK) {
        goto fail_fb;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_gc9a01(g->io, &panel_cfg, &g->panel);
    if (err != ESP_OK) {
        goto fail_io;
    }
    esp_lcd_panel_reset(g->panel);
    esp_lcd_panel_init(g->panel);
    esp_lcd_panel_invert_color(g->panel, true); /* GC9A01 panels want inversion on */
    esp_lcd_panel_mirror(g->panel, true, false); /* the module's default is X-mirrored */
    esp_lcd_panel_disp_on_off(g->panel, true);

    if (bl >= 0) {
        gpio_set_direction(bl, GPIO_MODE_OUTPUT);
        gpio_set_level(bl, 1);
    }

    ESP_LOGI(TAG, "GC9A01 up on SPI%d (sclk %d mosi %d cs %d dc %d rst %d)",
             CONFIG_KVM_GC9A01_SPI_HOST, sclk, mosi, cs, dc, rst);
    *ctx = g;
    return ESP_OK;

fail_io:
    esp_lcd_panel_io_del(g->io);
fail_fb:
    free(g->fb);
    free(g);
    return err == ESP_OK ? ESP_FAIL : err;
}

static esp_err_t render(void *ctx, const kvm_display_status_t *status)
{
    gc9a01_ctx_t *g = ctx;
    if (g->splash > 0) {
        g->splash--;
        render_splash(g, status);
        return esp_lcd_panel_draw_bitmap(g->panel, 0, 0, LCD_W, LCD_H, g->fb);
    }
    render_status(g, status);
    esp_err_t err = esp_lcd_panel_draw_bitmap(g->panel, 0, 0, LCD_W, LCD_H, g->fb);
    if (++g->tick >= SCREEN_DWELL_TICKS) {
        g->tick = 0;
        g->screen = (uint8_t)((g->screen + 1) % STATUS_SCREENS);
    }
    return err;
}

static void detach(void *ctx)
{
    gc9a01_ctx_t *g = ctx;
    if (!g) {
        return;
    }
    if (g->panel) {
        esp_lcd_panel_disp_on_off(g->panel, false);
        esp_lcd_panel_del(g->panel);
    }
    if (g->io) {
        esp_lcd_panel_io_del(g->io);
    }
    (void)spi_bus_free(CONFIG_KVM_GC9A01_SPI_HOST);
    if (g->bl >= 0) {
        gpio_set_level(g->bl, 0);
    }
    free(g->fb);
    free(g);
}

const kvm_display_driver_t kvm_display_gc9a01 = {
    .name = "gc9a01",
    .label = "GC9A01 240x240 round",
    .attach = attach,
    .render = render,
    .detach = detach,
};

#endif /* CONFIG_KVM_ENABLE_DISPLAY */
