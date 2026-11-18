/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watching a part of the screen: the arithmetic, with nothing of ESP-IDF in it
 * so the host tests run it as it is.
 *
 * A region is cut into a 16x16 grid and each cell becomes one brightness - the
 * region's signature. Two signatures differ by the share of cells that moved
 * more than a little, which ignores noise and still sees a progress bar creep.
 * The colour mode looks at the hue of the same samples instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REGION_MAX 8
#define REGION_GRID 16
#define REGION_CELLS (REGION_GRID * REGION_GRID)
#define REGION_NAME_MAX 24
/* Coordinates are parts of the frame, 0..REGION_SCALE, so a new resolution
   keeps the region where it was on the screen. */
#define REGION_SCALE 10000u
/* A cell counts as changed past this many levels of brightness. */
#define REGION_CELL_STEP 16
/* How long "changed" stays on, so a poll once a second still sees it. */
#define REGION_PULSE_MS 5000u

typedef enum {
    REGION_CHANGE, /* the picture in it changed */
    REGION_STILL,  /* it has not changed for hold_s seconds */
    REGION_LIKE,   /* it looks like the remembered picture */
    REGION_UNLIKE, /* it no longer looks like the remembered picture */
    REGION_COLOUR, /* its main colour is a given one, or changed */
    REGION_MODES
} region_mode_t;

typedef enum {
    REGION_COL_ANY, /* colour mode: any change of colour is the event */
    REGION_COL_BLACK,
    REGION_COL_GREY,
    REGION_COL_WHITE,
    REGION_COL_RED,
    REGION_COL_ORANGE,
    REGION_COL_YELLOW,
    REGION_COL_GREEN,
    REGION_COL_CYAN,
    REGION_COL_BLUE,
    REGION_COL_MAGENTA,
    REGION_COLOURS
} region_colour_t;

typedef struct {
    uint16_t x, y, w, h; /* 0..REGION_SCALE of the frame */
} region_rect_t;

typedef struct {
    uint8_t id; /* 1..REGION_MAX, 0 = unused slot */
    char name[REGION_NAME_MAX];
    region_rect_t rect;
    region_mode_t mode;
    uint8_t threshold; /* % of cells that must differ, 1..100 */
    uint16_t hold_s;   /* STILL: seconds unchanged; others: how long it must hold */
    region_colour_t colour;
    bool notify; /* a notification on each event */
    bool clip;   /* save a dashcam clip on each event */
    bool has_ref;
    uint8_t ref[REGION_CELLS]; /* LIKE/UNLIKE: the remembered signature */
} region_cfg_t;

/* A captured frame as it sits in memory. */
typedef struct {
    const uint8_t *px;
    uint16_t w, h;
    uint8_t bytes_per_px; /* 3: 24-bit RGB; 2: packed 4:2:2 */
    uint8_t luma_off;     /* 4:2:2: 1 for UYVY, 0 for YUYV */
    bool bgr;             /* 24-bit: blue first in memory */
} region_frame_t;

/* What one look at a region found. */
typedef struct {
    uint8_t sig[REGION_CELLS];
    uint8_t rgb[3];         /* mean colour */
    region_colour_t colour; /* the colour most samples have */
    uint8_t colour_pct;     /* how many of them */
} region_look_t;

typedef struct {
    bool have_base;
    uint8_t base[REGION_CELLS]; /* CHANGE: the settled picture; STILL: the last move */
    region_colour_t base_colour;
    uint32_t cond_since_ms; /* when the condition started, 0 = it does not hold */
    uint32_t moved_ms;      /* STILL: last time it changed */
    bool settling;          /* CHANGE: still moving after an event, the same change */
    bool active;
    uint32_t active_since_ms;
    uint32_t pulse_until_ms;
    uint8_t diff; /* % of cells off the base or the reference, last look */
    region_colour_t colour;
    uint8_t colour_pct;
    uint8_t rgb[3];
    uint32_t events;
} region_state_t;

/** Look at @p r in @p f. False for a rectangle that is empty or off the frame. */
bool region_look(const region_frame_t *f, const region_rect_t *r, region_look_t *out);

/** % of cells that differ by more than REGION_CELL_STEP. */
uint8_t region_diff(const uint8_t *a, const uint8_t *b);

/**
 * Feed one look into the state. @p now_ms must not be 0.
 * @return true on an event: "changed", or the condition coming true
 */
bool region_step(const region_cfg_t *c, region_state_t *s, const region_look_t *look,
                 uint32_t now_ms);

/** Forget the past: a new mode, a new picture to compare with. */
void region_reset(region_state_t *s);

const char *region_mode_name(region_mode_t m);
int region_mode_parse(const char *s); /* -1 if unknown */
const char *region_colour_name(region_colour_t c);
int region_colour_parse(const char *s); /* -1 if unknown */

/** The colour of one pixel, from RGB. */
region_colour_t region_classify(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
