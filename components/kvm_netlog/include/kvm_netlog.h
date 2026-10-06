/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A netconsole / syslog receiver: the target sends its kernel messages over UDP
 * (Linux netconsole, or anything that speaks plain syslog), and the device keeps
 * them - which still works when the target's own disk is gone, the moment a log
 * matters most. No wiring: it rides the network the KVM is already on.
 *
 * What arrives is unauthenticated UDP. Anyone on the network can send lines, so
 * they are kept and shown as text, can raise a notification (rate-limited, and
 * marked as coming from the network), and never drive anything: no runbook, no
 * power button. Off by default; settings nc_enable, nc_port, nc_from, nc_match.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_NETLOG_RING_BYTES (64 * 1024)

typedef struct {
    bool running;      /**< listening on the port */
    int port;
    uint64_t rx_bytes; /**< everything kept so far, the ring's head */
    uint32_t packets;
    uint32_t refused;  /**< packets from an address not in nc_from */
    char last_from[16];
} kvm_netlog_status_t;

/** One-time setup: the ring and the receiver task. Opens no socket yet. */
esp_err_t kvm_netlog_init(void);

/** (Re)read the nc_* settings, reopen the socket and report KVM_CAP_NETLOG. */
void kvm_netlog_apply(void);

void kvm_netlog_status(kvm_netlog_status_t *out);

/**
 * Copy what arrived from @p *pos on, up to @p cap bytes, and move @p *pos past
 * it; an old position jumps to the oldest byte kept. @p head, if not NULL, gets
 * the total. Returns the bytes copied.
 */
size_t kvm_netlog_read_from(uint64_t *pos, char *out, size_t cap, uint64_t *head);

#ifdef __cplusplus
}
#endif
