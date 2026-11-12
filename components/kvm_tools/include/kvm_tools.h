/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The other end of ESP-KVM Tools (espkvm-tools), the program on the target. It
 * talks over the USB serial port when usb_serial is set to "tools": one JSON
 * object per line, the device asks, the program answers. The protocol is in its repo,
 * PROTOCOL.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest text the device takes from or gives to the clipboard. */
#define KVM_TOOLS_MAX_TEXT (64 * 1024)

typedef struct {
    bool port;          /**< the USB serial port is there, in tools mode */
    bool connected;     /**< the program has the port open and said hello */
    char version[24];   /**< the program's version, from its hello */
    char os[16];        /**< linux, windows, darwin */
    char ops[160];      /**< the ops it answers, comma separated */
} kvm_tools_status_t;

/** Start listening when usb_serial is "tools" and the port is there. */
esp_err_t kvm_tools_init(void);

void kvm_tools_status(kvm_tools_status_t *out);

/**
 * Ask the program. @p params_json is a JSON object or NULL. On ESP_OK @p out
 * gets the whole response line (malloc'd, the caller frees it), which may
 * still say "ok":false. ESP_ERR_INVALID_STATE: not connected. ESP_ERR_TIMEOUT: it
 * did not answer in @p timeout_ms. One request at a time: a second caller waits
 * its turn within the same timeout.
 */
esp_err_t kvm_tools_call(const char *op, const char *params_json, int timeout_ms, char **out);

#ifdef __cplusplus
}
#endif
