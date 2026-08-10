/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared implementation for SSD1306/SH1106-class 128x64 monochrome OLEDs on the
 * capture chip's I2C bus. A concrete driver (drivers/ssd1306, drivers/sh1106)
 * supplies only its controller init sequence and column offset; the probe,
 * framebuffer, 5x7 font, status layout and flush all live here - so a new mono
 * panel of this family is just a handful of init bytes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "kvm_display_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mono_oled mono_oled_t;

/**
 * Probe the shared capture I2C bus (0x3C then 0x3D), add the panel, and run its
 * @p init_cmds. @p col_offset is the controller's first visible column (0 for
 * SSD1306, 2 for SH1106).
 * @return ESP_OK with *out set, ESP_ERR_NOT_FOUND when no panel answers, or
 *         ESP_ERR_INVALID_STATE if the capture bus is not up yet.
 */
esp_err_t mono_oled_attach(mono_oled_t **out, const uint8_t *init_cmds, size_t init_len,
                           uint8_t col_offset);

/**
 * Draw the current status and push the framebuffer. Called on each tick; this
 * helper keeps its own page counter and auto-advances the network/video/health
 * pages every few ticks, so a mono-OLED driver just forwards the call.
 */
esp_err_t mono_oled_show(mono_oled_t *m, const kvm_display_status_t *status);

/** Blank the panel, remove it from the bus, and free the context. */
void mono_oled_detach(mono_oled_t *m);

#ifdef __cplusplus
}
#endif
