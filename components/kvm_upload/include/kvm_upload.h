/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uploading what the recorder and the screenshot button left on the card to a
 * place of the operator's own: a WebDAV server for now. Files wait in a queue
 * and go one at a time from a low-priority task, streamed from the card, so
 * nothing the size of a recording sits in memory.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Whether a destination is set up, so the console can offer the buttons. */
bool kvm_upload_ready(void);

/**
 * Queue a file on the card, by its path from the card's root ("VIDEO/x.ts").
 * The caller checks the path is one it may hand out. ESP_ERR_INVALID_STATE
 * with no destination set up, ESP_ERR_NO_MEM when the queue is full. A file
 * already waiting or going is not queued twice.
 */
esp_err_t kvm_upload_queue(const char *rel);

/** What a file on the card is, for automatic upload. The bit 1 << kind in a
    destination's mask setting (up_dav_ev) sends that kind there on its own. */
typedef enum {
    KVM_UPLOAD_RECORDING = 0,
    KVM_UPLOAD_CLIP = 1,      /**< a clip saved by hand */
    KVM_UPLOAD_DASHCAM = 2,   /**< a clip the dashcam saved because something happened */
    KVM_UPLOAD_SCREENSHOT = 3,
    KVM_UPLOAD_TIMELAPSE = 4,
} kvm_upload_kind_t;

/**
 * A file the device just finished writing: queued if the operator ticked its
 * kind for a destination, ignored otherwise. Safe from any task.
 */
void kvm_upload_auto(kvm_upload_kind_t kind, const char *rel);

/**
 * At boot, once the card is mounted: pick up the files that were still
 * waiting when the device went down.
 */
void kvm_upload_start(void);

/** Stop the file going now and drop the ones waiting. */
void kvm_upload_cancel(void);

/** Check the destination: make the device's folder there. The answer comes in the status. */
esp_err_t kvm_upload_test(void);

/**
 * {"ready":true,"dest":"WebDAV","test":{"state":"idle|running|ok|error","message":".."},
 *  "items":[{"path":"VIDEO/x.ts","state":"queued|sending|done|error|cancelled",
 *            "bytes":0,"total":0,"message":"","retryIn":0,"auto":false}]}
 * A queued item with retryIn > 0 failed to reach the server and goes again in
 * that many seconds; its message says why.
 * Returns the length, or 0 if @p cap is too small.
 */
size_t kvm_upload_status_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
