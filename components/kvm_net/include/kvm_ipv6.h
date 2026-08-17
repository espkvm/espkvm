/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * IPv6 for whichever link is active.
 *
 * The device is dual-stack: IPv4 stays the way it always was, and IPv6 is added
 * alongside it when the network offers one. There is no address to configure -
 * a link-local address is formed from the interface's MAC, and a routable one
 * comes from the router's advertisements (SLAAC), which is how IPv6 hosts are
 * meant to be addressed. Turning it off is a setting rather than a build option
 * because a network can advertise a prefix the operator does not want the KVM
 * answering on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_netif.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Room for the longest textual IPv6 address plus its terminator. */
#define KVM_IP6_STRLEN 46

/** How many addresses one interface can hold (lwIP's per-netif limit). Callers
 *  size buffers with this, so it stays defined in a build without IPv6. */
#if CONFIG_LWIP_IPV6
#define KVM_IP6_MAX_ADDRS CONFIG_LWIP_IPV6_NUM_ADDRESSES
#else
#define KVM_IP6_MAX_ADDRS 3
#endif

/**
 * Called as the device learns routable IPv6 addresses, so the TLS layer can name
 * them in the certificate: @p global is the address that reaches it from outside
 * and @p ula the unique-local one that survives a change of prefix. Either may be
 * "". Returns true if that changed what is stored. Registered by the composition
 * root: the TLS code lives in a component that already depends on this one, so a
 * direct call would be a dependency cycle.
 */
typedef bool (*kvm_ipv6_identity_cb_t)(const char *global, const char *ula);

void kvm_ipv6_set_identity_cb(kvm_ipv6_identity_cb_t cb);

/**
 * Start IPv6 on @p netif: form its link-local address and watch for the ones
 * autoconfiguration brings. Call it once the interface is up (the link-local
 * address cannot be formed before that) - Ethernet does it on link-up, WiFi on
 * association. A no-op when the operator has turned IPv6 off.
 */
void kvm_ipv6_start(esp_netif_t *netif);

/**
 * The interface's current IPv6 addresses as text, most routable first (global,
 * then unique-local, then link-local). Writes at most @p max of them and returns
 * how many were written - 0 when IPv6 is off or nothing has been configured yet.
 */
int kvm_ipv6_addrs(char out[][KVM_IP6_STRLEN], int max);

/**
 * The device's best routable IPv6 address, or false when it has none. A
 * link-local address is not one: it cannot be reached from another subnet and
 * needs a zone index to be used at all.
 */
bool kvm_ipv6_routable(char *out, size_t len);

#ifdef __cplusplus
}
#endif
