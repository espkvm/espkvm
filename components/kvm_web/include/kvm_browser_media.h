/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual media from a console tab: the target boots a file that stays on the
 * operator's computer. The tab serves it over the /ws/media WebSocket, and
 * nothing is written to the card. The protocol is in media_ws.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Whether a tab's file is in the drive now, for the media selection. */
bool kvm_browser_media_inserted(void);

typedef struct {
    const char *state; /**< "off", "serving", "waiting" (the tab dropped; it may come back) */
    char name[64];
    uint64_t size;
} kvm_browser_media_status_t;

void kvm_browser_media_status(kvm_browser_media_status_t *out);

#ifdef __cplusplus
}
#endif
