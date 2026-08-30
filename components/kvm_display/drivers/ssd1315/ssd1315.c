/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SSD1315 OLED. The same controller family as the SSD1306 and the same
 * sequence, with one command the SSD1306 has no equivalent for: 0xAD selects
 * where the current reference comes from, and a module with no external
 * resistor stays dark until it is told to use the internal one. Modules that do
 * not need it accept the command anyway, so this driver is a superset.
 *
 * NOT confirmed on hardware yet - no SSD1315 has been in front of us.
 */
#include "kvm_display_driver.h"
#include "mono_oled.h"

static const uint8_t ssd1315_init[] = {
    0xAE,       /* display off */
    0xD5, 0x80, /* clock divide / oscillator */
    0x40,       /* start line 0 */
    0xAD, 0x30, /* internal IREF - the one command the SSD1306 does not have */
    0x8D, 0x14, /* charge pump on */
    0x20, 0x02, /* memory addressing mode: page */
    0xA1,       /* segment remap */
    0xC8,       /* COM scan direction remapped */
    0x81, 0xCF, /* contrast */
    0xD9, 0xF1, /* pre-charge */
    0xDB, 0x40, /* VCOMH */
    0xA4,       /* resume to RAM content */
    0xA6,       /* normal (not inverted) */
};

static esp_err_t attach(void **ctx)
{
    return mono_oled_attach((mono_oled_t **)ctx, ssd1315_init, sizeof(ssd1315_init), 0);
}

static esp_err_t render(void *ctx, const kvm_display_status_t *status)
{
    return mono_oled_show((mono_oled_t *)ctx, status);
}

static void detach(void *ctx)
{
    mono_oled_detach((mono_oled_t *)ctx);
}

const kvm_display_driver_t kvm_display_ssd1315 = {
    .name = "ssd1315",
    .label = "SSD1315",
    .attach = attach,
    .render = render,
    .detach = detach,
};
