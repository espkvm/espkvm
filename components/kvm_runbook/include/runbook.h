/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runbooks: a script of keys to send and phrases to wait for, run on the
 * device itself so it carries on when the browser tab is gone. One at a time.
 */
#pragma once

#include "esp_err.h"
#include "runbook_script.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUNBOOK_NAME_MAX 48
#define RUNBOOK_LINE_MAX 96

typedef enum {
    RUNBOOK_IDLE,    /* nothing has run since boot */
    RUNBOOK_RUNNING,
    RUNBOOK_DONE,    /* the last run reached its end */
    RUNBOOK_FAILED,  /* the last run stopped at a step; see message */
    RUNBOOK_STOPPED, /* somebody stopped the last run */
} runbook_state_t;

typedef struct {
    runbook_state_t state;
    char name[RUNBOOK_NAME_MAX];
    uint16_t step;               /* 1-based; the one running, or the one it ended on */
    uint16_t steps;
    char line[RUNBOOK_LINE_MAX]; /* that step as written */
    char message[RB_ERR_MAX];    /* why it ended, in words */
    uint32_t elapsed_ms;         /* of the run so far, or of the whole run */
} runbook_status_t;

/** Register the capability. Call once at boot, after the HID stack is up. */
void runbook_init(void);

/**
 * Start the runbook called @p name from the `runbooks_json` setting.
 * @param source who asked ("console", "mqtt"), for the log.
 * @param err    RB_ERR_MAX bytes for the parse error when the script is bad.
 * @return ESP_ERR_NOT_FOUND     no runbook of that name
 *         ESP_ERR_INVALID_STATE one is already running
 *         ESP_ERR_INVALID_ARG   the script does not parse; @p err says where
 *         ESP_ERR_NOT_SUPPORTED no USB target to send keys to
 *         ESP_ERR_NO_MEM        no room for the task or its buffers
 */
esp_err_t runbook_start(const char *name, const char *source, char *err, size_t err_cap);

/** Ask the running one to stop at its next step. ESP_ERR_INVALID_STATE if none. */
esp_err_t runbook_stop(void);

bool runbook_busy(void);
void runbook_get_status(runbook_status_t *out);

/**
 * A number that changes whenever the state or the step does - for a poller
 * that wants to know "anything new?" without copying the status. Cheap, and
 * it holds nothing on the caller's stack, which matters on the timer task.
 */
uint32_t runbook_seq(void);

#ifdef __cplusplus
}
#endif
