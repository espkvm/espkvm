/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The scheduler: cron lines that fire an action on the device - Wake-on-LAN in
 * the morning, a runbook overnight, a reset on a timetable. It needs a wall
 * clock, so it brings up SNTP itself and does nothing until the clock is set.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_SCHED_NAME_MAX 48

typedef struct {
    bool enabled;          /* the master switch is on */
    bool clock_valid;      /* SNTP has set the time */
    char now[24];          /* local time, "YYYY-MM-DD HH:MM:SS", or "" */
    char tz[40];           /* the POSIX TZ in force */
    uint16_t count;        /* schedules defined */
    char last_name[KVM_SCHED_NAME_MAX]; /* the last one that fired */
    char last_action[16];
    char last_at[24];      /* when it fired, local time */
} kvm_sched_status_t;

/**
 * Carry out one device action, the same set a schedule can fire: "wol",
 * "power", "reset", "poweroff", "runbook" (@p arg is its name), "clip" (save
 * the dashcam's last seconds), "screenshot", "restart". @p name labels it in
 * the log and the status ("button", a schedule's name); @p source says who
 * asked. Quick: a runbook starts its own task. An empty action does nothing.
 */
void kvm_action_run(const char *name, const char *action, const char *arg, const char *source);

/** Start the scheduler task and register the capability. Call once at boot. */
void kvm_sched_init(void);

void kvm_sched_status(kvm_sched_status_t *out);

/**
 * Fire the action of the named schedule now, ignoring its clock and its enabled
 * flag - the console's "Run now" and a test. ESP_ERR_NOT_FOUND if there is no
 * such schedule.
 */
esp_err_t kvm_sched_run(const char *name);

/**
 * The button on the box: a push button on a free GPIO (the M5Stack Unit Button
 * on a Grove port, or any switch to ground). A short press and a long hold each
 * run one action from kvm_action_run, as the btn_* settings choose. Call once
 * at boot; it follows the settings as they change.
 */
void kvm_button_init(void);

#ifdef __cplusplus
}
#endif
