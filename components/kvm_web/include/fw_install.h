/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Installing a published release the device fetches for itself.
 *
 * The console can already hand the device an image it downloaded, and that
 * covers an update. What it cannot do is go back: the browser can list the
 * published releases (the GitHub API allows a cross-origin read) but it cannot
 * fetch the image, because the host the assets are served from sends no
 * cross-origin header - so the download has to happen here.
 *
 * Doing it here is the better half of the bargain anyway. The device is what
 * needs the file, and a console reached from a phone over a VPN may have no
 * route to GitHub at all while the device on the LAN does.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

/** Longest version string accepted, including the terminator. */
#define FW_INSTALL_VERSION_MAX 32

typedef enum {
    FW_INSTALL_IDLE = 0,  /**< nothing has been asked for since boot */
    FW_INSTALL_RUNNING,   /**< fetching and writing */
    FW_INSTALL_DONE,      /**< written and armed; the restart is on its way */
    FW_INSTALL_FAILED,    /**< gave up; `message` says why */
} fw_install_state_t;

typedef struct {
    fw_install_state_t state;
    int percent;                          /**< 0..100, -1 while the size is unknown */
    char version[FW_INSTALL_VERSION_MAX]; /**< what was asked for */
    char message[96];                     /**< what is happening, or what went wrong */
} fw_install_status_t;

/**
 * Start fetching a published release and writing it to the spare slot.
 *
 * Returns at once: the work runs on its own task, because the web server is a
 * single task around a single select() and a multi-megabyte download on it
 * would hold the whole interface for its duration.
 *
 * @param version a published tag, e.g. "v.0.35.0". Checked against a strict
 *        shape before it is put in a URL - it is operator input, and it must
 *        never be able to point the device somewhere else.
 * @return ESP_ERR_INVALID_ARG for a version that is not a tag,
 *         ESP_ERR_INVALID_STATE if one is already running,
 *         ESP_ERR_NO_MEM if the task will not start.
 */
esp_err_t fw_install_start(const char *version);

/** Where it has got to. Safe to call at any time. */
void fw_install_get_status(fw_install_status_t *out);

/** Whether a fetch is running, so callers can refuse to start a second one. */
bool fw_install_busy(void);
