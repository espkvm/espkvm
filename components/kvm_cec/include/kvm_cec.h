/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI-CEC over the capture bridge's CEC line.
 *
 * The device is the sink on that cable, so on the CEC bus it is the TV: logical
 * address 0 (14 when a real TV already holds 0), physical address 0.0.0.0. It
 * answers what a TV must answer, keeps a table of who else is on the line, and
 * lets the operator put a source to sleep, wake it and press remote-control
 * keys on it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_CEC_DEVICES 15 /* logical addresses 0..14; 15 is broadcast */

/** Power status as a device reports it (CEC <Report Power Status>). */
typedef enum {
    KVM_CEC_POWER_UNKNOWN = -1,
    KVM_CEC_POWER_ON = 0,
    KVM_CEC_POWER_STANDBY = 1,
    KVM_CEC_POWER_TO_ON = 2,
    KVM_CEC_POWER_TO_STANDBY = 3,
} kvm_cec_power_t;

typedef struct {
    bool present;
    uint16_t phys_addr;   /**< 0xffff when not known */
    int8_t type;          /**< CEC device type, -1 when not known */
    uint32_t vendor;      /**< 24-bit IEEE OUI, 0xffffffff when not known */
    char name[15];        /**< OSD name, "" when not known */
    int8_t power;         /**< kvm_cec_power_t */
    int8_t version;       /**< CEC version byte (5 = 1.4, 6 = 2.0), -1 when not known */
    uint32_t seen_ms;     /**< uptime of the last frame from it */
} kvm_cec_device_t;

#define KVM_CEC_LOG 24

typedef struct {
    uint32_t ms;
    bool tx;             /**< sent by us, else heard */
    uint8_t result;      /**< tx only: 0 ok, 1 nack, 2 arbitration lost, 3 error, 4 timeout */
    uint8_t len;
    uint8_t data[16];
} kvm_cec_log_t;

typedef struct {
    bool running;          /**< the controller is on and the line can be driven */
    uint8_t own_addr;      /**< our logical address */
    int8_t active;         /**< logical address of the active source, -1 when none */
    kvm_cec_device_t dev[KVM_CEC_DEVICES];
    uint8_t log_count;
    kvm_cec_log_t log[KVM_CEC_LOG]; /**< oldest first */
} kvm_cec_status_t;

/** Start the CEC task. It waits for capture to bring the bridge up. */
void kvm_cec_init(void);

/** A snapshot of the table and the recent frames. @p out is large; not for a small stack. */
void kvm_cec_status(kvm_cec_status_t *out);

/** The device an action goes to when none is named: the active source, else the first
 *  playback device, else the first one present. -1 when nobody is there. */
int kvm_cec_target(void);

/** Put a device to sleep (<Standby>). @p la < 0 means kvm_cec_target(); 15 means everyone. */
esp_err_t kvm_cec_standby(int la);

/** Ask a device to wake: <Set Stream Path> to it and the remote's Power On key. Not every
 *  source obeys - a Steam Deck sleeps on CEC but does not wake on it. */
esp_err_t kvm_cec_wake(int la);

/** Press and release one remote-control key (a CEC user control code). */
esp_err_t kvm_cec_key(int la, uint8_t code);

/** Send a raw frame and wait (up to 3 s) for the line's answer; @p result is a short word.
 *  The other actions only queue their frames and return at once. */
esp_err_t kvm_cec_send(const uint8_t *msg, int len, const char **result);

/** The device actions go to, in brief: its name (or "logical address N") and its power
 *  as a word ("on", "standby", "unknown"). False when CEC is off or nobody is there. */
bool kvm_cec_source(char *name, int name_len, const char **power);

/** The same device's OSD name ("" when not known) and vendor id (0xffffffff when not
 *  known). False when CEC is off or nobody is there. */
bool kvm_cec_source_id(char *name, int name_len, uint32_t *vendor);

/** Ask the task to look for devices again now. */
void kvm_cec_rescan(void);

/** "up", "select", "play", ... to a user control code. */
bool kvm_cec_key_from_name(const char *name, uint8_t *code);

#ifdef __cplusplus
}
#endif
