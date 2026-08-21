/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The handover between the capture task, which reads the screen, and whoever
 * asks for the result. See screentext.h for what the reading itself is.
 */
#pragma once

#include "screentext.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set up the store. Call once, from a single thread, before capture starts.
 *
 * The buffers and the lock are made here rather than on first use because the
 * store is written from the capture task and cleared from the monitor task:
 * two tasks racing to create one mutex end up holding different mutexes, which
 * is worse than no mutex at all.
 */
void screentext_store_init(void);

/**
 * The capture task's working buffer, or NULL when PSRAM could not spare one.
 * Same buffer every time; only the capture task may write to it.
 */
screentext_grid_t *screentext_scratch(void);

/** Publish a reading, or pass NULL to say the screen no longer holds text. */
void screentext_publish(const screentext_grid_t *grid);

/**
 * Copy out the last reading. False when nothing has been read - no text screen
 * has been seen, or the last one has been replaced by a picture.
 */
bool screentext_latest(screentext_grid_t *out, uint32_t *age_ms);

/**
 * Say that somebody is asking for a reading right now.
 *
 * The device reads narrow modes by itself, but a wide one - a 1080p UEFI
 * console - is only read when asked, because reading every settled 1080p frame
 * on the chance that it is text costs four times what 1024x768 does and is
 * almost always a desktop. Pressing Select or Copy is that ask.
 *
 * The request stands for a few seconds, so the console's polling keeps it alive
 * for as long as the text layer is up and it lapses on its own afterwards. It is
 * a hint, not a command: it lifts the resolution policy, nothing else - the
 * picture must still settle, and the screen must still read as characters.
 */
void screentext_request(void);

/** Whether a request made by screentext_request() is still standing. */
bool screentext_requested(void);

/** Longest phrase an alert can carry, including its terminator. */
#define SCREENTEXT_ALERT_MAX 64

/**
 * Record that a watched phrase is on the screen, or pass NULL to say none is.
 *
 * Kept here rather than announced, because the two sides of this have no
 * business knowing about each other: the capture task finds the phrase, and
 * whoever reports it - the log, MQTT, the console - reads it when it suits
 * them. @p seq changes on every transition, so a reader can tell a new alert
 * from the same one still standing.
 */
void screentext_alert_set(const char *phrase);

/** The phrase currently on screen, if any. False when nothing is raised. */
bool screentext_alert_get(char *buf, size_t cap, uint32_t *seq);

#ifdef __cplusplus
}
#endif
