/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "region_core.h"

#include <string.h>
#include <strings.h>

/* Samples per cell side: 2x2 points, at a quarter and three quarters across. */
#define SUB 2

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* One pixel as RGB and luma, whatever the frame is made of. */
static void pixel(const region_frame_t *f, uint32_t x, uint32_t y, uint8_t rgb[3], uint8_t *luma)
{
    const size_t i = (size_t)y * f->w + x;
    if (f->bytes_per_px == 3) {
        const uint8_t *p = f->px + i * 3;
        rgb[0] = f->bgr ? p[2] : p[0];
        rgb[1] = p[1];
        rgb[2] = f->bgr ? p[0] : p[2];
        *luma = (uint8_t)((rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8);
        return;
    }
    /* Packed 4:2:2: a pair of pixels in four bytes shares U and V. UYVY puts
       the lumas at 1 and 3, YUYV at 0 and 2. */
    const uint8_t *p = f->px + (i & ~(size_t)1) * 2;
    const int yv = p[f->luma_off + (i & 1) * 2];
    const int u = p[f->luma_off ? 0 : 1] - 128;
    const int v = p[f->luma_off ? 2 : 3] - 128;
    *luma = (uint8_t)yv;
    rgb[0] = clamp8(yv + ((359 * v) >> 8));
    rgb[1] = clamp8(yv - ((88 * u + 183 * v) >> 8));
    rgb[2] = clamp8(yv + ((454 * u) >> 8));
}

region_colour_t region_classify(uint8_t r, uint8_t g, uint8_t b)
{
    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx < 48) {
        return REGION_COL_BLACK;
    }
    const int chroma = mx - mn;
    if (chroma < 40) {
        return mx > 200 ? REGION_COL_WHITE : REGION_COL_GREY;
    }
    /* Hue in degrees, the usual hexcone. */
    int h;
    if (mx == r) {
        h = 60 * (g - b) / chroma;
    } else if (mx == g) {
        h = 120 + 60 * (b - r) / chroma;
    } else {
        h = 240 + 60 * (r - g) / chroma;
    }
    if (h < 0) {
        h += 360;
    }
    if (h < 15 || h >= 345) {
        return REGION_COL_RED;
    }
    if (h < 45) {
        return REGION_COL_ORANGE;
    }
    if (h < 70) {
        return REGION_COL_YELLOW;
    }
    if (h < 170) {
        return REGION_COL_GREEN;
    }
    if (h < 200) {
        return REGION_COL_CYAN;
    }
    if (h < 260) {
        return REGION_COL_BLUE;
    }
    return REGION_COL_MAGENTA;
}

bool region_look(const region_frame_t *f, const region_rect_t *r, region_look_t *out)
{
    if (!f || !f->px || !f->w || !f->h || !r->w || !r->h) {
        return false;
    }
    if ((uint32_t)r->x + r->w > REGION_SCALE || (uint32_t)r->y + r->h > REGION_SCALE) {
        return false;
    }
    const uint32_t x0 = (uint32_t)r->x * f->w / REGION_SCALE;
    const uint32_t y0 = (uint32_t)r->y * f->h / REGION_SCALE;
    uint32_t rw = (uint32_t)r->w * f->w / REGION_SCALE;
    uint32_t rh = (uint32_t)r->h * f->h / REGION_SCALE;
    if (rw == 0) {
        rw = 1;
    }
    if (rh == 0) {
        rh = 1;
    }
    if (x0 >= f->w || y0 >= f->h) {
        return false;
    }
    if (x0 + rw > f->w) {
        rw = f->w - x0;
    }
    if (y0 + rh > f->h) {
        rh = f->h - y0;
    }

    uint32_t votes[REGION_COLOURS] = {0};
    uint32_t sum[3] = {0, 0, 0};
    uint32_t n = 0;
    for (uint32_t cy = 0; cy < REGION_GRID; cy++) {
        for (uint32_t cx = 0; cx < REGION_GRID; cx++) {
            uint32_t lsum = 0;
            for (uint32_t sy = 0; sy < SUB; sy++) {
                for (uint32_t sx = 0; sx < SUB; sx++) {
                    /* A quarter and three quarters into the cell: never its edge. */
                    const uint32_t px = x0 + rw * (cx * 2 * SUB + 2 * sx + 1) / (2 * SUB * REGION_GRID);
                    const uint32_t py = y0 + rh * (cy * 2 * SUB + 2 * sy + 1) / (2 * SUB * REGION_GRID);
                    uint8_t rgb[3], l;
                    pixel(f, px < x0 + rw ? px : x0 + rw - 1, py < y0 + rh ? py : y0 + rh - 1, rgb, &l);
                    lsum += l;
                    sum[0] += rgb[0];
                    sum[1] += rgb[1];
                    sum[2] += rgb[2];
                    votes[region_classify(rgb[0], rgb[1], rgb[2])]++;
                    n++;
                }
            }
            out->sig[cy * REGION_GRID + cx] = (uint8_t)(lsum / (SUB * SUB));
        }
    }
    for (int k = 0; k < 3; k++) {
        out->rgb[k] = (uint8_t)(sum[k] / n);
    }
    region_colour_t best = REGION_COL_BLACK;
    for (int c = REGION_COL_BLACK; c < REGION_COLOURS; c++) {
        if (votes[c] > votes[best]) {
            best = (region_colour_t)c;
        }
    }
    out->colour = best;
    out->colour_pct = (uint8_t)(votes[best] * 100 / n);
    return true;
}

uint8_t region_diff(const uint8_t *a, const uint8_t *b)
{
    uint32_t moved = 0;
    for (int i = 0; i < REGION_CELLS; i++) {
        const int d = (int)a[i] - (int)b[i];
        if (d > REGION_CELL_STEP || d < -REGION_CELL_STEP) {
            moved++;
        }
    }
    return (uint8_t)(moved * 100 / REGION_CELLS);
}

void region_reset(region_state_t *s)
{
    const uint32_t events = s->events;
    memset(s, 0, sizeof(*s));
    s->events = events;
}

/* A level condition: on once it has held for hold_s, off as soon as it fails.
   True on the rise. */
static bool level(const region_cfg_t *c, region_state_t *s, bool cond, uint32_t now)
{
    if (!cond) {
        s->cond_since_ms = 0;
        s->active = false;
        return false;
    }
    if (!s->cond_since_ms) {
        s->cond_since_ms = now;
    }
    if (!s->active && now - s->cond_since_ms >= (uint32_t)c->hold_s * 1000u) {
        s->active = true;
        s->active_since_ms = now;
        s->events++;
        return true;
    }
    return false;
}

/* A moment: the condition held for hold_s, the event fires, and "active" stays
   on for a few seconds so a poll does not miss it. */
static bool pulse(const region_cfg_t *c, region_state_t *s, bool cond, uint32_t now)
{
    if (s->active && now >= s->pulse_until_ms) {
        s->active = false;
    }
    if (!cond) {
        s->cond_since_ms = 0;
        return false;
    }
    if (!s->cond_since_ms) {
        s->cond_since_ms = now;
    }
    if (now - s->cond_since_ms < (uint32_t)c->hold_s * 1000u) {
        return false;
    }
    s->cond_since_ms = 0;
    s->active = true;
    s->active_since_ms = now;
    s->pulse_until_ms = now + REGION_PULSE_MS;
    s->events++;
    return true;
}

bool region_step(const region_cfg_t *c, region_state_t *s, const region_look_t *look, uint32_t now)
{
    s->colour = look->colour;
    s->colour_pct = look->colour_pct;
    memcpy(s->rgb, look->rgb, sizeof(s->rgb));
    const uint8_t thr = c->threshold ? c->threshold : 1;

    if (!s->have_base) {
        memcpy(s->base, look->sig, REGION_CELLS);
        s->base_colour = look->colour;
        s->have_base = true;
        s->moved_ms = now;
        if (c->mode == REGION_CHANGE || c->mode == REGION_STILL ||
            (c->mode == REGION_COLOUR && c->colour == REGION_COL_ANY)) {
            s->diff = 0;
            return false; /* nothing to compare with yet */
        }
    }

    switch (c->mode) {
    case REGION_CHANGE: {
        s->diff = region_diff(look->sig, s->base);
        if (s->settling) {
            /* An animation runs over several looks: one change, one event.
               Follow it until a look matches the one before. */
            memcpy(s->base, look->sig, REGION_CELLS);
            s->settling = s->diff >= thr;
            pulse(c, s, false, now);
            return false;
        }
        const bool ev = pulse(c, s, s->diff >= thr, now);
        if (ev) {
            memcpy(s->base, look->sig, REGION_CELLS); /* the new picture is the one to leave */
            s->settling = true;
        }
        return ev;
    }
    case REGION_STILL: {
        /* Against the picture at the last move, not the last look: a slow
           creep adds up instead of hiding under the threshold at 5 Hz. */
        s->diff = region_diff(look->sig, s->base);
        if (s->diff >= thr) {
            memcpy(s->base, look->sig, REGION_CELLS);
            s->moved_ms = now;
            s->diff = 0;
        }
        const uint32_t hold = (c->hold_s ? c->hold_s : 1) * 1000u;
        const bool still = now - s->moved_ms >= hold;
        if (!still) {
            s->active = false;
            return false;
        }
        if (!s->active) {
            s->active = true;
            s->active_since_ms = now;
            s->events++;
            return true;
        }
        return false;
    }
    case REGION_LIKE:
    case REGION_UNLIKE:
        if (!c->has_ref) {
            s->diff = 0;
            s->active = false;
            s->cond_since_ms = 0;
            return false;
        }
        s->diff = region_diff(look->sig, c->ref);
        return level(c, s, c->mode == REGION_LIKE ? s->diff < thr : s->diff >= thr, now);
    case REGION_COLOUR:
        s->diff = 0;
        if (c->colour == REGION_COL_ANY) {
            const bool ev = pulse(c, s, look->colour != s->base_colour, now);
            if (ev) {
                s->base_colour = look->colour;
            }
            return ev;
        }
        return level(c, s, look->colour == c->colour, now);
    default:
        return false;
    }
}

static const char *const k_modes[REGION_MODES] = {"change", "still", "like", "unlike", "colour"};
static const char *const k_colours[REGION_COLOURS] = {
    "any", "black", "grey", "white", "red", "orange", "yellow", "green", "cyan", "blue", "magenta",
};

const char *region_mode_name(region_mode_t m)
{
    return (unsigned)m < REGION_MODES ? k_modes[m] : "change";
}

int region_mode_parse(const char *s)
{
    for (int i = 0; s && i < REGION_MODES; i++) {
        if (strcasecmp(s, k_modes[i]) == 0) {
            return i;
        }
    }
    if (s && strcasecmp(s, "color") == 0) {
        return REGION_COLOUR;
    }
    return -1;
}

const char *region_colour_name(region_colour_t c)
{
    return (unsigned)c < REGION_COLOURS ? k_colours[c] : "any";
}

int region_colour_parse(const char *s)
{
    for (int i = 0; s && i < REGION_COLOURS; i++) {
        if (strcasecmp(s, k_colours[i]) == 0) {
            return i;
        }
    }
    if (s && strcasecmp(s, "gray") == 0) {
        return REGION_COL_GREY;
    }
    return -1;
}
