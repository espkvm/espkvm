/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The display-driver interface. The core (kvm_display.c) gathers telemetry into a
 * plain, self-contained struct and hands it to a driver, which owns EVERYTHING
 * panel-specific: its transport (I2C on the shared capture bus, or SPI on
 * dedicated pins), its pixel format (1-bit mono, RGB565, ...), and its layout. So
 * a monochrome OLED and a round colour TFT implement the same three calls in
 * completely different ways, and neither the core nor one driver knows about the
 * other.
 *
 * To add a display:
 *   1. create drivers/<name>/<name>.c that defines a kvm_display_driver_t and
 *      implements attach() / render() / detach();
 *   2. add its extern to the drivers table in kvm_display.c and to the "disp_type"
 *      setting's choices.
 * Monochrome I2C OLEDs can reuse drivers/mono_oled.* for the framebuffer, font,
 * layout and bus, so such a driver is only its init sequence and column offset.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A snapshot of device telemetry for a panel to render. Deliberately made of only
 * strings and scalars - no cross-component types - so a driver depends on nothing
 * but this header.
 */
typedef struct {
    char hostname[32]; /**< configured hostname (also the mDNS name) */
    /**
     * How to reach the console: the IPv4 address, or the mDNS name where there is
     * no IPv4 (an IPv6-only network). A raw IPv6 address is not offered - it does
     * not fit a panel this size and nobody would read it back off one - and this
     * is "" only when the link is down.
     */
    char ip[40];
    char link[12];     /**< "Ethernet" / "Wi-Fi" / "AP mode" */
    char ssid[33];     /**< Wi-Fi SSID (station) or hotspot name (AP), else "" */
    char ts_ip[24];    /**< Tailscale 100.x address, or "" when not on a tailnet */
    bool ap_mode;      /**< the rescue/AP hotspot is the active link */
    /**
     * A ready-to-encode Wi-Fi join string for the rescue hotspot, in the format
     * phone cameras understand ("WIFI:T:WPA;S:<ssid>;P:<pass>;;"), or "" when the
     * hotspot is not the active link. Built by the core, already escaped, so a
     * driver never has to know the format - it just hands this to a QR encoder.
     *
     * The point of the hotspot is that the device is otherwise unreachable, and
     * the person standing in front of it should not have to type a passphrase off
     * a screen this small. A panel with room for a scannable code draws one; one
     * without ignores this field.
     */
    char join_qr[256];

    /**
     * The hotspot's passphrase, plain, or "" when the hotspot is open or is not
     * the active link. For panels too small to carry a scannable code - a
     * 128x64 OLED would need about forty modules across and has room for
     * fourteen - which leaves reading it off the glass and typing it.
     *
     * Printing a password on a screen is exactly as safe as the hotspot it
     * belongs to: it is there so that whoever is standing at the device can get
     * in, and standing at the device is already the credential.
     */
    char ap_pass[64];

    /**
     * A notice that outranks everything else on the panel: while @c notice is
     * non-empty a driver must draw it and nothing else, ignoring its own screen
     * rotation. See kvm_display_notice().
     *
     * The title is meant to stay put while the detail line changes underneath
     * it, so the panel reads as one thing progressing rather than a slideshow.
     */
    char notice[16];
    char notice_detail[24];
    int8_t notice_pct; /**< 0..100 of progress, or -1 for a notice with none */

    bool video_signal; /**< HDMI is locked and delivering pixels */
    uint16_t hres;     /**< active width, 0 until locked */
    uint16_t vres;     /**< active height */
    uint16_t fps;      /**< encoded frames per second (whole) */
    char codec[8];     /**< "MJPEG" / "H.264" */

    int temp_c;              /**< chip temperature, degrees C */
    uint32_t heap_kb;        /**< free internal heap, KB */
    uint32_t heap_total_kb;  /**< total internal heap, KB (for a usage bar) */
    uint32_t psram_kb;       /**< free PSRAM, KB */
    uint32_t psram_total_kb; /**< total PSRAM, KB (for a usage bar) */
    uint32_t uptime_s;       /**< seconds since boot */
    char version[32];        /**< firmware version, e.g. "v.0.20.0" */
} kvm_display_status_t;

/**
 * A display driver: one panel type. See the file header for the contract.
 *
 * The core calls render() on a steady tick (about once a second) with the latest
 * telemetry. HOW that is presented is entirely the driver's business: a small
 * panel can auto-cycle pages and keep an internal page counter between calls; a
 * round panel can draw a fixed layout (say a progress ring around the data); a
 * large panel can show everything at once. A driver that wants smoother animation
 * than the tick provides can run its own timer from attach(). The core never
 * paginates or lays anything out.
 */
typedef struct {
    const char *name;  /**< selector value (matches a "disp_type" choice) + log tag */
    const char *label; /**< human-readable name */
    /**
     * Detect and bring the panel up. Return ESP_ERR_NOT_FOUND when the panel is
     * absent (so auto-detection just moves on), or ESP_OK with *ctx set to a
     * per-instance context the other calls receive.
     */
    esp_err_t (*attach)(void **ctx);
    /** Draw the current @p status. Called on each tick; the driver owns layout. */
    esp_err_t (*render)(void *ctx, const kvm_display_status_t *status);
    /** Blank the panel and release everything attach() acquired. */
    void (*detach)(void *ctx);
} kvm_display_driver_t;

#ifdef __cplusplus
}
#endif
