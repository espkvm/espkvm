/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deciding whether a frame is one flat colour. Pure arithmetic over a pixel
 * buffer, with no ESP-IDF in it, so the decision can be compiled and tested on
 * a development machine - see test/run.sh. The timing and the state that go
 * with it are in capture_flat.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether nearly every one of a few hundred samples sits on one colour.
 *
 * @param px      the frame, as the capture buffer holds it
 * @param pixels  how many pixels it has
 * @param bytes_per_px  3 for RGB888/BGR888, 2 for packed UYVY
 */
bool capture_flat_is_flat(const uint8_t *px, size_t pixels, uint8_t bytes_per_px);

#ifdef __cplusplus
}
#endif
