/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Notifications: when something happens the operator would want to know about
 * while nobody is watching - a watched phrase on the screen, the screen going
 * blank - send a message to the channels set up: Telegram, a webhook, the
 * browsers that asked for Web Push. Each channel has a mask of the kinds it
 * takes. All the work is on a low-priority background task; nothing here runs
 * in the video path.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What a notification is about, which picks the channels it goes to. The bit
 * order of the notify_*_ev settings: append, never reorder.
 */
typedef enum {
    KVM_NOTIFY_WATCH,  /**< a watched phrase on the screen */
    KVM_NOTIFY_FLAT,   /**< the screen went one colour */
    KVM_NOTIFY_AREA,   /**< a screen area fired */
    KVM_NOTIFY_CLIP,   /**< the dashcam saved a clip */
    KVM_NOTIFY_NETLOG, /**< a watched word in the target's log */
    KVM_NOTIFY_TSKEY,  /**< the tailnet key runs out soon */
    KVM_NOTIFY_RELEASE, /**< a newer firmware release is out */
    KVM_NOTIFY_KINDS,
    KVM_NOTIFY_ALL = 31, /**< every channel: a test, the "alerts dropped" note */
} kvm_notify_ev_t;

/** Start the worker and the event poll, and register the capability. Once. */
void kvm_notify_init(void);

/**
 * Queue a notification. @p want_photo asks for a screenshot; it is attached
 * only if the setting allows it and there is a signal to take it from.
 * Safe to call from any task; it copies and returns at once.
 */
void kvm_notify_send(kvm_notify_ev_t kind, const char *title, const char *body, bool want_photo);

/**
 * Send the test message, with a screenshot, to one channel: @p route_key is the
 * setting that routes to it, such as "notify_ntfy_ev". NULL or "" sends it to
 * every channel set up. ESP_ERR_NOT_FOUND for a key no channel has.
 */
esp_err_t kvm_notify_send_test(const char *route_key);

/**
 * Send a saved video: to Telegram as a video that plays in the chat (when it is
 * under the Bot API's 50 MB), to the webhook as a message naming the file.
 * @p path is the file's full path; @p card_path is what to call it, e.g.
 * "VIDEO/20260917-140322-event.mp4". Queued, like kvm_notify_send.
 */
void kvm_notify_send_clip(const char *path, const char *card_path, const char *caption);

typedef struct {
    bool enabled;
    char last_result[96]; /* "ok", or why the last send failed */
    char last_at[24];     /* device local time of the last attempt, or "" */
    int pending;          /* events waiting for the network to come back */
} kvm_notify_status_t;

void kvm_notify_status(kvm_notify_status_t *out);

/**
 * Which channels are set up, keyed by the setting that routes to each:
 * {"notify_tg_ev":true,"notify_hook_ev":false,...}. Returns the length, or 0
 * if @p cap is too small.
 */
size_t kvm_notify_ready_json(char *out, size_t cap);

/**
 * Whether a notification of @p kind would go anywhere: notifications are on,
 * and a channel that is set up has its box ticked. Lets a sender skip work that
 * only feeds a notification, such as asking the internet whether a release is out.
 */
bool kvm_notify_routes(kvm_notify_ev_t kind);

/**
 * Ask Telegram, with the saved bot token, which chats have written to the bot,
 * so the chat id can be picked rather than looked up. Runs on the notify task;
 * read the result with kvm_notify_chats_json().
 */
esp_err_t kvm_notify_find_chats(void);

/**
 * The last run as JSON: {"state":"idle|running|ok|error","error":"...",
 * "bot":"username","chats":[{"id":"..","type":"..","name":".."}]}.
 * Returns the length, or 0 if @p cap is too small.
 */
size_t kvm_notify_chats_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
