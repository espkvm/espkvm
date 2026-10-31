/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the VNC worker. The listener follows vnc_enable / vnc_port at runtime. */
esp_err_t kvm_vnc_init(void);

/** Ask the worker to reconcile the listener after a VNC setting changes. */
void kvm_vnc_apply(void);

#ifdef __cplusplus
}
#endif
