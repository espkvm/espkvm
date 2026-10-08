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
    /* Appended, and it has to stay appended: the index is what NVS holds. */
    {KVM_PANEL_DRV_SSD1315, 128, 64, 0}, /* 0.96", untested */
    {KVM_PANEL_DRV_SSD1315, 72, 40, 28}, /* 0.42", the M5Stack Mini OLED Unit */
    {KVM_PANEL_DRV_SH1107, 128, 64, 32}, /* 1.3", the M5Stack Unit OLED; RAM column 32 */
    /* 1.51" transparent, the M5Stack Unit Glass2 (U158-B). An SSD1309 speaks the
       SSD1306's commands but has no charge pump: the 0x14 after the SSD1306's
       0x8D reads to it as a column address, which every page write sets again.
       The first Unit Glass (U158) has an STM32 in front of its panel and is not
       this. */
    {KVM_PANEL_DRV_SSD1306, 128, 64, 0},
};

const kvm_panel_t *kvm_panel_selected(void)
{
    const int idx = (int)kvm_setting_int("disp_type");
    const size_t n = sizeof(k_panels) / sizeof(k_panels[0]);
    return (idx >= 0 && (size_t)idx < n) ? &k_panels[idx] : &k_panels[0];
}
