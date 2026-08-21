/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deciding when to read the screen as text.
 *
 * The reading itself is in kvm_screentext and costs one pass over the frame.
 * The policy is here, because it is the capture loop that knows what is going
 * on, and because the whole feature has to stay invisible in the frame budget:
 *
 *   - Only text modes: 80 columns of 8 or 9 pixels for a BIOS, 128 for a UEFI
 *     console at 1024 wide. 1080p would be 240 columns and is refused by the
 *     resolution alone, which is the case that would actually hurt.
 *   - Only a settled picture. A cheap signature of a few hundred pixels says
 *     whether anything moved; while it keeps changing - someone is scrolling a
 *     menu - nothing is scanned. It is read once the picture has held still,
 *     and not again until it changes.
 *
 * So the steady-state cost is the signature, and a BIOS someone is walking
 * through gets read a few times a minute.
 */
#include "capture_priv.h"
#include "capture_pixfmt.h"
#include "screentext.h"
#include "screentext_store.h"

#include "esp_cache.h"
#include "esp_timer.h"
#include "kvm_settings.h"

#include <ctype.h>
#include <string.h>

/* How long the picture must hold still before it is worth reading. Long enough
 * that a repainting menu is read once, short enough to feel immediate. */
#define SETTLE_US (300 * 1000)

/* How often the watch looks, with nobody connected. A screen that has just
 * changed still has to hold still for SETTLE_US before it is read, so this is
 * the rate of looking, not of scanning. */
#define IDLE_INTERVAL_US (1000 * 1000)

/* Pixels the signature looks at. Spread over the frame by a stride that is
 * coprime with the row length, so it never samples one column. */
#define SIG_SAMPLES 512

static uint32_t frame_signature(const uint8_t *px, size_t bytes)
{
    uint32_t h = 0x811C9DC5u;
    const size_t step = bytes / SIG_SAMPLES | 1u;
    for (size_t i = 0; i < bytes; i += step) {
        h = (h ^ px[i]) * 0x01000193u;
    }
    return h;
}

static uint32_t s_sig;
static int64_t s_changed_us;
static bool s_done;

/**
 * Is this mode one to read?
 *
 * Narrow modes are read by themselves - that is where a BIOS and a boot menu
 * live. A wide one is read only while somebody is asking for it, because a
 * settled 1080p frame is four times the work of 1024x768 and is nearly always a
 * desktop rather than text. Pressing Select or Copy is the ask, and it lapses
 * on its own (screentext_request).
 */
static bool worth_reading(uint32_t hres, uint32_t vres)
{
    if (screentext_mode_unprompted(hres, vres)) {
        return true;
    }
    return screentext_requested() && screentext_mode_supported(hres, vres);
}

/*
 * Bumped by the monitor task when the picture this all describes is gone.
 *
 * The capture task reads it before a scan and again before publishing: a scan
 * that was already in flight when the signal dropped must not put its result
 * back after the clear, which would leave the console offering a copy of a
 * screen that no longer exists - the exact thing forgetting is for.
 */
static volatile uint32_t s_generation;

void capture_screentext_forget(void)
{
    s_generation++;
    s_done = false;
    screentext_publish(NULL);
    screentext_alert_set(NULL);
}

/*
 * Read the screen again even though nothing on it changed.
 *
 * Turning the watch on, or editing the phrases, has to look at what is on the
 * screen now. Without this a machine hung on a panic - a picture that will
 * never change again - is never matched against the phrase just typed to catch
 * exactly that.
 */
static void settings_changed(const char *key, void *user)
{
    (void)user;
    if (strcmp(key, "scr_watch") == 0 || strcmp(key, "scr_match") == 0) {
        s_done = false;
    }
}

void capture_screentext_init(void)
{
    kvm_settings_subscribe(settings_changed, NULL);
}

/* ---- watching for words ------------------------------------------------- */

/**
 * Does @p row read as @p needle anywhere along it?
 *
 * Matched over the row rather than the whole screen, because a screen has no
 * wrapping: a phrase split across two lines was two phrases. Case is ignored,
 * and anything outside ASCII cannot be in a phrase the operator typed, so it
 * simply never matches.
 */
static bool row_contains(const screentext_grid_t *g, uint16_t r, const char *needle)
{
    char line[SCREENTEXT_MAX_COLS + 1];
    uint16_t n = 0;
    for (uint16_t col = 0; col < g->cols && n < sizeof(line) - 1; col++) {
        const uint16_t cp = g->cells[(size_t)r * g->cols + col];
        line[n++] = (cp < 0x80) ? (char)tolower((int)cp) : '\x01';
    }
    line[n] = '\0';
    return strstr(line, needle) != NULL;
}

/**
 * Compare a reading against the phrases the operator asked about, and record
 * the first that is on screen. Raising and clearing are both edges the store
 * reports once, so a phrase that stays on screen does not re-alert every second.
 */
static void check_watch(const screentext_grid_t *g)
{
    if (!kvm_setting_bool("scr_watch")) {
        return;
    }
    char phrases[128];
    snprintf(phrases, sizeof(phrases), "%s", kvm_setting_str("scr_match"));

    char *save = NULL;
    for (char *p = strtok_r(phrases, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        while (*p == ' ') {
            p++;
        }
        size_t len = strlen(p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[--len] = '\0';
        }
        if (!len) {
            continue;
        }
        /* One bounded copy, used both to match and to report. Matching on the
           full phrase but reporting a truncated one - or the other way round -
           makes the alert re-raise itself forever, because the store compares
           what it was given with what it stored. */
        char phrase[SCREENTEXT_ALERT_MAX];
        size_t i = 0;
        for (; i < len && i < sizeof(phrase) - 1; i++) {
            phrase[i] = p[i];
        }
        phrase[i] = '\0';

        char lower[SCREENTEXT_ALERT_MAX];
        for (size_t k = 0; k <= i; k++) {
            lower[k] = (char)tolower((unsigned char)phrase[k]);
        }

        for (uint16_t r = 0; r < g->rows; r++) {
            if (row_contains(g, r, lower)) {
                screentext_alert_set(phrase);
                return;
            }
        }
    }
    screentext_alert_set(NULL);
}

/**
 * The same reading, on a device nobody is watching.
 *
 * With no viewer the loop encodes nothing - which is the right call for a
 * feature nobody is looking at, and the wrong one for a watch that exists
 * precisely for the hours nobody is looking. So when the operator has asked for
 * a watch, take a frame anyway, once a second, and read it.
 *
 * The order of the gates matters: the resolution is checked before the frame is
 * touched at all, so a target sitting at 1080p costs nothing - not even the
 * cache sync, which at that size is the expensive part.
 */
void capture_screentext_idle(capture_ctx_t *c)
{
    static int64_t s_last_us;

    if (!kvm_setting_bool("scr_watch") || !c->signal_present) {
        return;
    }
    /* Not worth_reading(): the watch runs with the console closed, so there is
       nobody to have asked, and a watch that woke the chip for a full 1080p pass
       every second would be a different feature with a different price. */
    if (!screentext_mode_unprompted(c->hres, c->vres)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - s_last_us < IDLE_INTERVAL_US) {
        return;
    }
    s_last_us = now;

    portENTER_CRITICAL(&c->fb_lock);
    const int hidx = c->ready_fb_idx;
    c->held_fb_idx = hidx;
    portEXIT_CRITICAL(&c->fb_lock);
    if (hidx < 0) {
        return;
    }
    void *src = c->fb[hidx];
    if (esp_cache_msync(src, c->frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C) == ESP_OK) {
        capture_screentext_tick(c, src);
    }
    portENTER_CRITICAL(&c->fb_lock);
    c->held_fb_idx = -1;
    portEXIT_CRITICAL(&c->fb_lock);
}

void capture_screentext_tick(capture_ctx_t *c, const void *frame)
{

    if (!worth_reading(c->hres, c->vres)) {
        if (s_sig) {
            /* Left text behind - a boot loader handing over to an OS. Whatever
             * is on file now describes a screen that is gone. */
            screentext_publish(NULL);
            s_sig = 0;
            s_done = false;
        }
        return;
    }

    const uint32_t sig = frame_signature(frame, c->frame_bytes);
    const int64_t now = esp_timer_get_time();
    if (sig != s_sig) {
        s_sig = sig;
        s_changed_us = now;
        s_done = false;
        return;
    }
    if (s_done || now - s_changed_us < SETTLE_US) {
        return;
    }
    s_done = true; /* one attempt per still picture, whether or not it reads */

    screentext_grid_t *grid = screentext_scratch();
    if (!grid) {
        return;
    }
    const uint32_t generation = s_generation;
    const screentext_frame_t f = {
        .pixels = frame,
        .fmt = (capture_pixfmt_bytes() == 3) ? SCREENTEXT_FMT_RGB888 : SCREENTEXT_FMT_UYVY,
        .width = c->hres,
        .height = c->vres,
        .stride = c->hres * capture_pixfmt_bytes(),
    };
    const bool got = screentext_scan(&f, grid);
    if (generation != s_generation) {
        return; /* the signal went away while we were reading; the clear wins */
    }
    if (got) {
        screentext_publish(grid);
        check_watch(grid);
    } else {
        screentext_publish(NULL);
        screentext_alert_set(NULL);
    }
}
