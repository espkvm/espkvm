/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * SH1107 OLED, as on the M5Stack Unit OLED (U119, 1.3" 128x64). The controller
 * sees the glass on its side - 64 columns starting at RAM column 32, by 128
 * rows - so the shared helper turns the picture as it sends it. The sequence
 * follows M5Stack's own driver (M5GFX Panel_SH110x), with the segment and COM
 * directions flipped to match the other drivers, so disp_rotate_180 works the
 * same way here.
 *
 * Confirmed on a Unit OLED on the M5Stack Unit PoE-P4, 2026-10-08.
 */
#include "kvm_display_driver.h"
#include "mono_oled.h"

static const uint8_t sh1107_init[] = {
    0xAE,       /* display off */
    0x40,       /* start line 0 */
    0xEE,       /* end read-modify-write, in case it was left on */
    0x20,       /* page addressing (on the SH1107 this takes no argument) */
    0xDC, 0x00, /* display start line */
    0xD5, 0x50, /* clock divide / oscillator */
    0xAD, 0x8B, /* DC-DC on */
    0xA1,       /* segment remap */
    0xC8,       /* COM scan direction remapped */
    0xD9, 0x22, /* pre-charge */
    0xDB, 0x35, /* VCOMH */
    0xA4,       /* resume to RAM content */
    0xA6,       /* normal (not inverted) */
    0x81, 0x80, /* contrast */
};

static esp_err_t attach(void **ctx)
{
    return mono_oled_attach_ex((mono_oled_t **)ctx, sh1107_init, sizeof(sh1107_init), 0, true);
}

static esp_err_t render(void *ctx, const kvm_display_status_t *status)
{
    return mono_oled_show((mono_oled_t *)ctx, status);
}

static void detach(void *ctx)
{
    mono_oled_detach((mono_oled_t *)ctx);
}

const kvm_display_driver_t kvm_display_sh1107 = {
    .name = "sh1107",
    .label = "SH1107",
    .attach = attach,
    .render = render,
    .detach = detach,
};
