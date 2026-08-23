/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mono_oled.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "capture.h" /* capture_i2c_bus() - the shared bus */
#include "font5x7.h"
#include "icons.h"
#include "kvm_panels.h"
#include "qrcode.h"
#include "logo.h"

#define TAG "mono_oled"

#define OLED_FB_BYTES (MONO_OLED_MAX_W * (MONO_OLED_MAX_H / 8))
#define I2C_TIMEOUT_MS 100

/* Status screens this helper cycles through, and how many render ticks each is
 * shown for (the core ticks about once a second). */
#define STATUS_SCREENS 3
#define SCREEN_DWELL_TICKS 4

/* Most status lines any screen offers; the panel takes as many as it can hold. */
#define STATUS_ROWS_MAX 6

struct mono_oled {
    i2c_master_dev_handle_t dev;
    uint8_t col_offset;
    uint8_t w;     /* visible width, pixels */
    uint8_t h;     /* visible height, pixels */
    uint8_t pages; /* h / 8 - the controller addresses rows in bands of eight */
    uint8_t first;  /* first page a status row may use */
    uint8_t usable; /* pages left for status from there down */
    bool title;     /* is there room for the screen's name and icon? */
    uint8_t screen; /* which status screen is showing */
    uint8_t tick;   /* ticks the current screen has been up */
    uint8_t scroll;     /* characters the long rows have stepped along */
    uint8_t scroll_max; /* how far the longest row on this screen has to go */
    uint8_t splash; /* ticks of the boot logo still to show */
    uint8_t fb[OLED_FB_BYTES];
};

/** Bytes of framebuffer this panel actually uses. */
static inline size_t fb_bytes(const mono_oled_t *m)
{
    return (size_t)m->w * m->pages;
}

/* Where status rows start and how many there are. A font row needs one page.
   Tall glass spaces them two pages apart, as 128x64 always looked; a short panel
   cannot afford that gap and packs them one page apart. */
static void plan_rows(mono_oled_t *m)
{
    /* Two pages is barely one line of text - no room for a title. */
    m->title = m->pages >= 3;
    /* The blank page under the header is breathing room the tallest glass can
       afford. Anything shorter needs the row more than the gap. */
    m->first = (uint8_t)(!m->title ? 0 : (m->pages >= 8 ? 2 : 1));
    m->usable = (uint8_t)(m->pages - m->first);
}

/* Characters a row fits. The 5x7 font is drawn in cells of six pixels. */
static uint8_t cols(const mono_oled_t *m)
{
    return (uint8_t)((m->w - 5) / 6 + 1);
}

/* Rows that fit at the given page spacing. Step 2 leaves room for the health
   screen's bars and costs half the lines, so short panels use step 1. */
static uint8_t rows_that_fit(const mono_oled_t *m, uint8_t step)
{
    const uint8_t n = (uint8_t)(m->usable / step);
    return n > STATUS_ROWS_MAX ? STATUS_ROWS_MAX : n;
}

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

static esp_err_t send_page(i2c_master_dev_handle_t dev, const uint8_t *row, uint8_t w)
{
    uint8_t buf[1 + MONO_OLED_MAX_W];
    buf[0] = 0x40; /* D/C#=1: pixel data follows */
    memcpy(buf + 1, row, w);
    return i2c_master_transmit(dev, buf, (size_t)w + 1, I2C_TIMEOUT_MS);
}

static void fb_char(mono_oled_t *m, int x, int page, char c)
{
    if (x < 0 || x > m->w - 5 || page < 0 || page >= m->pages) {
        return;
    }
    const uint8_t *g = font5x7[(c < 0x20 || c > 0x7F) ? 0 : (c - 0x20)];
    for (int i = 0; i < 5; i++) {
        m->fb[page * m->w + x + i] = g[i];
    }
}

static void fb_text_at(mono_oled_t *m, int x, int page, const char *s)
{
    for (; *s && x <= m->w - 5; s++, x += 6) {
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
    if (page < 0 || page >= m->pages) {
        return;
    }
    for (int i = 0; i < 8 && x + i < m->w; i++) {
        m->fb[page * m->w + x + i] = g[i];
    }
}

/* Set one pixel - for scaled text (the splash), which is not page-aligned. */
static void fb_px(mono_oled_t *m, int x, int y)
{
    if (x >= 0 && x < m->w && y >= 0 && y < m->h) {
        m->fb[(y / 8) * m->w + x] |= (uint8_t)(1u << (y % 8));
    }
}

/* A 1x string centred on a page row. */
static void fb_text_center(mono_oled_t *m, int page, const char *s)
{
    int x = (m->w - (int)strlen(s) * 6) / 2;
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
    if (x < 0 || x >= m->w || y < 0 || y >= m->h) {
        return;
    }
    const int idx = (y / 8) * m->w + x;
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
    /* Scale to fit, frame included: 3x on a full 128x64, less on smaller glass,
       and nothing at all where even 1x plus its border will not go. */
    int s = 3;
    while (s > 1 && (LOGO_COLS * s + 12 > m->w || LOGO_ROWS * s + 10 > m->h)) {
        s--;
    }
    if (LOGO_COLS * s + 12 > m->w || LOGO_ROWS * s + 10 > m->h) {
        return;
    }
    const int lw = LOGO_COLS * s, lh = LOGO_ROWS * s;
    const int x0 = (m->w - lw) / 2, y0 = (m->h - lh) / 2;
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

/*
 * One line of a status screen, and how badly it is wanted.
 *
 * A screen offers more lines than a short panel can show, so each carries a
 * rank: 0 is what the panel exists to tell you (the address), 1 is worth having,
 * 2 is detail. When the rows run out the highest ranks drop off, and the ones
 * that survive keep the order they were added in - so a 32-pixel strip shows the
 * top of the same screen, never a reshuffled one.
 */
typedef struct {
    char text[48];
    uint8_t rank;
} row_t;

typedef struct {
    row_t v[STATUS_ROWS_MAX * 2];
    uint8_t n;
} rows_t;

static void row_add(rows_t *r, uint8_t rank, const char *fmt, ...)
{
    if (r->n >= sizeof(r->v) / sizeof(r->v[0])) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->v[r->n].text, sizeof(r->v[r->n].text), fmt, ap);
    va_end(ap);
    if (r->v[r->n].text[0]) {
        r->v[r->n].rank = rank;
        r->n++;
    }
}

/* A label on the left and its value on the right of a fixed field, so the
   numbers line up down the screen. Ten columns is what the narrowest glass
   holds, and it clears the health bars on the widest. */
#define KV_COLS 10

static void row_add_kv(rows_t *r, uint8_t rank, const char *label, const char *value)
{
    const int pad = KV_COLS - (int)strlen(label) - (int)strlen(value);
    row_add(r, rank, "%s%*s%s", label, pad > 0 ? pad : 1, "", value);
}

/* Draw the rows that fit, best-ranked first, in the order they were added. */
static void rows_draw(mono_oled_t *m, const rows_t *r, uint8_t step)
{
    const uint8_t cap = rows_that_fit(m, step);
    uint8_t keep[STATUS_ROWS_MAX * 2];
    uint8_t n = 0;
    for (uint8_t rank = 0; rank <= 2 && n < cap; rank++) {
        for (uint8_t i = 0; i < r->n && n < cap; i++) {
            if (r->v[i].rank == rank) {
                keep[n++] = i;
            }
        }
    }
    /* Back into screen order - insertion sort, at most six items. */
    for (uint8_t i = 1; i < n; i++) {
        const uint8_t v = keep[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && keep[j] > v) {
            keep[j + 1] = keep[j];
            j--;
        }
        keep[j + 1] = v;
    }
    for (uint8_t i = 0; i < n; i++) {
        const char *text = r->v[keep[i]].text;
        /* A row wider than the glass steps along a character at a time instead
           of being cut off, and the screen waits for it - a clipped address
           looks like a real one, which is worse than a slow one. */
        const size_t len = strlen(text);
        if (len > cols(m)) {
            const uint8_t over = (uint8_t)(len - cols(m));
            if (over > m->scroll_max) {
                m->scroll_max = over;
            }
            text += (m->scroll < over) ? m->scroll : over;
        }
        fb_text(m, m->first + i * step, text);
    }
}

/* Which of the status screens is showing, as dots in the header. */
static void draw_page_dots(mono_oled_t *m, uint8_t page)
{
    const int x0 = m->w - 2 - (3 + (STATUS_SCREENS - 1) * 5);
    for (uint8_t i = 0; i < STATUS_SCREENS; i++) {
        const int x = x0 + i * 5;
        if (i == page) {
            for (int a = 0; a < 3; a++) {
                for (int b = 0; b < 3; b++) {
                    fb_px(m, x + a, 2 + b);
                }
            }
        } else {
            fb_px(m, x + 1, 3);
        }
    }
}

static void draw_title(mono_oled_t *m, const uint8_t icon[8], const char *name, uint8_t page)
{
    if (!m->title) {
        return;
    }
    /* On narrow glass the icon would eat two of ten columns, so the name alone,
       and no room for the dots either. */
    if (m->w >= 96) {
        fb_icon(m, 0, 0, icon);
        fb_text_at(m, 12, 0, name);
        draw_page_dots(m, page);
    } else {
        fb_text_at(m, 0, 0, name);
    }
    /* Reverse video: a solid bar reads as a header rather than a first line. */
    for (int x = 0; x < m->w; x++) {
        m->fb[x] ^= 0xFF;
    }
}

/* Two pixels of light around the code. The standard asks for four modules, but
   that costs a whole doubling of the scale here - see render_join_qr(). */
#define QR_MARGIN_PX 2

/* Shortest code the encoder can produce, so the smallest glass that could ever
   show one. Panels below this never get the page. */
#define QR_MIN_H (21 + 2 * QR_MARGIN_PX)

typedef struct {
    mono_oled_t *m;
    int side;
} qr_draw_t;

static void qr_paint(esp_qrcode_handle_t qr, void *user_data)
{
    qr_draw_t *d = (qr_draw_t *)user_data;
    mono_oled_t *m = d->m;
    const int modules = esp_qrcode_get_size(qr);
    const int box = (m->h < m->w ? m->h : m->w) - 2 * QR_MARGIN_PX;
    const int scale = box / modules;
    if (scale < 1) {
        return;
    }
    const int side = modules * scale;
    const int x0 = (m->w - side) / 2;
    const int y0 = (m->h - side) / 2;
    /* Light the glass, then cut the dark modules out of it: a code has to be
       dark on light, and here "light" is a lit pixel. */
    memset(m->fb, 0xFF, fb_bytes(m));
    for (int y = 0; y < modules; y++) {
        for (int x = 0; x < modules; x++) {
            if (!esp_qrcode_get_module(qr, x, y)) {
                continue;
            }
            for (int a = 0; a < scale; a++) {
                for (int b = 0; b < scale; b++) {
                    fb_px_set(m, x0 + x * scale + a, y0 + y * scale + b, 0);
                }
            }
        }
    }
    d->side = side;
}

/*
 * The hotspot join code, as large as the glass allows.
 *
 * Scale is the whole game: at one pixel per module a 29-module code is about
 * five millimetres across, which a phone reads only if it can focus that close.
 * Trimming the quiet zone from the standard's four modules to two pixels is
 * what buys the second pixel per module on a 64-pixel panel - 58 pixels instead
 * of 37. The rest of the glass is lit, so the sides give back more zone than
 * the top and bottom gave up.
 *
 * Returns false when not even one pixel per module fits; the caller then shows
 * the text instead.
 */
static bool render_join_qr(mono_oled_t *m, const kvm_display_status_t *st)
{
    qr_draw_t draw = {.m = m, .side = 0};
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func_with_cb = qr_paint;
    cfg.user_data = &draw; /* non-NULL is what selects the callback form */
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    m->scroll_max = 0;
    return esp_qrcode_generate(&cfg, st->join_qr) == ESP_OK && draw.side > 0;
}

/** Is there a join code to show, and glass to show it on? */
static bool qr_page(const mono_oled_t *m, const kvm_display_status_t *st)
{
    return st->ap_mode && st->join_qr[0] && m->h >= QR_MIN_H;
}

static void render_status(mono_oled_t *m, const kvm_display_status_t *st, uint8_t page)
{
    memset(m->fb, 0, fb_bytes(m));
    m->scroll_max = 0; /* rows_draw raises it for every row that overflows */
    /* Under about sixteen characters a line, a label costs more than it says. */
    const bool narrow = m->w < 96;
    rows_t r = {0};

    switch (page) {
    case 0: /* network */
        draw_title(m, icon_net, st->ap_mode ? "AP RESCUE" : "NETWORK", page);
        row_add(&r, 0, narrow ? "%s" : "IP %s", st->ip[0] ? st->ip : "(none)");
        row_add(&r, 1, "%s", st->link);
        if (st->ssid[0]) {
            row_add(&r, 2, "%s", st->ssid);
        }
        if (st->ap_mode) {
            /* The same address is already on the line above, so on narrow glass
               the row is worth more than the "http://" in front of it. */
            if (!narrow) {
                row_add(&r, 1, "http://192.168.4.1");
            }
            /* No room for a code here, so the credentials go on as text - this
             * is the screen someone reads while joining the hotspot from a
             * phone, and the alternative is that they cannot join at all. */
            if (st->ap_pass[0]) {
                row_add(&r, 0, narrow ? "%s" : "key %s", st->ap_pass);
            } else {
                row_add(&r, 2, "open network");
            }
        } else if (st->hostname[0]) {
            row_add(&r, 2, "%s.local", st->hostname);
        }
        if (st->ts_ip[0]) {
            row_add(&r, 2, narrow ? "%s" : "TS %s", st->ts_ip);
        }
        break;

    case 1: /* video */
        draw_title(m, icon_video, "VIDEO", page);
        if (st->video_signal) {
            row_add(&r, 0, "%ux%u", (unsigned)st->hres, (unsigned)st->vres);
            row_add(&r, 1, narrow ? "%u fps" : "%u fps  %s", (unsigned)st->fps, st->codec);
            if (narrow) {
                row_add(&r, 2, "%s", st->codec);
            }
        } else {
            row_add(&r, 0, "NO SIGNAL");
        }
        break;

    default: /* health */
        draw_title(m, icon_health, "HEALTH", page);
        {
            char v[16];
            snprintf(v, sizeof(v), "%dC", st->temp_c);
            row_add_kv(&r, 0, "Temp", v);
            const float ram = st->psram_total_kb
                                  ? 1.0f - (float)st->psram_kb / (float)st->psram_total_kb
                                  : 0.0f;
            snprintf(v, sizeof(v), "%d%%", (int)(ram * 100 + 0.5f));
            row_add_kv(&r, 1, "RAM", v);
            if (st->uptime_s < 3600) {
                snprintf(v, sizeof(v), "%um %us", (unsigned)(st->uptime_s / 60),
                         (unsigned)(st->uptime_s % 60));
            } else {
                snprintf(v, sizeof(v), "%uh %um", (unsigned)(st->uptime_s / 3600),
                         (unsigned)((st->uptime_s % 3600) / 60));
            }
            row_add_kv(&r, 2, "Up", v);
        }
        break;
    }
    /* Health spreads out for its bars, but only on glass tall enough to lose
       half its lines and wide enough to put a bar beside the number. */
    const bool spread = (page >= 2 && m->w >= 96 && rows_that_fit(m, 2) >= 2);
    rows_draw(m, &r, spread ? 2 : 1);

    if (spread) {
        const int bx = KV_COLS * 6 + 4, bw = m->w - bx - 4;
        const int first_y = m->first * 8;
        const float ram = st->psram_total_kb
                              ? 1.0f - (float)st->psram_kb / (float)st->psram_total_kb
                              : 0.0f;
        fb_bar(m, bx, first_y + 1, bw, 6, st->temp_c / 90.0f); /* 0..90 C */
        fb_bar(m, bx, first_y + 17, bw, 6, ram);
    }
}

static esp_err_t flush(mono_oled_t *m)
{
    for (uint8_t page = 0; page < m->pages; page++) {
        const uint8_t set[] = {
            (uint8_t)(0xB0 | page),
            (uint8_t)(0x00 | (m->col_offset & 0x0F)),
            (uint8_t)(0x10 | (m->col_offset >> 4)),
        };
        esp_err_t err = send_cmds(m->dev, set, sizeof(set));
        if (err != ESP_OK) {
            return err;
        }
        err = send_page(m->dev, m->fb + (size_t)page * m->w, m->w);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t mono_oled_attach(mono_oled_t **out, const uint8_t *init_cmds, size_t init_len,
                           uint8_t base_col)
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
    /* The glass is the operator's answer to give - nothing on this bus can be
       asked which one is bonded to the controller. */
    const kvm_panel_t *p = kvm_panel_selected();
    m->w = p->w;
    m->h = p->h;
    m->pages = (uint8_t)(p->h / 8);
    m->col_offset = (uint8_t)(base_col + p->extra_col);
    plan_rows(m);
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
    if (err == ESP_OK) {
        /*
         * The three commands that depend on the glass rather than the
         * controller, sent after the driver's own sequence and before the panel
         * is switched on. Multiplex ratio is simply the last row. The COM pin
         * layout is the one real fork: panels taller than 32 pixels wire their
         * COM lines alternately (0x12), shorter ones sequentially (0x02), and
         * getting it wrong shows every other row of pixels doubled or missing.
         */
        const uint8_t geom[] = {
            0xA8, (uint8_t)(m->h - 1),
            0xDA, (uint8_t)(m->h > 32 ? 0x12 : 0x02),
            0xD3, 0x00, /* no display offset - the glass is centred by column */
            0xAF,       /* display on */
        };
        err = send_cmds(m->dev, geom, sizeof(geom));
    }
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(m->dev);
        free(m);
        return err;
    }
    ESP_LOGI(TAG, "OLED at 0x%02x, %ux%u, column offset %u, %u status rows", addr,
             (unsigned)m->w, (unsigned)m->h, (unsigned)m->col_offset,
             (unsigned)rows_that_fit(m, 1));
    *out = m;
    return ESP_OK;
}

static void render_splash(mono_oled_t *m, const kvm_display_status_t *st)
{
    memset(m->fb, 0, fb_bytes(m));
    if (m->splash > SPLASH_TICKS / 2) {
        /* Version card first, centred on whatever pages there are. */
        const uint8_t a = (uint8_t)(m->pages >= 6 ? 2 : 0);
        fb_text_center(m, a, "ESP-KVM");
        if (m->pages > a + 1) {
            fb_text_center(m, (uint8_t)(a + (m->pages >= 6 ? 2 : 1)), st->version);
        }
    } else {
        /* Then the ESP-KVM badge. */
        draw_logo(m);
    }
}

/* A notice: title, one line under it, and the progress as a bar - the
 * rectangular equivalent of the round panel's rim gauge. On a short panel the
 * bar is what goes: an update that says "Updating 40%" in words has told you
 * the same thing. */
static void render_notice(mono_oled_t *m, const kvm_display_status_t *st)
{
    memset(m->fb, 0, fb_bytes(m));
    const uint8_t gap = (uint8_t)(m->pages >= 6 ? 2 : 1);
    fb_text_center(m, m->pages >= 6 ? 1 : 0, st->notice);
    if (m->pages > gap) {
        fb_text_center(m, (uint8_t)((m->pages >= 6 ? 1 : 0) + gap), st->notice_detail);
    }
    if (st->notice_pct >= 0 && m->pages >= 6 && m->w >= 96) {
        const int bw = m->w - 28;
        fb_bar(m, 14, (m->pages - 3) * 8, bw, 8, st->notice_pct / 100.0f);
    }
}

esp_err_t mono_oled_show(mono_oled_t *m, const kvm_display_status_t *status)
{
    if (status->notice[0]) {
        m->splash = 0;
        m->scroll = 0;
        render_notice(m, status);
        return flush(m);
    }
    if (m->splash > 0) {
        m->splash--;
        render_splash(m, status);
        return flush(m);
    }
    /* In hotspot mode the code takes a turn of its own between the network
       screen and the rest, so a phone gets four seconds at it. */
    const bool qr = qr_page(m, status);
    uint8_t page = m->screen;
    if (qr && page == 1) {
        if (!render_join_qr(m, status)) {
            /* No room after all - show the network screen rather than a blank. */
            render_status(m, status, 0);
        }
    } else {
        render_status(m, status, (qr && page > 1) ? (uint8_t)(page - 1) : page);
    }
    esp_err_t err = flush(m);
    if (m->scroll < m->scroll_max) {
        m->scroll++;
    }
    /* Move on once the screen has had its dwell and every long row has been
       shown to its end. */
    if (++m->tick >= SCREEN_DWELL_TICKS && m->scroll >= m->scroll_max) {
        const uint8_t screens = (uint8_t)(STATUS_SCREENS + (qr ? 1 : 0));
        m->tick = 0;
        m->scroll = 0;
        m->screen = (uint8_t)((m->screen + 1) % screens);
    }
    return err;
}

void mono_oled_detach(mono_oled_t *m)
{
    if (!m) {
        return;
    }
    memset(m->fb, 0, fb_bytes(m));
    (void)flush(m);
    const uint8_t off = 0xAE; /* display off */
    (void)send_cmds(m->dev, &off, 1);
    i2c_master_bus_rm_device(m->dev);
    free(m);
}

