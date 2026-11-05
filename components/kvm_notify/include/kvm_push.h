/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web Push: notifications straight to a browser (RFC 8030, 8291, 8292). The
 * console subscribes a browser and hands the subscription over. The device
 * signs with its own VAPID key, so nothing has to be registered anywhere.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_PUSH_MAX 8

/** The VAPID public key, base64url (made on first use). */
esp_err_t kvm_push_public_key(char *out, size_t cap);

/**
 * Add a browser, or update it when the endpoint is known. p256dh and auth are
 * base64url, as PushSubscription.toJSON() gives them. Returns the slot.
 */
esp_err_t kvm_push_subscribe(const char *endpoint, const char *p256dh, const char *auth,
                             const char *name, int *slot);

esp_err_t kvm_push_remove(int slot);

/** Slot of a known endpoint, or -1. */
int kvm_push_find(const char *endpoint);

/** Subscribed browsers as a JSON array: [{"id","name","host","added"}]. */
size_t kvm_push_list_json(char *out, size_t cap);

/** Bit per subscribed slot. */
uint32_t kvm_push_mask(void);

/**
 * Send one notification to the slots in *mask. A slot that took it, refused
 * it or is gone (the browser unsubscribed) leaves the mask; what is left is
 * worth trying again later. Returns true when some slot refused it.
 */
bool kvm_push_send(const char *title, const char *body, time_t at, uint32_t *mask);

#ifdef __cplusplus
}
#endif
