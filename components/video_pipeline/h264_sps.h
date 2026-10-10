/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Add "frames are never reordered" to the SPS in an Annex-B access unit.
 *
 * The encoder writes no B-frames, but its SPS carries a VUI with no
 * bitstream_restriction, and a decoder that is not told holds several frames
 * back before showing one (Chrome: about four, 174 ms at 1080p). This writes
 * max_num_reorder_frames = 0 and max_dec_frame_buffering = 1 into it.
 *
 * Works in place: @p buf holds @p len bytes and has room for @p cap. Returns
 * the new length - @p len unchanged when there is no SPS, it already has the
 * restriction, it is a layout not handled (High profiles, HRD), or there is
 * no room.
 */
size_t h264_sps_add_restriction(uint8_t *buf, size_t len, size_t cap);
