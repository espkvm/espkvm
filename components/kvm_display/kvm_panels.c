/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "kvm_panels.h"

#include <stddef.h>

#include "kvm_settings.h"

/*
 * Order must match s_display_choices[] in kvm_config/kvm_settings_table.c.
 *
 * Entries 0..2 are the three panels that existed before the size was a choice.
 * Their values are stored in NVS, so they must keep their meaning - which is
 * why the list is not grouped by controller.
 *
 * extra_col is where the glass starts in the controller's RAM. A panel narrower
 * than the controller is wired to the middle of it; the number comes from the
 * module's datasheet and does not follow from the width.
 */
static const kvm_panel_t k_panels[] = {
    {KVM_PANEL_DRV_SSD1306, 128, 64, 0},  /* 0.96" and 1.3" */
    {KVM_PANEL_DRV_SH1106, 128, 64, 0},   /* 1.3" */
    {KVM_PANEL_DRV_GC9A01, 240, 240, 0},  /* round colour LCD */
    {KVM_PANEL_DRV_SSD1306, 128, 32, 0},  /* 0.91" */
    {KVM_PANEL_DRV_SSD1306, 96, 16, 0},   /* 0.69" */
    {KVM_PANEL_DRV_SSD1306, 72, 40, 28},  /* 0.42" */
    {KVM_PANEL_DRV_SSD1306, 64, 48, 32},  /* 0.66", the Wemos shield */
    {KVM_PANEL_DRV_SSD1306, 64, 32, 32},  /* 0.49" */
    {KVM_PANEL_DRV_SH1106, 128, 32, 0},
    {KVM_PANEL_DRV_SH1106, 96, 16, 0},
    {KVM_PANEL_DRV_SH1106, 64, 48, 32},
};

const kvm_panel_t *kvm_panel_selected(void)
{
    const int idx = (int)kvm_setting_int("disp_type");
    const size_t n = sizeof(k_panels) / sizeof(k_panels[0]);
    return (idx >= 0 && (size_t)idx < n) ? &k_panels[idx] : &k_panels[0];
}
