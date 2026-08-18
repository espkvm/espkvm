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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
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
#include "qrcode.h"

#define TAG "gc9a01"

#define LCD_W 240
#define LCD_H 240
#define LCD_CENTER 120
#define FB_PIXELS (LCD_W * LCD_H)

#define STATUS_SCREENS 3
#define SCREEN_DWELL_TICKS 4
#define SPLASH_TICKS 4 /* first half: version card, second half: espkvm logo */
/* Rows per SPI transfer; see flush() for why the frame is not sent in one go. */
#define FLUSH_BAND_ROWS 24

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

/*
 * The same ring drawn as a gauge: filled @p fg clockwise from twelve o'clock to
 * @p frac of a turn, the rest left as @p bg.
 *
 * This is the one thing a round panel does better than a rectangular one - a
 * hold-to-confirm gesture has a natural home on the rim, where it reads as
 * "keep going" from across a room without a single word.
 */
static void arc(uint16_t *fb, int outer, int thick, float frac, uint16_t fg, uint16_t bg)
{
    const int inner = outer - thick;
    const int o2 = outer * outer, i2 = inner * inner;
    for (int y = LCD_CENTER - outer; y <= LCD_CENTER + outer; y++) {
        for (int x = LCD_CENTER - outer; x <= LCD_CENTER + outer; x++) {
            const int dx = x - LCD_CENTER, dy = y - LCD_CENTER;
            const int d2 = dx * dx + dy * dy;
            if (d2 > o2 || d2 < i2) {
                continue;
            }
            /* atan2(dx, -dy): zero straight up, growing clockwise, so the sweep
             * matches how a progress dial is read. */
            float t = atan2f((float)dx, (float)-dy) / (2.0f * (float)M_PI);
            if (t < 0) {
                t += 1.0f;
            }
            put_px(fb, x, y, t <= frac ? fg : bg);
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

/*
 * The largest scale at which @p str fits on row @p y, down to 1.
 *
 * Two things make a fixed scale wrong here. The face is round, so the usable
 * width is a chord rather than the full 240 px - a row near the rim is much
 * narrower than one across the middle. And the strings vary: an IPv4 address
 * with three-digit octets is 15 characters, which at scale 3 wants 270 px, and
 * "<hostname>.local" can be far longer still. Overflow was invisible rather than
 * ugly, because put_px() clips - the ends simply disappeared off both edges.
 */
#define TEXT_R 108 /* usable radius, inside the status ring */

static int text_scale_for(int y, const char *str, int max_s)
{
    const int len = (int)strlen(str);
    if (len <= 0) {
        return max_s;
    }
    for (int s = max_s; s > 1; s--) {
        /* The corner of the text box furthest from the centre row decides the
         * chord: a tall line sitting low is limited by its bottom edge. */
        int top = y - LCD_CENTER, bot = y + 7 * s - LCD_CENTER;
        if (top < 0) top = -top;
        if (bot < 0) bot = -bot;
        const int dy = top > bot ? top : bot;
        if (dy >= TEXT_R) {
            continue;
        }
        int half = TEXT_R; /* integer sqrt of TEXT_R^2 - dy^2 */
        while (half > 0 && half * half > TEXT_R * TEXT_R - dy * dy) {
            half--;
        }
        if (len * 6 * s <= 2 * half) {
            return s;
        }
    }
    return 1;
}

/* Centred text that shrinks to fit the round face rather than running off it. */
static void text_fit(uint16_t *fb, int y, const char *str, int max_s, uint16_t c)
{
    text_centered(fb, y, str, text_scale_for(y, str, max_s), c);
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

/*
 * The rescue hotspot's join code.
 *
 * A QR has to be dark-on-light - plenty of readers refuse an inverted one - so on
 * this dark face the symbol gets its own white tile, quiet zone included. That
 * four-module margin is part of the symbol rather than decoration: without it a
 * reader cannot find the edges.
 *
 * Everything is sized in whole pixels per module. A fractional scale would put
 * module boundaries mid-pixel, and that blurred edge is exactly what makes a
 * small code unscannable.
 */
#define QR_QUIET 4
/* The panel is round: the largest square inside a 240 px circle is about 169 px,
 * and the caption below needs room too, so the symbol gets 140 - which keeps its
 * corners well inside the glass. */
#define QR_BOX 140
#define QR_TOP 34

typedef struct {
    uint16_t *fb;
    int side; /**< pixels actually drawn, 0 if it would not fit */
} qr_paint_t;

static void qr_paint(esp_qrcode_handle_t qr, void *user_data)
{
    qr_paint_t *p = (qr_paint_t *)user_data;
    const int modules = esp_qrcode_get_size(qr);
    const int total = modules + 2 * QR_QUIET;
    const int scale = QR_BOX / total;
    if (scale < 1) {
        return; /* denser than this panel can show; caller falls back to text */
    }

    const int side = total * scale;
    const int x0 = (LCD_W - side) / 2;
    fill_rect(p->fb, x0, QR_TOP, side, side, rgb(255, 255, 255));

    const uint16_t dark = rgb(0, 0, 0);
    for (int y = 0; y < modules; y++) {
        for (int x = 0; x < modules; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {
                fill_rect(p->fb, x0 + (QR_QUIET + x) * scale, QR_TOP + (QR_QUIET + y) * scale,
                          scale, scale, dark);
            }
        }
    }
    p->side = side;
}

/** Draw @p payload as a QR code. Returns the side length drawn, or 0 on failure. */
static int draw_join_qr(uint16_t *fb, const char *payload)
{
    qr_paint_t paint = {.fb = fb, .side = 0};
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func_with_cb = qr_paint;
    cfg.user_data = &paint; /* non-NULL is what selects the callback form */
    /*
     * Version 6 is 41x41 modules - three pixels each here - and holds far more
     * than the longest hotspot string a 63-character passphrase can produce.
     * Allowing more would encode fine and scan badly, and the encoder's working
     * buffer grows with the cap.
     */
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

    return esp_qrcode_generate(&cfg, payload) == ESP_OK ? paint.side : 0;
}

/*
 * A notice: one thing, said large, with the rim as its progress gauge.
 *
 * Nothing else is drawn - no status ring, no rotation. Someone reading this is
 * standing at the device with a finger on the button, and the panel's whole job
 * for those two seconds is to answer "is it working?".
 */
static void render_notice(gc9a01_ctx_t *g, const kvm_display_status_t *st)
{
    const uint16_t bg = rgb(8, 10, 14);
    const uint16_t cyan = rgb(90, 200, 235);
    const uint16_t white = rgb(235, 238, 245);
    const uint16_t grey = rgb(140, 150, 165);

    fill(g->fb, bg);
    if (st->notice_pct >= 0) {
        arc(g->fb, 118, 10, st->notice_pct / 100.0f, cyan, rgb(38, 44, 54));
    } else {
        ring(g->fb, 118, 10, cyan);
    }
    text_fit(g->fb, 96, st->notice, 4, white);
    text_fit(g->fb, 142, st->notice_detail, 2, grey);
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
        /*
         * In hotspot mode the address is always 192.168.4.1 and the interesting
         * thing is joining the network at all - so hand over the whole face to a
         * code the phone can scan, rather than a passphrase to squint at and
         * retype. Falls through to the text layout if the code will not fit.
         */
        if (st->ap_mode && st->join_qr[0]) {
            const int side = draw_join_qr(g->fb, st->join_qr);
            if (side > 0) {
                text_centered(g->fb, QR_TOP + side + 8, "SCAN TO JOIN", 2, cyan);
                text_fit(g->fb, QR_TOP + side + 28, st->ssid, 1, grey);
                break;
            }
        }
        draw_icon(g->fb, 108, 36, icon_net, 3, cyan);
        text_centered(g->fb, 66, st->ap_mode ? "AP RESCUE" : "NETWORK", 2, cyan);
        text_fit(g->fb, 104, st->ip[0] ? st->ip : "no link", 3, white);
        if (st->ap_mode) {
            text_centered(g->fb, 150, "192.168.4.1", 2, grey);
        } else {
            snprintf(buf, sizeof(buf), "%s.local", st->hostname);
            text_fit(g->fb, 150, buf, 2, grey);
            if (st->ts_ip[0]) { /* also reachable over the tailnet */
                snprintf(buf, sizeof(buf), "TS %s", st->ts_ip);
                text_fit(g->fb, 176, buf, 2, green);
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
    /* Cache-aligned, or the SPI driver cannot DMA out of PSRAM and falls back to
     * a private internal copy of the whole frame - see flush(). */
    size_t align = 64;
    (void)esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align);
    if (align == 0) {
        align = 64;
    }
    const size_t fb_bytes = ((FB_PIXELS * sizeof(uint16_t)) + align - 1) / align * align;
    g->fb = heap_caps_aligned_alloc(align, fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
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
        /* One band, not one frame: the bus sizes its DMA descriptors from this. */
        .max_transfer_sz = LCD_W * FLUSH_BAND_ROWS * sizeof(uint16_t),
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

/*
 * Push the framebuffer to the panel in horizontal bands.
 *
 * Sending all 115 KB in one transfer asks the SPI driver for a DMA descriptor
 * chain over the whole frame, and if it cannot use the buffer directly it
 * allocates a private copy of that size in INTERNAL memory - of which this chip
 * has about half a megabyte in total, shared with TLS sessions, USB, lwIP, the
 * SD card and the H.264 encoder's own buffers. It loses that race, and then
 * everything downwind of it loses too: the panel logs "send color failed" every
 * second and the video encoder cannot get its reference frame.
 *
 * A band is a few kilobytes, so the worst case stops being fatal. The buffer is
 * also cache-aligned now, which is what lets the DMA read it in place and skip
 * the copy entirely - the bands are the belt to that pair of braces.
 */
static esp_err_t flush(gc9a01_ctx_t *g)
{
    for (int y = 0; y < LCD_H; y += FLUSH_BAND_ROWS) {
        const int rows = (y + FLUSH_BAND_ROWS <= LCD_H) ? FLUSH_BAND_ROWS : (LCD_H - y);
        const esp_err_t err =
            esp_lcd_panel_draw_bitmap(g->panel, 0, y, LCD_W, y + rows, g->fb + (size_t)y * LCD_W);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t render(void *ctx, const kvm_display_status_t *status)
{
    gc9a01_ctx_t *g = ctx;
    if (status->notice[0]) {
        /* A notice arriving mid-splash ends the splash: by the time it clears,
         * an animation about having just booted is no longer news. */
        g->splash = 0;
        render_notice(g, status);
        return flush(g);
    }
    if (g->splash > 0) {
        g->splash--;
        render_splash(g, status);
        return flush(g);
    }
    render_status(g, status);
    esp_err_t err = flush(g);
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
