/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen areas: up to eight rectangles the device watches on its own - for a
 * change, for stillness, for a remembered picture, or for a colour - and the
 * events they raise. The capture task feeds frames in; the notifier, the
 * dashcam, MQTT and the runbooks each poll the event counts, the way they
 * already poll the screen-text watch.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "region_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t id;
    char name[REGION_NAME_MAX];
    region_mode_t mode;
    region_colour_t want_colour;
    bool notify;
    bool clip;
    bool active;
    uint32_t events;
    uint8_t diff;
    region_colour_t colour;
} kvm_region_status_t;

void kvm_regions_init(void);

/** With nobody watching: true when the next look is due and something acts
    on it (a notification, MQTT, a runbook waiting). Worth taking a frame. */
bool kvm_regions_due_unwatched(void);

/** A runbook starts (true) or stops (false) waiting on an area. */
void kvm_regions_waiting(bool on);

/** A captured frame, from the capture task. Looks at most five times a second. */
void kvm_regions_scan(const region_frame_t *f);

/** The picture went away or changed mode: start every area from scratch. */
void kvm_regions_forget(void);

/** The areas as JSON, with what each sees now. Caller frees. */
char *kvm_regions_json(void);

/** Replace all the areas from {"regions":[...]}. */
esp_err_t kvm_regions_set_json(const char *json, char *err, size_t err_len);

/** Remember what area @p id shows now, for "like" and "unlike". */
esp_err_t kvm_regions_remember(uint8_t id);

/** Fill @p out with the areas in use; returns how many. */
int kvm_regions_list(kvm_region_status_t *out, int max);

/** An area by name (any case), or by its id given as digits. False if none. */
bool kvm_regions_find(const char *name, kvm_region_status_t *out);

/** Moves whenever the set of areas or their names and modes change. */
uint32_t kvm_regions_generation(void);

/** What an event of @p a means, in a sentence for a notification. */
void kvm_regions_describe(const kvm_region_status_t *a, char *out, size_t len);

/** A moment (a change) rather than a state that holds: "wait" waits for the next one. */
static inline bool kvm_regions_is_moment(const kvm_region_status_t *a)
{
    return a->mode == REGION_CHANGE || (a->mode == REGION_COLOUR && a->want_colour == REGION_COL_ANY);
}

#ifdef __cplusplus
}
#endif
