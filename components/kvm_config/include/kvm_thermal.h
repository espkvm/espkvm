/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * How hot the chip is, and what to do about it.
 *
 * Encoding video is the only thing this device does that generates real heat,
 * so it is also the only thing worth giving up. Control is not: a KVM that
 * stops accepting keystrokes because it is warm has failed at the job it was
 * bought for, at exactly the moment someone is trying to fix the machine it is
 * attached to. So the video path degrades and everything else keeps running.
 *
 * Nothing here happens silently - the state is reported over the API so the
 * console can say why the picture slowed down or stopped.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_THERMAL_NORMAL = 0,
    /** Warm: the frame rate is capped to shed load before it becomes urgent. */
    KVM_THERMAL_HOT,
    /** Too hot: encoding stops until it cools. Input and the web stay up. */
    KVM_THERMAL_CRITICAL,
    /** No usable reading - the sensor did not start. */
    KVM_THERMAL_UNKNOWN,
} kvm_thermal_state_t;

/** Install the on-chip sensor and start sampling. Call once at start-up. */
void kvm_thermal_init(void);

/** Latest reading in Celsius, or 0 when there is no sensor. */
float kvm_thermal_celsius(void);

kvm_thermal_state_t kvm_thermal_state(void);

/** "normal", "hot", "critical", "unknown" - as reported over the API. */
const char *kvm_thermal_state_name(kvm_thermal_state_t state);

/**
 * Upper bound on encoded frames per second right now, folding in the thermal
 * state: the configured limit when all is well, half of it when hot, and zero
 * when encoding must stop.
 */
int kvm_thermal_fps_limit(int configured_fps);

#ifdef __cplusplus
}
#endif
