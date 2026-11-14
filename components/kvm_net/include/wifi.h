/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** Network mode, mirroring the `net_mode` setting's enum order. */
typedef enum {
    KVM_NET_ETHERNET = 0, /**< wired Ethernet */
    KVM_NET_WIFI_STA = 1, /**< join a WiFi network (station) */
    KVM_NET_WIFI_AP = 2,  /**< be a WiFi hotspot (access point) */
    KVM_NET_AUTO = 3,     /**< Ethernet, with a WiFi station kept up as the backup */
} kvm_net_mode_t;

/**
 * Bring up WiFi in the mode the `net_mode` setting selects (station or AP). The
 * ESP32-P4 has no radio; this runs through an onboard ESP32-C6 over SDIO
 * (esp-hosted + esp_wifi_remote), compiled in only where a board carries a C6
 * (CONFIG_KVM_WIFI). Called by main only when net_mode is not Ethernet. In
 * "auto" the station runs beside Ethernet with a lower route priority, so it
 * carries traffic only while the cable is down.
 *
 * Non-fatal: a missing or silent co-processor logs and returns ESP_OK rather than
 * faulting the boot (the reset button clears the mode back to Ethernet). A no-op
 * stub on a build without CONFIG_KVM_WIFI.
 */
esp_err_t kvm_wifi_init(void);

/**
 * Tell WiFi that nobody has set the console password yet. In AP mode the
 * hotspot then comes up open, as the setup hotspot does on a board with
 * Ethernet: on a board with no network port AP is where a new device starts,
 * and a password invented for it reaches only a serial console or a display,
 * which a new owner may have neither of. Call before kvm_wifi_init().
 */
void kvm_wifi_set_unclaimed(bool unclaimed);

/**
 * Report the WiFi capability as present (hardware compiled in) without spinning
 * up the co-processor. Call this at boot in every mode so the console's
 * Connection switcher and WiFi settings appear even on Ethernet - the C6 is only
 * actually brought up when a WiFi mode is selected. A no-op stub without a C6.
 */
void kvm_wifi_announce(void);

/**
 * Start the open setup hotspot: the way in for a device with no password and no
 * cable. Where the card shares the co-processor's SD slot, the card is unmounted
 * until the next restart. Only the auth endpoints answer until a password is set.
 */
esp_err_t kvm_wifi_setup_ap_start(void);

/** Is the open setup hotspot up? The console is served in the clear while it is. */
bool kvm_wifi_setup_ap_active(void);


/** Live WiFi state for the console's network indicator. */
typedef struct {
    kvm_net_mode_t mode; /**< the active mode (Ethernet when WiFi is not running) */
    bool active;         /**< WiFi carries the default route (always, outside "auto") */
    bool up;             /**< station: has an IP; AP: hotspot started */
    int rssi;            /**< station only: associated AP signal, dBm (0 otherwise) */
    int ap_clients;      /**< AP only: number of associated stations */
    char ssid[33];       /**< station: the joined SSID; AP: the hotspot name */
} kvm_wifi_status_t;

/** Fill @p out with the current WiFi state. Safe to call at any time. */
void kvm_wifi_status(kvm_wifi_status_t *out);

/** True when a WiFi mode was chosen but the co-processor did not start. */
bool kvm_wifi_coproc_failed(void);

/**
 * Start an asynchronous scan for nearby WiFi networks. Non-blocking: it runs on
 * a worker, and only a running WiFi station can scan; other modes finish with
 * an error. Returns ESP_ERR_INVALID_STATE if a scan is already running. Poll
 * kvm_wifi_scan_json() for the result. A no-op without a co-processor.
 */
esp_err_t kvm_wifi_scan_start(void);

/**
 * Write the scan status and results as JSON into @p buf:
 * {"status":"idle|scanning|done|error","aps":[{"ssid":"..","rssi":-50,"auth":3},..]}
 */
void kvm_wifi_scan_json(char *buf, size_t len);
