/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The panels the firmware can drive, one entry per "disp_type" choice.
 */
#pragma once

#include <stdint.h>

/* Index into the driver table in kvm_display.c. */
typedef enum {
    KVM_PANEL_DRV_SSD1306 = 0,
    KVM_PANEL_DRV_SH1106 = 1,
    KVM_PANEL_DRV_GC9A01 = 2,
} kvm_panel_drv_t;

typedef struct {
    uint8_t drv;
    uint8_t w;         /* glass width, pixels */
    uint8_t h;         /* glass height, pixels */
    uint8_t extra_col; /* where the glass starts in the controller's RAM */
} kvm_panel_t;

/* What the operator picked. Never NULL: an out-of-range setting falls back to
   the first entry. */
const kvm_panel_t *kvm_panel_selected(void);
