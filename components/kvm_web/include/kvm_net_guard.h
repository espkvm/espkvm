/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The safety net for a device nobody can walk up to.
 *
 * A network change on trial: a settings change that can cut the device off
 * (Wi-Fi, address, Tailscale, WireGuard) keeps the old values, and if the
 * device is not back on the network within 10 minutes of the change taking
 * effect, they are put back and the device restarts (setting net_trial).
 *
 * A network watchdog: with no working network for a long time, restart, with
 * the wait growing each time (setting net_watchdog). It covers a Wi-Fi chip
 * that did not start or stopped answering, and whatever nobody foresaw.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the trial left by the last boot, if any, and the watchdog. Once, after
 *  the network and the VPN workers exist. */
void kvm_net_guard_start(void);

/** Before a settings PUT is applied: note which link settings it changes and
 *  keep their current values. @p req is the request, for the path it came by. */
void kvm_net_guard_before_put(const char *json, httpd_req_t *req);

/** After the PUT: @p applied says whether the values were written. */
void kvm_net_guard_after_put(bool applied);

/** A signed-in request was served: the device is reachable. Cheap. */
void kvm_net_guard_seen(httpd_req_t *req);

/** Drop a pending trial without putting anything back (settings reset, the
 *  button reset at start-up). */
void kvm_net_guard_forget(void);

/** True when @p req came over Tailscale (to the device's tailnet address). */
bool kvm_net_guard_via_tailscale(httpd_req_t *req);

/** Seconds left on a running trial, 0 when none. */
uint32_t kvm_net_guard_trial_left(void);

/** What the last revert put back and why, "" when nothing was reverted. */
const char *kvm_net_guard_last_revert(void);

#ifdef __cplusplus
}
#endif
