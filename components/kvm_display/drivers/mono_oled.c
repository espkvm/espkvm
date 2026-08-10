/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mono_oled.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "capture.h" /* capture_i2c_bus() - the shared bus */
#include "font5x7.h"
#include "icons.h"
#include "logo.h"

#define TAG "mono_oled"

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H / 8)
#define OLED_FB_BYTES (OLED_W * OLED_PAGES)
#define I2C_TIMEOUT_MS 100

/* Status screens this helper cycles through, and how many render ticks each is
 * shown for (the core ticks about once a second). */
#define STATUS_SCREENS 3
#define SCREEN_DWELL_TICKS 4

struct mono_oled {
    i2c_master_dev_handle_t dev;
    uint8_t col_offset;
    uint8_t screen; /* which status screen is showing */
    uint8_t tick;   /* ticks the current screen has been up */
    uint8_t splash; /* ticks of the boot logo still to show */
    uint8_t fb[OLED_FB_BYTES];
};

/* Ticks the boot screens show (the core ticks ~once a second): the first half is
 * the version card, the second half the espkvm logo. */
#define SPLASH_TICKS 4

static esp_err_t send_cmds(i2c_master_dev_handle_t dev, const uint8_t *cmds, size_t n)
{
    uint8_t buf[48];
    if (n + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 0x00; /* Co=0, D/C#=0: the rest of the stream is commands */
    memcpy(buf + 1, cmds, n);
    return i2c_master_transmit(dev, buf, n + 1, I2C_TIMEOUT_MS);
}

static esp_err_t send_page(i2c_master_dev_handle_t dev, const uint8_t *row)
{
    uint8_t buf[1 + OLED_W];
    buf[0] = 0x40; /* D/C#=1: pixel data follows */
    memcpy(buf + 1, row, OLED_W);
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static void fb_char(mono_oled_t *m, int x, int page, char c)
{
    if (x < 0 || x > OLED_W - 5 || page < 0 || page >= OLED_PAGES) {
        return;
    }
    const uint8_t *g = font5x7[(c < 0x20 || c > 0x7F) ? 0 : (c - 0x20)];
    for (int i = 0; i < 5; i++) {
        m->fb[page * OLED_W + x + i] = g[i];
    }
}

static void fb_text_at(mono_oled_t *m, int x, int page, const char *s)
{
    for (; *s && x <= OLED_W - 5; s++, x += 6) {
        fb_char(m, x, page, *s);
    }
}

static void fb_text(mono_oled_t *m, int page, const char *s)
{
    fb_text_at(m, 0, page, s);
}

/* Blit an 8x8 icon (column-major, like a glyph) at column x, page row. */
static void fb_icon(mono_oled_t *m, int x, int page, const uint8_t g[8])
{
    if (page < 0 || page >= OLED_PAGES) {
        return;
    }
    for (int i = 0; i < 8 && x + i < OLED_W; i++) {
        m->fb[page * OLED_W + x + i] = g[i];
    }
}

/* Set one pixel - for scaled text (the splash), which is not page-aligned. */
static void fb_px(mono_oled_t *m, int x, int y)
{
    if (x >= 0 && x < OLED_W && y >= 0 && y < OLED_H) {
        m->fb[(y / 8) * OLED_W + x] |= (uint8_t)(1u << (y % 8));
    }
}

/* A 1x string centred on a page row. */
static void fb_text_center(mono_oled_t *m, int page, const char *s)
{
    int x = (OLED_W - (int)strlen(s) * 6) / 2;
    fb_text_at(m, x < 0 ? 0 : x, page, s);
}

/* A bordered progress bar at (x, y) in pixels, filled to @p frac (0..1). */
static void fb_bar(mono_oled_t *m, int x, int y, int w, int h, float frac)
{
    for (int i = 0; i < w; i++) {
        fb_px(m, x + i, y);
        fb_px(m, x + i, y + h - 1);
    }
    for (int j = 0; j < h; j++) {
        fb_px(m, x, y + j);
        fb_px(m, x + w - 1, y + j);
    }
    if (frac < 0) {
        frac = 0;
    } else if (frac > 1) {
        frac = 1;
    }
    const int fw = (int)((w - 2) * frac + 0.5f);
    for (int i = 0; i < fw; i++) {
        for (int j = 2; j < h - 2; j++) {
            fb_px(m, x + 2 + i, y + j);
        }
    }
}

/* Set (on=1) or clear (on=0) one pixel - the logo cuts dark letters out of a
 * white square. */
static void fb_px_set(mono_oled_t *m, int x, int y, int on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return;
    }
    const int idx = (y / 8) * OLED_W + x;
    const uint8_t bit = (uint8_t)(1u << (y % 8));
    if (on) {
        m->fb[idx] |= bit;
    } else {
        m->fb[idx] &= (uint8_t)~bit;
    }
}

/* The ESP-KVM logo from icon.svg: light ESP/KVM pixels, framed as a tile. */
static void draw_logo(mono_oled_t *m)
{
    const int s = 3;
    const int lw = LOGO_COLS * s, lh = LOGO_ROWS * s;
    const int x0 = (OLED_W - lw) / 2, y0 = (OLED_H - lh) / 2;
    for (int row = 0; row < LOGO_ROWS; row++) {
        for (int col = 0; col < LOGO_COLS; col++) {
            if (logo_px(col, row)) {
                for (int a = 0; a < s; a++) {
                    for (int b = 0; b < s; b++) {
                        fb_px_set(m, x0 + col * s + a, y0 + row * s + b, 1);
                    }
                }
            }
        }
    }
    /* Frame it as the icon's tile. */
    const int bx = x0 - 6, by = y0 - 5, bw = lw + 12, bh = lh + 10;
    for (int i = 0; i < bw; i++) {
        fb_px_set(m, bx + i, by, 1);
        fb_px_set(m, bx + i, by + bh - 1, 1);
    }
    for (int j = 0; j < bh; j++) {
        fb_px_set(m, bx, by + j, 1);
        fb_px_set(m, bx + bw - 1, by + j, 1);
    }
}

static void render_status(mono_oled_t *m, const kvm_display_status_t *st, uint8_t page)
{
    memset(m->fb, 0, sizeof(m->fb));
    char buf[40]; /* only ~21 chars fit on screen, but size it so snprintf never truncates */
    switch (page) {
    case 0: /* network */
        fb_icon(m, 0, 0, icon_net);
        fb_text_at(m, 12, 0, st->ap_mode ? "AP RESCUE" : "NETWORK");
        fb_text(m, 2, st->link);
        if (st->ssid[0]) {
            fb_text(m, 3, st->ssid);
        }
        snprintf(buf, sizeof(buf), "IP %s", st->ip[0] ? st->ip : "(none)");
        fb_text(m, 4, buf);
        if (st->ap_mode) {
            fb_text(m, 5, "http://192.168.4.1");
        } else if (st->hostname[0]) {
            snprintf(buf, sizeof(buf), "%s.local", st->hostname);
            fb_text(m, 5, buf);
        }
        if (st->ts_ip[0]) {
            snprintf(buf, sizeof(buf), "TS %s", st->ts_ip);
            fb_text(m, 6, buf);
        }
        break;
    case 1: /* video */
        fb_icon(m, 0, 0, icon_video);
        fb_text_at(m, 12, 0, "VIDEO");
        if (st->video_signal) {
            snprintf(buf, sizeof(buf), "%ux%u", (unsigned)st->hres, (unsigned)st->vres);
            fb_text(m, 2, buf);
            snprintf(buf, sizeof(buf), "%u fps  %s", (unsigned)st->fps, st->codec);
            fb_text(m, 3, buf);
        } else {
            fb_text(m, 2, "NO SIGNAL");
        }
        break;
    default: /* health */
        fb_icon(m, 0, 0, icon_health);
        fb_text_at(m, 12, 0, "HEALTH");
        snprintf(buf, sizeof(buf), "Temp %dC", st->temp_c);
        fb_text_at(m, 0, 2, buf);
        fb_bar(m, 56, 17, 68, 6, st->temp_c / 90.0f); /* 0..90 C */
        {
            const float ram = st->psram_total_kb
                                  ? 1.0f - (float)st->psram_kb / (float)st->psram_total_kb
                                  : 0.0f;
            snprintf(buf, sizeof(buf), "RAM %d%%", (int)(ram * 100 + 0.5f));
            fb_text_at(m, 0, 4, buf);
            fb_bar(m, 56, 33, 68, 6, ram);
        }
        if (st->uptime_s < 3600) {
            snprintf(buf, sizeof(buf), "Up %um %us", (unsigned)(st->uptime_s / 60),
                     (unsigned)(st->uptime_s % 60));
        } else {
            snprintf(buf, sizeof(buf), "Up %uh %um", (unsigned)(st->uptime_s / 3600),
                     (unsigned)((st->uptime_s % 3600) / 60));
        }
        fb_text_at(m, 0, 6, buf);
        break;
    }
}

static esp_err_t flush(mono_oled_t *m)
{
    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        const uint8_t set[] = {
            (uint8_t)(0xB0 | page),
            (uint8_t)(0x00 | (m->col_offset & 0x0F)),
            (uint8_t)(0x10 | (m->col_offset >> 4)),
        };
        esp_err_t err = send_cmds(m->dev, set, sizeof(set));
        if (err != ESP_OK) {
            return err;
        }
        err = send_page(m->dev, m->fb + (size_t)page * OLED_W);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t mono_oled_attach(mono_oled_t **out, const uint8_t *init_cmds, size_t init_len,
                           uint8_t col_offset)
{
    i2c_master_bus_handle_t bus = capture_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE; /* capture path not up yet - caller retries */
    }
    uint16_t addr = 0;
    if (i2c_master_probe(bus, 0x3C, I2C_TIMEOUT_MS) == ESP_OK) {
        addr = 0x3C;
    } else if (i2c_master_probe(bus, 0x3D, I2C_TIMEOUT_MS) == ESP_OK) {
        addr = 0x3D;
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    mono_oled_t *m = calloc(1, sizeof(*m));
    if (!m) {
        return ESP_ERR_NO_MEM;
    }
    m->col_offset = col_offset;
    m->splash = SPLASH_TICKS;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &m->dev);
    if (err != ESP_OK) {
        free(m);
        return err;
    }
    err = send_cmds(m->dev, init_cmds, init_len);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(m->dev);
        free(m);
        return err;
    }
    ESP_LOGI(TAG, "OLED at 0x%02x, column offset %u", addr, col_offset);
    *out = m;
    return ESP_OK;
}

static void render_splash(mono_oled_t *m, const kvm_display_status_t *st)
{
    memset(m->fb, 0, sizeof(m->fb));
    if (m->splash > SPLASH_TICKS / 2) {
        /* Version card first. */
        fb_text_center(m, 2, "ESP-KVM");
        fb_text_center(m, 4, st->version);
    } else {
        /* Then the ESP-KVM badge. */
        draw_logo(m);
    }
}

esp_err_t mono_oled_show(mono_oled_t *m, const kvm_display_status_t *status)
{
    if (m->splash > 0) {
        m->splash--;
        render_splash(m, status);
        return flush(m);
    }
    render_status(m, status, m->screen);
    esp_err_t err = flush(m);
    /* Advance to the next screen after it has had its dwell. */
    if (++m->tick >= SCREEN_DWELL_TICKS) {
        m->tick = 0;
        m->screen = (uint8_t)((m->screen + 1) % STATUS_SCREENS);
    }
    return err;
}

void mono_oled_detach(mono_oled_t *m)
{
    if (!m) {
        return;
    }
    memset(m->fb, 0, sizeof(m->fb));
    (void)flush(m);
    const uint8_t off = 0xAE; /* display off */
    (void)send_cmds(m->dev, &off, 1);
    i2c_master_bus_rm_device(m->dev);
    free(m);
}
