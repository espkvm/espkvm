/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Web Push channel: the browsers that asked for notifications, a bit each.
 * The sending itself is in web_push.c. A push message is a few hundred bytes,
 * so it carries text only.
 */
#include "kvm_push.h"
#include "notify_priv.h"

#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "notify";

static bool push_send(const notify_pending_t *p, uint32_t *left)
{
    char text[NOTIFY_BODY_MAX + 200];
    if (p->ev.op == EV_CLIP) {
        notify_clip_text(p, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "%s", p->ev.body);
    }
    const bool refused = kvm_push_send(p->ev.title, text, p->at, left);
    ESP_LOGI(TAG, "web push: %s", *left ? "will retry" : "done");
    return refused;
}

const notify_channel_t notify_channel_push = {
    .id = "push",
    .route_key = "notify_push_ev",
    .targets = kvm_push_mask,
    .send = push_send,
};
