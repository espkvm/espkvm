/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "kvm_proto.h"
#include "usb_video.h"

#define CONFIG_KVM_ENABLE_USB_VIDEO 1
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_WS_TYPE_BINARY 2
#define HTTPD_WS_TYPE_CLOSE 8
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

typedef int esp_err_t;
typedef struct {
    bool view_only;
    const uint8_t *bytes;
    size_t length;
} httpd_req_t;
typedef struct {
    size_t len;
    unsigned type;
    uint8_t *payload;
} httpd_ws_frame_t;

static httpd_req_t *request;
static unsigned edits, queries, role_replies, state_replies;
static uint32_t expected_revision;

static int httpd_req_to_sockfd(httpd_req_t *req) { request = req; return 1; }
static bool kvm_auth_socket_ok(int fd) { return fd == 1; }
static bool kvm_auth_socket_view_only(int fd) { assert(fd == 1); return request->view_only; }
static esp_err_t kvm_auth_reject_ws(httpd_req_t *req) { (void)req; return ESP_FAIL; }
static bool ctrl_client_add(int fd) { assert(fd == 1); return false; }
static void kvm_display_console_joined(void) { assert(false); }
static void kvm_speaker_console_joined(void) { assert(false); }
static void ws_send_pong(int fd) { (void)fd; assert(false); }
static void ws_send_status_to(int fd) { (void)fd; assert(false); }

static esp_err_t httpd_ws_recv_frame(httpd_req_t *req, httpd_ws_frame_t *frame, size_t cap)
{
    frame->len = req->length;
    frame->type = HTTPD_WS_TYPE_BINARY;
    if (cap) {
        assert(cap >= req->length);
        memcpy(frame->payload, req->bytes, req->length);
    }
    return ESP_OK;
}

static void ws_send_control(int fd, uint8_t role)
{
    assert(fd == 1 && role == CTRL_VIEW_ONLY);
    role_replies++;
}

static void ws_send_binary(int fd, const uint8_t *p, size_t n)
{
    assert(fd == 1 && n == 1 && p[0] == WS_D2C_USB_VIDEO);
    state_replies++;
}

void usb_video_console_set(bool enabled, uint8_t display, uint32_t revision)
{
    assert(enabled && display == 2 && revision == 0x78563412);
    edits++;
}

uint8_t *usb_video_console_state(uint8_t opcode, uint32_t revision, size_t *length)
{
    assert(opcode == WS_D2C_USB_VIDEO && revision == expected_revision);
    uint8_t *reply = malloc(1);
    assert(reply);
    *reply = opcode;
    *length = 1;
    queries++;
    return reply;
}

/* Compile the production handler through its USB dispatch with host I/O stubs. */
#include "usb_ws_handler.inc"

int main(void)
{
    const uint8_t packet[] = {WS_C2D_USB_VIDEO, 0x12, 0x34, 0x56, 0x78, 1, 2};
    const size_t lengths[] = {5, 7};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        expected_revision = i ? 0 : 0x78563412;
        httpd_req_t req = {.view_only = true, .bytes = packet, .length = lengths[i]};
        assert(ws_input_handler(&req) == ESP_OK);
        assert(role_replies == i + 1 && queries == i && edits == 0 && state_replies == i);

        req.view_only = false;
        assert(ws_input_handler(&req) == ESP_OK);
        assert(role_replies == i + 1 && queries == i + 1 && edits == i && state_replies == i + 1);
    }
}
