/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tell the browser's H.264 decoder that frames are never reordered.
 *
 * The encoder writes no B-frames, but its SPS does not say so: the VUI has no
 * bitstream_restriction. A decoder then holds frames back before showing any -
 * about a second over WebRTC, measured. The console does the same for the
 * WebSocket in web/src/video/spsPatch.ts; over WebRTC the browser decodes on
 * its own, so the SPS is fixed here before it is sent.
 *
 * Only the layout this encoder writes is handled (Baseline, VUI, no HRD).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The SPS NAL @p nal (header byte included, escaped as on the wire) with the
 * reorder restriction added, written to @p out. Returns its length, or 0 when
 * it already has one, is a layout this does not know, or does not fit.
 */
size_t sps_patch(const uint8_t *nal, size_t len, uint8_t *out, size_t cap);

/**
 * The access unit @p au with its SPS patched, written to @p out (which needs
 * @p len + 16 bytes). Returns the new length, or 0 when there is nothing to
 * patch and @p au should be sent as it is.
 */
size_t sps_patch_au(const uint8_t *au, size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
