/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A small RFB 3.8 server. Tight/JPEG carries the capture pipeline's JPEG as-is,
 * so a VNC viewer does not make the P4 allocate a second full-screen buffer.
 */
#include "kvm_vnc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "capture.h"
#include "kvm_settings.h"
#include "usb_hid.h"

static const char *TAG = "vnc";

#define VNC_TASK_STACK 8192
#define VNC_TASK_PRIO 5
#define VNC_CLIENT_TEXT_MAX 4096
#define VNC_READ_TIMEOUT_MS 1000
#define VNC_SEND_TIMEOUT_MS 3000

static volatile uint32_t s_generation;
static TaskHandle_t s_task;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool recv_exact(int fd, void *data, size_t len, uint32_t generation)
{
    uint8_t *p = data;
    while (len) {
        const int n = recv(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            return false;
        }
        if (generation != s_generation) {
            return false;
        }
    }
    return generation == s_generation;
}

static bool send_all(int fd, const void *data, size_t len, uint32_t generation)
{
    const uint8_t *p = data;
    while (len) {
        const int n = send(fd, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        (void)generation;
        return false;
    }
    return generation == s_generation;
}

static bool send_u32(int fd, uint32_t value, uint32_t generation)
{
    uint8_t b[4];
    put_be32(b, value);
    return send_all(fd, b, sizeof(b), generation);
}

/* DES tables are the bit positions from FIPS 46-3, numbered from the MSB. */
static const uint8_t s_ip[64] = {
    58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,
    64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7
};
static const uint8_t s_fp[64] = {
    40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,
    37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,
    34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25
};
static const uint8_t s_e[48] = {
    32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1
};
static const uint8_t s_p[32] = {
    16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,
    2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25
};
static const uint8_t s_pc1[56] = {
    57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,
    60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,
    29,21,13,5,28,20,12,4
};
static const uint8_t s_pc2[48] = {
    14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32
};
static const uint8_t s_shifts[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t s_sbox[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
     4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
     0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
     13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
     10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
     4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
     9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
     1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
     7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

static uint64_t permute(uint64_t in, const uint8_t *table, unsigned n, unsigned width)
{
    uint64_t out = 0;
    for (unsigned i = 0; i < n; i++) {
        out = (out << 1) | ((in >> (width - table[i])) & 1u);
    }
    return out;
}

static uint32_t rol28(uint32_t v, unsigned n)
{
    return ((v << n) | (v >> (28u - n))) & 0x0fffffffu;
}

static uint64_t des_encrypt(uint64_t block, const uint8_t key[8])
{
    uint64_t raw_key = 0;
    for (unsigned i = 0; i < 8; i++) {
        raw_key = (raw_key << 8) | key[i];
    }
    const uint64_t selected = permute(raw_key, s_pc1, 56, 64);
    uint32_t c = (uint32_t)(selected >> 28);
    uint32_t d = (uint32_t)selected & 0x0fffffffu;
    uint64_t round_key[16];
    for (unsigned i = 0; i < 16; i++) {
        c = rol28(c, s_shifts[i]);
        d = rol28(d, s_shifts[i]);
        round_key[i] = permute(((uint64_t)c << 28) | d, s_pc2, 48, 56);
    }

    const uint64_t ip = permute(block, s_ip, 64, 64);
    uint32_t left = (uint32_t)(ip >> 32);
    uint32_t right = (uint32_t)ip;
    for (unsigned round = 0; round < 16; round++) {
        const uint64_t expanded = permute(right, s_e, 48, 32) ^ round_key[round];
        uint32_t s = 0;
        for (unsigned box = 0; box < 8; box++) {
            const uint8_t six = (uint8_t)((expanded >> (42u - 6u * box)) & 0x3fu);
            const uint8_t row = (uint8_t)(((six & 0x20u) >> 4) | (six & 1u));
            const uint8_t col = (uint8_t)((six >> 1) & 0x0fu);
            s = (s << 4) | s_sbox[box][row * 16u + col];
        }
        const uint32_t f = (uint32_t)permute(s, s_p, 32, 32);
        const uint32_t next = left ^ f;
        left = right;
        right = next;
    }
    return permute(((uint64_t)right << 32) | left, s_fp, 64, 64);
}

static uint8_t reverse_bits(uint8_t v)
{
    v = (uint8_t)(((v & 0x55u) << 1) | ((v >> 1) & 0x55u));
    v = (uint8_t)(((v & 0x33u) << 2) | ((v >> 2) & 0x33u));
    return (uint8_t)((v << 4) | (v >> 4));
}

static bool vnc_auth(int fd, uint32_t generation)
{
    const char *password = kvm_setting_str("vnc_pass");
    if (!password || !password[0] || strlen(password) > 8) {
        return false;
    }
    uint8_t key[8] = {0};
    for (size_t i = 0; password[i]; i++) {
        const unsigned char ch = (unsigned char)password[i];
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
        key[i] = reverse_bits(ch);
    }

    uint8_t challenge[16], response[16];
    esp_fill_random(challenge, sizeof(challenge));
    if (!send_all(fd, challenge, sizeof(challenge), generation) ||
        !recv_exact(fd, response, sizeof(response), generation)) {
        return false;
    }
    uint8_t expected[16];
    for (unsigned i = 0; i < 2; i++) {
        uint64_t block = 0;
        for (unsigned j = 0; j < 8; j++) {
            block = (block << 8) | challenge[i * 8 + j];
        }
        const uint64_t encrypted = des_encrypt(block, key);
        for (unsigned j = 0; j < 8; j++) {
            expected[i * 8 + j] = (uint8_t)(encrypted >> (56u - 8u * j));
        }
    }
    uint8_t diff = 0;
    for (unsigned i = 0; i < sizeof(response); i++) {
        diff |= expected[i] ^ response[i];
    }
    return diff == 0;
}

static bool rfb_handshake(int fd, uint32_t generation, uint16_t *width, uint16_t *height)
{
    static const uint8_t version[] = "RFB 003.008\n";
    uint8_t client_version[12];
    if (!send_all(fd, version, 12, generation) ||
        !recv_exact(fd, client_version, sizeof(client_version), generation)) {
        return false;
    }
    (void)client_version;
    static const uint8_t security_types[] = {1, 2}; /* one type: VNC Authentication */
    uint8_t selected;
    if (!send_all(fd, security_types, sizeof(security_types), generation) ||
        !recv_exact(fd, &selected, 1, generation) || selected != 2) {
        return false;
    }
    if (!vnc_auth(fd, generation)) {
        (void)send_u32(fd, 1, generation);
        static const char reason[] = "VNC password is invalid or authentication failed";
        (void)send_u32(fd, sizeof(reason) - 1, generation);
        (void)send_all(fd, reason, sizeof(reason) - 1, generation);
        return false;
    }
    if (!send_u32(fd, 0, generation)) {
        return false;
    }
    uint8_t shared;
    if (!recv_exact(fd, &shared, 1, generation)) {
        return false;
    }
    (void)shared;

    kvm_video_status_t status;
    capture_status_get(&status);
    *width = (uint16_t)(status.hres ? status.hres : 1);
    *height = (uint16_t)(status.vres ? status.vres : 1);
    uint8_t init[24] = {0};
    put_be16(init, *width);
    put_be16(init + 2, *height);
    /* 32-bit true colour, 8 bits per channel, RGB in the normal order. */
    init[4] = 32;
    init[5] = 24;
    init[6] = 0; /* little endian pixels */
    init[7] = 1; /* true colour */
    put_be16(init + 8, 255);
    put_be16(init + 10, 255);
    put_be16(init + 12, 255);
    init[14] = 16;
    init[15] = 8;
    init[16] = 0;
    put_be32(init + 20, 12);
    return send_all(fd, init, sizeof(init), generation) &&
           send_all(fd, "ESP-KVM VNC", 12, generation);
}

static bool rfb_send_update(int fd, uint32_t generation, uint16_t *width, uint16_t *height,
                            bool desktop_size)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    const esp_err_t err = capture_snapshot_jpeg(&jpeg, &jpeg_len, 1800);
    if (err != ESP_OK || !jpeg || !jpeg_len || jpeg_len > 0x3fffffu) {
        free(jpeg);
        /* An empty update leaves the client's outstanding request satisfied. */
        const uint8_t empty[] = {0, 0, 0, 0};
        return send_all(fd, empty, sizeof(empty), generation);
    }

    kvm_video_status_t status;
    capture_status_get(&status);
    const uint16_t w = (uint16_t)status.hres;
    const uint16_t h = (uint16_t)status.vres;
    const bool resized = w && h && (w != *width || h != *height);
    if (resized) {
        if (!desktop_size) {
            free(jpeg);
            const uint8_t empty[] = {0, 0, 0, 0};
            return send_all(fd, empty, sizeof(empty), generation);
        }
        const uint8_t update[] = {0, 0, 0, 1};
        uint8_t rect[12] = {0};
        put_be16(rect + 4, w);
        put_be16(rect + 6, h);
        put_be32(rect + 8, (uint32_t)-223); /* DesktopSize pseudo-encoding */
        const bool ok = send_all(fd, update, sizeof(update), generation) &&
                        send_all(fd, rect, sizeof(rect), generation);
        if (ok) {
            *width = w;
            *height = h;
        }
        free(jpeg);
        return ok;
    }
    const uint8_t update[] = {0, 0, 0, 1};
    bool ok = send_all(fd, update, sizeof(update), generation);
    if (ok) {
        uint8_t rect[12] = {0};
        put_be16(rect + 4, w ? w : *width);
        put_be16(rect + 6, h ? h : *height);
        put_be32(rect + 8, 7); /* Tight encoding */
        const uint8_t jpeg_subencoding = 0x90; /* JPEG rectangle */
        ok = send_all(fd, rect, sizeof(rect), generation) &&
             send_all(fd, &jpeg_subencoding, 1, generation);
    }
    if (ok) {
        uint8_t compact[3];
        size_t n;
        if (jpeg_len < 128) {
            compact[0] = (uint8_t)jpeg_len;
            n = 1;
        } else if (jpeg_len < 16384) {
            compact[0] = (uint8_t)((jpeg_len & 0x7fu) | 0x80u);
            compact[1] = (uint8_t)(jpeg_len >> 7);
            n = 2;
        } else {
            compact[0] = (uint8_t)((jpeg_len & 0x7fu) | 0x80u);
            compact[1] = (uint8_t)(((jpeg_len >> 7) & 0x7fu) | 0x80u);
            compact[2] = (uint8_t)(jpeg_len >> 14);
            n = 3;
        }
        ok = send_all(fd, compact, n, generation) && send_all(fd, jpeg, jpeg_len, generation);
    }
    free(jpeg);
    return ok;
}

static uint8_t keysym_usage(uint32_t key)
{
    if (key >= 'a' && key <= 'z') return (uint8_t)(0x04 + key - 'a');
    if (key >= 'A' && key <= 'Z') return (uint8_t)(0x04 + key - 'A');
    if (key >= '1' && key <= '9') return (uint8_t)(0x1e + key - '1');
    if (key == '0') return 0x27;
    switch (key) {
    case 0xff08: return 0x2a; /* Backspace */
    case 0xff09: return 0x2b; /* Tab */
    case 0xff0d: return 0x28; /* Return */
    case 0xff1b: return 0x29; /* Escape */
    case ' ': return 0x2c;
    case '-': case '_': return 0x2d;
    case '=': case '+': return 0x2e;
    case '[': case '{': return 0x2f;
    case ']': case '}': return 0x30;
    case '\\': case '|': return 0x31;
    case ';': case ':': return 0x33;
    case '\'': case '"': return 0x34;
    case '`': case '~': return 0x35;
    case ',': case '<': return 0x36;
    case '.': case '>': return 0x37;
    case '/': case '?': return 0x38;
    case 0xffe5: return 0x39; /* Caps Lock */
    case 0xffbe: return 0x3a; case 0xffbf: return 0x3b;
    case 0xffc0: return 0x3c; case 0xffc1: return 0x3d;
    case 0xffc2: return 0x3e; case 0xffc3: return 0x3f;
    case 0xffc4: return 0x40; case 0xffc5: return 0x41;
    case 0xffc6: return 0x42; case 0xffc7: return 0x43;
    case 0xffc8: return 0x44; case 0xffc9: return 0x45;
    case 0xff50: return 0x4a; /* Home */
    case 0xff51: return 0x50; /* Left */
    case 0xff52: return 0x52; /* Up */
    case 0xff53: return 0x4f; /* Right */
    case 0xff54: return 0x51; /* Down */
    case 0xff55: return 0x4b; /* Page Up */
    case 0xff56: return 0x4e; /* Page Down */
    case 0xff57: return 0x4d; /* End */
    case 0xff63: return 0x49; /* Insert */
    case 0xffff: return 0x4c; /* Delete */
    default: return 0;
    }
}

static uint8_t keysym_modifier(uint32_t key)
{
    switch (key) {
    case 0xffe1: return 1u << 1; /* left shift */
    case 0xffe2: return 1u << 5; /* right shift */
    case 0xffe3: return 1u << 0; /* left control */
    case 0xffe4: return 1u << 4; /* right control */
    case 0xffe9: return 1u << 2; /* left alt */
    case 0xffea: return 1u << 6; /* right alt */
    case 0xffeb: return 1u << 3; /* left GUI */
    case 0xffec: return 1u << 7; /* right GUI */
    default: return 0;
    }
}

typedef struct {
    uint32_t keysyms[16];
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
    bool tight;
    bool desktop_size;
} client_state_t;

static void send_keyboard_state(const client_state_t *state)
{
    uint8_t modifier = 0, keys[6] = {0};
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (!state->keysyms[i]) continue;
        modifier |= keysym_modifier(state->keysyms[i]);
        const uint8_t usage = keysym_usage(state->keysyms[i]);
        if (!usage) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < n; j++) duplicate |= keys[j] == usage;
        if (!duplicate && n < sizeof(keys)) keys[n++] = usage;
    }
    usb_hid_keyboard(modifier, keys);
}

static void client_key(client_state_t *state, bool down, uint32_t keysym)
{
    int found = -1, empty = -1;
    for (unsigned i = 0; i < 16; i++) {
        if (state->keysyms[i] == keysym) found = (int)i;
        if (!state->keysyms[i] && empty < 0) empty = (int)i;
    }
    if (down && found < 0 && empty >= 0) state->keysyms[empty] = keysym;
    if (!down && found >= 0) state->keysyms[found] = 0;
    send_keyboard_state(state);
}

static void client_pointer(client_state_t *state, uint8_t mask, uint16_t x, uint16_t y)
{
    kvm_video_status_t status;
    capture_status_get(&status);
    const uint32_t w = status.hres ? status.hres : 1;
    const uint32_t h = status.vres ? status.vres : 1;
    if (x >= w) x = (uint16_t)(w - 1);
    if (y >= h) y = (uint16_t)(h - 1);
    uint8_t buttons = 0;
    if (mask & 1u) buttons |= 1u;
    if (mask & 2u) buttons |= 4u; /* RFB middle becomes HID middle */
    if (mask & 4u) buttons |= 2u; /* RFB right becomes HID right */
    const int8_t wheel = (mask & 8u) && !(state->buttons & 8u) ? 1 :
                         (mask & 16u) && !(state->buttons & 16u) ? -1 : 0;
    const int8_t pan = (mask & 32u) && !(state->buttons & 32u) ? 1 :
                       (mask & 64u) && !(state->buttons & 64u) ? -1 : 0;
    state->buttons = mask;
    state->x = x;
    state->y = y;
    usb_hid_mouse_abs(buttons, (uint16_t)((uint32_t)x * 32767u / (w > 1 ? w - 1 : 1)),
                      (uint16_t)((uint32_t)y * 32767u / (h > 1 ? h - 1 : 1)), wheel, pan);
}

static bool discard_bytes(int fd, uint32_t len, uint32_t generation)
{
    uint8_t buf[128];
    while (len) {
        const size_t n = len > sizeof(buf) ? sizeof(buf) : len;
        if (!recv_exact(fd, buf, n, generation)) return false;
        len -= (uint32_t)n;
    }
    return true;
}

static bool client_loop(int fd, uint32_t generation)
{
    uint16_t width, height;
    if (!rfb_handshake(fd, generation, &width, &height)) return false;
    client_state_t state = {0};
    video_frame_viewer_enter();
    bool alive = true;
    while (alive && generation == s_generation) {
        uint8_t type;
        if (!recv_exact(fd, &type, 1, generation)) break;
        switch (type) {
        case 0: { /* SetPixelFormat */
            uint8_t rest[19];
            alive = recv_exact(fd, rest, sizeof(rest), generation);
            break;
        }
        case 1: { /* SetColourMapEntries */
            uint8_t rest[5];
            if (!recv_exact(fd, rest, sizeof(rest), generation)) { alive = false; break; }
            const uint32_t bytes = (uint32_t)be16(rest + 3) * 6u;
            alive = discard_bytes(fd, bytes, generation);
            break;
        }
        case 2: { /* SetEncodings */
            uint8_t header[3];
            if (!recv_exact(fd, header, sizeof(header), generation)) { alive = false; break; }
            const uint16_t count = be16(header + 1);
            if (count > 64) { alive = false; break; }
            state.tight = false;
            state.desktop_size = false;
            for (unsigned i = 0; i < count; i++) {
                uint8_t enc[4];
                if (!recv_exact(fd, enc, sizeof(enc), generation)) { alive = false; break; }
                const uint32_t value = be32(enc);
                if (value == 7) state.tight = true;
                if (value == (uint32_t)-223) state.desktop_size = true;
            }
            if (!state.tight) alive = false;
            break;
        }
        case 3: { /* FramebufferUpdateRequest */
            uint8_t request[9];
            if (!recv_exact(fd, request, sizeof(request), generation)) { alive = false; break; }
            (void)request;
            if (state.tight) {
                alive = rfb_send_update(fd, generation, &width, &height, state.desktop_size);
            } else {
                const uint8_t empty[] = {0, 0, 0, 0};
                alive = send_all(fd, empty, sizeof(empty), generation);
            }
            break;
        }
        case 4: { /* KeyEvent */
            uint8_t event[7];
            if (!recv_exact(fd, event, sizeof(event), generation)) { alive = false; break; }
            client_key(&state, event[0] != 0, be32(event + 3));
            break;
        }
        case 5: { /* PointerEvent */
            uint8_t event[5];
            if (!recv_exact(fd, event, sizeof(event), generation)) { alive = false; break; }
            client_pointer(&state, event[0], be16(event + 1), be16(event + 3));
            break;
        }
        case 6: { /* ClientCutText: consume but do not put clipboard data on the target. */
            uint8_t header[7];
            if (!recv_exact(fd, header, sizeof(header), generation)) { alive = false; break; }
            const uint32_t len = be32(header + 3);
            alive = len <= VNC_CLIENT_TEXT_MAX && discard_bytes(fd, len, generation);
            break;
        }
        default:
            alive = false;
            break;
        }
    }
    uint8_t released[6] = {0};
    usb_hid_keyboard(0, released);
    usb_hid_mouse_abs(0, (uint16_t)((uint32_t)state.x * 32767u / (width > 1 ? width - 1 : 1)),
                      (uint16_t)((uint32_t)state.y * 32767u / (height > 1 ? height - 1 : 1)), 0, 0);
    video_frame_viewer_leave();
    return alive;
}

static int open_listener(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void set_client_timeouts(int fd)
{
    const struct timeval rx = {.tv_sec = VNC_READ_TIMEOUT_MS / 1000,
                               .tv_usec = (VNC_READ_TIMEOUT_MS % 1000) * 1000};
    const struct timeval tx = {.tv_sec = VNC_SEND_TIMEOUT_MS / 1000,
                               .tv_usec = (VNC_SEND_TIMEOUT_MS % 1000) * 1000};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rx, sizeof(rx));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx, sizeof(tx));
}

static void vnc_task(void *arg)
{
    (void)arg;
    uint32_t generation = UINT32_MAX;
    int64_t retry_at_us = 0;
    int listener = -1;
    for (;;) {
        if (generation != s_generation) {
            generation = s_generation;
            retry_at_us = 0;
            if (listener >= 0) close(listener);
            listener = -1;
            if (kvm_setting_bool("vnc_enable")) {
                const char *password = kvm_setting_str("vnc_pass");
                const size_t pass_len = password ? strlen(password) : 0;
                bool printable = pass_len > 0 && pass_len <= 8;
                for (size_t i = 0; printable && i < pass_len; i++) {
                    printable = (unsigned char)password[i] >= 0x20 &&
                                (unsigned char)password[i] <= 0x7e;
                }
                if (!printable) {
                    ESP_LOGW(TAG, "disabled: set a 1-8 character printable ASCII password first");
                } else {
                    const int port = (int)kvm_setting_int("vnc_port");
                    listener = open_listener((uint16_t)port);
                    if (listener >= 0) {
                        ESP_LOGI(TAG, "listening on TCP port %d (VNC authentication enabled)", port);
                    } else {
                        ESP_LOGE(TAG, "could not listen on TCP port %d", port);
                        retry_at_us = esp_timer_get_time() + 5000000;
                    }
                }
            }
        }
        if (listener < 0) {
            if (generation == s_generation && kvm_setting_bool("vnc_enable") &&
                retry_at_us && esp_timer_get_time() >= retry_at_us) {
                retry_at_us = esp_timer_get_time() + 5000000;
                const char *password = kvm_setting_str("vnc_pass");
                const size_t pass_len = password ? strlen(password) : 0;
                bool printable = pass_len > 0 && pass_len <= 8;
                for (size_t i = 0; printable && i < pass_len; i++) {
                    printable = (unsigned char)password[i] >= 0x20 &&
                                (unsigned char)password[i] <= 0x7e;
                }
                if (printable) {
                    listener = open_listener((uint16_t)kvm_setting_int("vnc_port"));
                    if (listener >= 0) {
                        ESP_LOGI(TAG, "VNC listener is now available on TCP port %d",
                                 (int)kvm_setting_int("vnc_port"));
                        retry_at_us = 0;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listener, &readfds);
        const struct timeval wait = {.tv_sec = 1, .tv_usec = 0};
        const int ready = select(listener + 1, &readfds, NULL, NULL, &wait);
        if (ready <= 0 || !FD_ISSET(listener, &readfds)) continue;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int client = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) continue;
        set_client_timeouts(client);
        char ip[16];
        inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "client connected from %s", ip);
        const bool clean = client_loop(client, generation);
        shutdown(client, SHUT_RDWR);
        close(client);
        ESP_LOGI(TAG, "client disconnected%s", clean ? "" : " (protocol or network error)");
    }
}

esp_err_t kvm_vnc_init(void)
{
    if (s_task) return ESP_OK;
    if (xTaskCreateWithCaps(vnc_task, "vnc", VNC_TASK_STACK, NULL, VNC_TASK_PRIO,
                            &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void kvm_vnc_apply(void)
{
    s_generation++;
}
