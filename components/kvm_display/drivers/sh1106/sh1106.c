/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SH1106 OLED. Like the SSD1306 but with its own DC-DC init command, and a
 * 132-column RAM whose glass starts at column 2. Size-dependent commands come
 * from the shared mono-OLED helper.
 */
#include "kvm_display_driver.h"
#include "mono_oled.h"

static const uint8_t sh1106_init[] = {
    0xAE,       /* display off */
    0xD5, 0x80, /* clock divide / oscillator */
    0x40,       /* start line 0 */
    0xAD, 0x8B, /* DC-DC control on (SH1106-specific) */
    0xA1,       /* segment remap */
    0xC8,       /* COM scan direction remapped */
    0x81, 0x80, /* contrast */
    0xD9, 0x22, /* pre-charge */
    0xDB, 0x35, /* VCOMH */
    0xA4,       /* resume to RAM content */
    0xA6,       /* normal (not inverted) */
};

static esp_err_t attach(void **ctx)
{
    return mono_oled_attach((mono_oled_t **)ctx, sh1106_init, sizeof(sh1106_init), 2);
}

static esp_err_t render(void *ctx, const kvm_display_status_t *status)
{
    return mono_oled_show((mono_oled_t *)ctx, status);
}

static void detach(void *ctx)
{
    mono_oled_detach((mono_oled_t *)ctx);
}

const kvm_display_driver_t kvm_display_sh1106 = {
    .name = "sh1106",
    .label = "SH1106",
    .attach = attach,
    .render = render,
    .detach = detach,
};
