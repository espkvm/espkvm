/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A runbook's script, parsed. Plain C over a string, so it runs on a host too.
 *
 * One step per line, `#` starts a comment:
 *
 *   key ctrl+alt+del      press a chord and let go
 *   type hello            type text (US layout)
 *   delay 500             wait that many milliseconds
 *   timeout 120           seconds the waits below may take (default 60)
 *   wait Press F2         until a row of the screen contains the phrase
 *   gone Loading          until no row contains it
 *
 * The device reads the screen as characters, so a wait only ever sees a text
 * screen: a BIOS, a boot loader, a console. On a graphical screen it waits for
 * its timeout and the runbook fails there.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB_MAX_STEPS 64
/* Typed text, or a phrase, with its terminator. */
#define RB_ARG_MAX 160
/* A phrase is matched against one row of the screen; longer than this and the
   row matcher would never find it. */
#define RB_PHRASE_MAX 64
#define RB_ERR_MAX 96

typedef enum {
    RB_KEY,
    RB_TYPE,
    RB_DELAY,
    RB_TIMEOUT,
    RB_WAIT,
    RB_GONE,
    RB_RECORD,      /* value: seconds to record, 0 = the rec_max_min setting */
    RB_RECORD_STOP,
    RB_SCREENSHOT,
    RB_TIMELAPSE,   /* every: seconds between frames; value: seconds to run, 0 = until stopped */
    RB_HDMI,        /* HDMI-CEC: arg "standby", "wake" or "key <name>" */
} rb_kind_t;

typedef struct {
    rb_kind_t kind;
    uint16_t line;        /* 1-based line in the source, for messages */
    uint8_t mod;          /* RB_KEY: modifier bits */
    uint8_t code;         /* RB_KEY: usage */
    uint32_t value;       /* RB_DELAY: milliseconds; RB_TIMEOUT: seconds */
    uint16_t every;       /* RB_TIMELAPSE: seconds between frames */
    char arg[RB_ARG_MAX]; /* RB_TYPE: the text; RB_WAIT/RB_GONE: the phrase;
                             RB_KEY: the chord as written */
} rb_step_t;

typedef struct {
    rb_step_t steps[RB_MAX_STEPS];
    uint16_t count;
} rb_script_t;

/**
 * Parse @p src into @p out. False on the first problem, with @p err holding
 * "line N: what", so an editor can show it before anything runs.
 */
bool rb_parse(const char *src, rb_script_t *out, char *err, size_t err_cap);

/**
 * Map a printable ASCII byte to a US-layout usage and whether Shift is held.
 * False for anything not on a US keyboard.
 */
bool rb_ascii_usage(char c, uint8_t *usage, bool *shift);

/** The verb a step was written with: "key", "wait", ... */
const char *rb_kind_name(rb_kind_t kind);

#ifdef __cplusplus
}
#endif
