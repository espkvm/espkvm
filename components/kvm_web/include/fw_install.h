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
#include <stdint.h>

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
 * @param trial_s keep the new image on trial this long after it boots
 *        (kvm_ota_trial.h), 0 for none. The caller checks the range.
 * @return ESP_ERR_INVALID_ARG for a version that is not a tag,
 *         ESP_ERR_INVALID_STATE if one is already running or an update is on trial,
 *         ESP_ERR_NO_MEM if the task will not start.
 */
/**
 * Ask the update manifest what the newest published version is.
 *
 * @param out     receives the version string, e.g. "v.0.40.1"
 * @param out_len size of @p out
 * @return ESP_OK, ESP_ERR_NOT_ALLOWED when the device may not ask
 *         (fw_may_check), or a failure from the request.
 */
esp_err_t fw_latest_version(char *out, size_t out_len);

/**
 * Whether the device may ask the manifest itself: "Let the device fetch
 * releases itself" is on, or the update check is on and a notification
 * channel takes "New firmware". Otherwise it never reaches out.
 */
bool fw_may_check(void);

/**
 * Check once a day whether a newer release is out, while fw_may_check() and a
 * channel wants "New firmware", and send one notification per release. Once,
 * after the notifications are up.
 */
void fw_watch_start(void);

esp_err_t fw_install_start(const char *version, uint32_t trial_s);

/** Where it has got to. Safe to call at any time. */
void fw_install_get_status(fw_install_status_t *out);

/** Whether a fetch is running, so callers can refuse to start a second one. */
bool fw_install_busy(void);
