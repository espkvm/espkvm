/*
 * SPDX-FileCopyrightText: 2026 Jonathan Rowny
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the p4kvm project: https://github.com/jrowny/p4kvm
 */
#pragma once

#include "esp_log.h"
#include "tc358743.h"

static inline void tc358743_debug_status(tc358743_t *dev)
{
    if (!dev) {
        return;
    }
    uint8_t st = 0;
    if (tc358743_sys_status(dev, &st) != ESP_OK) {
        return;
    }
    ESP_LOGI("tc358743", "SYS_STATUS=0x%02x (TMDS=%d HDMI=%d SYNC=%d DDC5V=%d)", st, (int)(st >> 1) & 1,
             (int)(st >> 4) & 1, (int)(st >> 7) & 1, (int)st & 1);
}
