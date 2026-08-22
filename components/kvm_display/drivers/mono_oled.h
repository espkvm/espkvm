/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared code for SSD1306/SH1106 mono OLEDs on the capture chip's I2C bus. A
 * driver supplies its init sequence and base column; probe, framebuffer, font,
 * layout and flush live here.
 *
 * These panels cannot be asked their size - the controller drives any glass up
 * to 128x64 and reports nothing - so the size comes from a setting and the
 * layout adapts to it.
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

/** The largest glass these controllers drive; the framebuffer is sized for it. */
#define MONO_OLED_MAX_W 128
#define MONO_OLED_MAX_H 64

/**
 * Probe the capture I2C bus (0x3C then 0x3D) and set the panel up for the
 * configured size.
 *
 * @p init_cmds is the controller's own sequence, display left off. It must omit
 * the size-dependent commands - multiplex ratio, COM pin layout, display offset
 * - which are sent from here before the panel is switched on.
 *
 * @p base_col is where the controller's RAM starts on the glass: 0 for SSD1306,
 * 2 for the SH1106's 132-column RAM.
 *
 * @return ESP_OK with *out set, ESP_ERR_NOT_FOUND when no panel answers, or
 *         ESP_ERR_INVALID_STATE if the capture bus is not up yet.
 */
esp_err_t mono_oled_attach(mono_oled_t **out, const uint8_t *init_cmds, size_t init_len,
                           uint8_t base_col);

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
