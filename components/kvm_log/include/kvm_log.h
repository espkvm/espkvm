/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device's own log, kept in a ring that survives a restart.
 *
 * Every hard question this firmware has produced - an update that rolls back, a
 * board that comes up with no picture, a panel that stays dark - was answered by
 * a serial log, and asking an operator to solder on a serial adapter is not an
 * answer. This keeps the last few thousand lines' worth on the device, readable
 * from the console, so a bug report can carry evidence instead of adjectives.
 *
 * It lives in RTC memory, which matters twice over: that memory is not cleared
 * by a software restart, so the log of the boot that failed is still there after
 * the device has restarted into something that works - and it needs no cache, so
 * writing to it is safe even while the flash cache is off, which is exactly when
 * an update is being written.
 *
 * NEVER log a secret. The console can hand this file to anyone who asks for it,
 * and people paste it into public bug reports. Values of settings marked
 * KVM_SF_SECRET - passwords, keys, tokens - must not reach a log line, here or
 * anywhere else.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start capturing. Call first in app_main: everything logged before this - the
 * bootloader and the ROM - is already on the wire and cannot be recovered.
 *
 * Records a boot marker, so a reader can tell where one run ends and the next
 * begins in what is otherwise one continuous stream.
 */
void kvm_log_init(void);

/**
 * Copy the ring out in the order it was written, oldest first.
 *
 * @param out  destination, always NUL-terminated when @p cap is non-zero
 * @param cap  size of @p out; a smaller buffer keeps the NEWEST end, which is
 *             the part worth having
 * @return     bytes written, not counting the terminator
 */
size_t kvm_log_read(char *out, size_t cap);

/** Bytes currently held. */
size_t kvm_log_size(void);

/** Capacity of the ring, so a caller can size its own buffer. */
size_t kvm_log_capacity(void);

#ifdef __cplusplus
}
#endif
