/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Capability registry.
 *
 * A capability is "active" only when all three are true:
 *   compiled  - built into this firmware (Kconfig KVM_ENABLE_*)
 *   available - the hardware probe at boot succeeded
 *   enabled   - the user has not switched it off in the settings
 *
 * When a capability is unavailable it carries a human-readable reason, so the
 * web UI can show a disabled control that explains itself instead of a button
 * that silently does nothing.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_CAP_VIDEO,   /**< TC358743 responds on I2C and the CSI path is up */
    KVM_CAP_MJPEG,   /**< hardware JPEG encoder usable */
    KVM_CAP_H264,    /**< hardware H.264 encoder usable */
    KVM_CAP_HID,     /**< USB device stack mounted on the target */
    KVM_CAP_MSC,     /**< virtual media: microSD present and mounted */
    KVM_CAP_ATX,     /**< ATX power control wired and configured */
    KVM_CAP_AUDIO,   /**< HDMI audio over I2S */
    KVM_CAP_HTTPS,   /**< TLS + authentication */
    KVM_CAP_OTA,     /**< two OTA slots present in the partition table */
    KVM_CAP_NET_STATIC, /**< static addressing rather than DHCP */
    KVM_CAP_COUNT,
} kvm_cap_t;

/** Reset every capability to "compiled per Kconfig, not yet probed". */
void kvm_caps_init(void);

/**
 * Record the outcome of a hardware probe.
 * @param reason_fmt printf format explaining why it is unavailable; pass NULL
 *                   when @p available is true.
 */
void kvm_cap_report(kvm_cap_t cap, bool available, const char *reason_fmt, ...)
    __attribute__((format(printf, 3, 4)));

bool kvm_cap_compiled(kvm_cap_t cap);
bool kvm_cap_available(kvm_cap_t cap);

/** compiled && available && enabled - the only check feature code should make. */
bool kvm_cap_active(kvm_cap_t cap);

/** Stable identifier used in the REST API and in log lines ("video", "h264", ...). */
const char *kvm_cap_key(kvm_cap_t cap);

/** Why the capability is unavailable, or "" when it is. */
const char *kvm_cap_reason(kvm_cap_t cap);

/** Log one line per capability. Called once after probing, for the boot log. */
void kvm_caps_log(void);

/**
 * Serialise the whole registry.
 * @return malloc'd JSON string the caller must free(), or NULL on allocation failure.
 */
char *kvm_caps_to_json(void);

#ifdef __cplusplus
}
#endif
