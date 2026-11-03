/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The wire protocol of the two WebSockets: /ws (input and control) and /video
 * (frames). For the firmware and any client written in C; the browser console
 * keeps its own copy in web/src/input/control.ts and web/src/video/stream.ts.
 *
 * /ws, binary, first byte is the message:
 *   client -> device
 *     0x01 mouse abs   buttons:u8, x:u16, y:u16 (0..32767), wheel:i8, pan:i8
 *     0x02 mouse rel   buttons:u8, dx:i16, dy:i16, wheel:i8, pan:i8
 *     0x03 keyboard    modifier:u8, keys:u8[6] (HID usages)
 *     0x04 consumer    usage:u16 (0 = release)
 *     0x05 release all
 *     0x06 ping        (also "send me the status")
 *     0x07 take control (become the controlling client, demoting whoever held it)
 *     0x08 gamepad     buttons:u16, hat:u8 (8 none), lx, ly, rx, ry:u8 (128 centre)
 *   device -> client
 *     0x81 status      flags:u8 (bit0 target attached), leds:u8
 *     0x82 pong
 *     0x83 control     state:u8 (0 held by another, 1 you hold it, 2 free)
 *     0x84 update      phase, then percent
 * All multi-byte numbers are little-endian.
 *
 * /video: every frame is a 12-byte header and the payload:
 *   0      magic 'K'
 *   1      payload type: 1 JPEG, 2 H.264 Annex-B
 *   2..3   width
 *   4..5   height
 *   6..9   sequence number, so a client can see what it missed
 *   10..11 low 16 bits of the device's milliseconds since boot
 * A client's first message subscribes (WS_SUBSCRIBE_TEXT as its first byte
 * asks for the text screen instead); any later message asks for a keyframe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    WS_C2D_MOUSE_ABS = 0x01,
    WS_C2D_MOUSE_REL = 0x02,
    WS_C2D_KEYBOARD = 0x03,
    WS_C2D_CONSUMER = 0x04,
    WS_C2D_RELEASE_ALL = 0x05,
    WS_C2D_PING = 0x06,
    WS_C2D_TAKEOVER = 0x07,
    WS_C2D_PAD = 0x08,
    /* On /video: "frame <seq> arrived". A client that sends these gets at most
     * VIDEO_ACK_WINDOW frames ahead of its acks instead of a full TCP buffer. */
    WS_C2D_VIDEO_ACK = 0x09,

    WS_D2C_STATUS = 0x81,
    WS_D2C_PONG = 0x82,
    WS_D2C_CONTROL = 0x83,
    WS_D2C_UPDATE = 0x84,
    /* On /video, once after subscribing: this device takes WS_C2D_VIDEO_ACK.
     * Older firmware reads any later message as a keyframe request, so a
     * client acks only after it has seen this. */
    WS_D2C_VIDEO_ACKS = 0x85,
};

/* WS_D2C_CONTROL states. */
enum {
    CTRL_HELD_OTHER = 0, /* another client is in control; input here is ignored */
    CTRL_YOU = 1,        /* this client holds control */
    CTRL_FREE = 2,       /* nobody holds control; interacting takes it */
    CTRL_VIEW_ONLY = 3,  /* this client signed in as the viewer: it can never take control */
};

#define KVM_ABS_MAX 32767

#define VIDEO_HDR_LEN 12
#define VIDEO_MAGIC 0x4bu
#define VIDEO_TYPE_JPEG 1
#define VIDEO_TYPE_H264 2
/* The first byte of a subscriber's first message: what it wants to be sent. */
#define WS_SUBSCRIBE_TEXT 2
#define VIDEO_ACK_WINDOW 2

typedef struct {
    uint8_t type;
    uint16_t width, height;
    uint32_t seq;
    uint16_t pts_ms;
} kvm_video_hdr_t;

static inline void kvm_video_hdr_write(uint8_t out[VIDEO_HDR_LEN], const kvm_video_hdr_t *h)
{
    out[0] = VIDEO_MAGIC;
    out[1] = h->type;
    out[2] = (uint8_t)(h->width & 0xff);
    out[3] = (uint8_t)(h->width >> 8);
    out[4] = (uint8_t)(h->height & 0xff);
    out[5] = (uint8_t)(h->height >> 8);
    out[6] = (uint8_t)(h->seq & 0xff);
    out[7] = (uint8_t)((h->seq >> 8) & 0xff);
    out[8] = (uint8_t)((h->seq >> 16) & 0xff);
    out[9] = (uint8_t)((h->seq >> 24) & 0xff);
    out[10] = (uint8_t)(h->pts_ms & 0xff);
    out[11] = (uint8_t)(h->pts_ms >> 8);
}

/** False when @p len is short or the magic is wrong. */
static inline bool kvm_video_hdr_read(const uint8_t *in, size_t len, kvm_video_hdr_t *h)
{
    if (len < VIDEO_HDR_LEN || in[0] != VIDEO_MAGIC) {
        return false;
    }
    h->type = in[1];
    h->width = (uint16_t)(in[2] | (in[3] << 8));
    h->height = (uint16_t)(in[4] | (in[5] << 8));
    h->seq = (uint32_t)in[6] | ((uint32_t)in[7] << 8) | ((uint32_t)in[8] << 16) | ((uint32_t)in[9] << 24);
    h->pts_ms = (uint16_t)(in[10] | (in[11] << 8));
    return true;
}

/* Builders for the client side; each returns the message length. */
static inline size_t kvm_msg_mouse_abs(uint8_t out[8], uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel,
                                       int8_t pan)
{
    out[0] = WS_C2D_MOUSE_ABS;
    out[1] = buttons;
    out[2] = (uint8_t)(x & 0xff);
    out[3] = (uint8_t)(x >> 8);
    out[4] = (uint8_t)(y & 0xff);
    out[5] = (uint8_t)(y >> 8);
    out[6] = (uint8_t)wheel;
    out[7] = (uint8_t)pan;
    return 8;
}

static inline size_t kvm_msg_keyboard(uint8_t out[8], uint8_t modifier, const uint8_t keys[6])
{
    out[0] = WS_C2D_KEYBOARD;
    out[1] = modifier;
    for (int i = 0; i < 6; i++) {
        out[2 + i] = keys[i];
    }
    return 8;
}
