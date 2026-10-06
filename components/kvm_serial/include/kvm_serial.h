/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The target's serial console: a UART on two pins the operator picks, wired to
 * the target's console port - straight for 3.3 V logic (a Raspberry Pi, a
 * router's header), through a MAX3232 module for a real RS-232 port. For a box
 * with no video output at all it is the only console there is.
 *
 * What the target sends is kept in a ring in PSRAM, counted in bytes from the
 * start, so a reader can ask for everything after the position it last saw and
 * a console that opens late still gets the boot messages. Settings: ser_enable,
 * ser_tx, ser_rx, ser_baud. Format is fixed at 8N1.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How much of the target's output is kept. */
#define KVM_SERIAL_RING_BYTES (64 * 1024)

typedef struct {
    bool running;    /**< the UART is up on valid pins */
    int tx_gpio;     /**< -1 when not set */
    int rx_gpio;
    uint32_t baud;
    uint64_t rx_bytes; /**< everything ever received, the ring's head */
    uint64_t tx_bytes;
} kvm_serial_status_t;

/** One-time setup: the ring and the reader task. Opens no UART yet. */
esp_err_t kvm_serial_init(void);

/** (Re)read the ser_* settings, reopen the UART and report KVM_CAP_SERIAL. */
void kvm_serial_apply(void);

void kvm_serial_status(kvm_serial_status_t *out);

/** Send bytes to the target. ESP_ERR_INVALID_STATE when the UART is not up. */
esp_err_t kvm_serial_write(const void *data, size_t len);

/**
 * Copy what arrived from @p *pos on, up to @p max bytes, and move @p *pos past
 * it. A position older than the ring jumps to the oldest byte still kept.
 * Returns the number of bytes copied (0 when nothing new).
 */
size_t kvm_serial_read_from(uint64_t *pos, uint8_t *out, size_t max);

/** The position of the oldest byte still in the ring. */
uint64_t kvm_serial_oldest(void);

/** Block up to @p timeout_ms until more than @p seen bytes have arrived. */
bool kvm_serial_wait(uint64_t seen, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
