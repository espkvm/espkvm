/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SSD1306 OLED. Page-addressing mode, RAM starting at column 0. Multiplex
 * ratio, COM pin layout and display-on come from the shared helper, which knows
 * the panel's size.
 */
#include "kvm_display_driver.h"
#include "mono_oled.h"

static const uint8_t ssd1306_init[] = {
    0xAE,       /* display off */
    0xD5, 0x80, /* clock divide / oscillator */
    0x40,       /* start line 0 */
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
    return mono_oled_attach((mono_oled_t **)ctx, ssd1306_init, sizeof(ssd1306_init), 0);
}

static esp_err_t render(void *ctx, const kvm_display_status_t *status)
{
    return mono_oled_show((mono_oled_t *)ctx, status);
}

static void detach(void *ctx)
{
    mono_oled_detach((mono_oled_t *)ctx);
}

const kvm_display_driver_t kvm_display_ssd1306 = {
    .name = "ssd1306",
    .label = "SSD1306",
    .attach = attach,
    .render = render,
    .detach = detach,
};
