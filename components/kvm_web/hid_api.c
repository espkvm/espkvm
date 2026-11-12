/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The agent / CUA REST surface: one JPEG frame and simple HID primitives.
 */
#include "http_server.h"

#include "sdkconfig.h" /* the CONFIG_ guards below are read before any of them */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "kvm_ota_trial.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

/* espcoredump only puts its header on the include path when core dumps are
 * enabled, so a build with them off must not ask for it. Everything that uses
 * it is behind the same guard. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

#include "esp_https_server.h"
#include "esp_tls.h"
#include "esp_netif.h"

#include "lwip/sockets.h"

#include "cJSON.h"

#include "http_recv.h"

#include "capture.h"
#include "screentext.h"
#include "screentext_store.h"
#include "ethernet.h"
#include "kvm_ipv6.h"
#include "wifi.h"
#include "coproc.h"
#include "kvm_rtc.h"
#include "kvm_atx.h"
#include "kvm_mqtt.h"
#include "kvm_ts.h"
#include "kvm_wg.h"
#include "kvm_auth.h"
#include "fw_install.h"
#include "kvm_notify.h"
#include "kvm_sched.h"
#include "runbook.h"
#include "kvm_board_header.h"
#include "kvm_caps.h"
#include "kvm_display.h"
#include "kvm_log.h"
#include "kvm_settings.h"
#include "kvm_profiles.h"
#include "kvm_record.h"
#include "web_priv.h"
#include "http_priv.h"
#include "kvm_storage.h"
#include "kvm_thermal.h"
#include "kvm_tls.h"
#include "kvm_acme.h"
#include "kvm_tools.h"
#include "kvm_proto.h"
#include "usb_hid.h"
#include "video_frame.h"

/* ---- agent / CUA endpoints: a single JPEG frame and simple HID primitives ---
 *
 * These make the device drivable by an AI "computer use" agent (or any script)
 * without touching the binary WebSocket protocol: fetch a still, then click,
 * move, press keys or type text over plain REST. Coordinates are the raw HID
 * range 0..32767 on both axes; the caller maps screen pixels onto that using the
 * resolution from /api/v1/video/status. Everything here needs a session and a
 * USB target, exactly like the interactive controls. */

static uint16_t clamp_abs(double v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 32767) {
        v = 32767;
    }
    return (uint16_t)v;
}

/* Mouse button bits in a USB HID boot-mouse report's button byte. */
#define MOUSE_BTN_LEFT 0x01
#define MOUSE_BTN_RIGHT 0x02
#define MOUSE_BTN_MIDDLE 0x04

/* Keyboard modifier byte: Left Shift bit (USB HID). */
#define HID_MOD_LSHIFT 0x02

/* "left" (default) / "right" / "middle", or a raw button mask as a number. */
static uint8_t parse_button(const cJSON *j)
{
    const cJSON *b = cJSON_GetObjectItem(j, "button");
    if (cJSON_IsString(b)) {
        if (strcmp(b->valuestring, "right") == 0) {
            return MOUSE_BTN_RIGHT;
        }
        if (strcmp(b->valuestring, "middle") == 0) {
            return MOUSE_BTN_MIDDLE;
        }
        return MOUSE_BTN_LEFT;
    }
    if (cJSON_IsNumber(b)) {
        return (uint8_t)b->valueint;
    }
    return MOUSE_BTN_LEFT;
}


/* Shared gate for the agent endpoints: a valid session AND the agent API turned
 * on. The toggle is off by default because these hand full keyboard/mouse/screen
 * control of the target to whatever calls them - the same control the console
 * has, over a simpler interface. Returns true to proceed; on false it has
 * already written the response into *out. */

static bool agent_allowed(httpd_req_t *req, esp_err_t *out)
{
    if (!kvm_auth_check(req)) {
        *out = kvm_auth_challenge(req);
        return false;
    }
    if (!kvm_setting_bool("agent_api")) {
        *out = send_json_error(req, "403 Forbidden",
                               "the agent API is off (enable it in Settings > Security)");
        return false;
    }
    return true;
}

bool kvm_web_agent_allowed(httpd_req_t *req, esp_err_t *out)
{
    return agent_allowed(req, out);
}

/* GET a single JPEG still. Needs the MJPEG codec (an H.264 frame is not a
 * standalone image); counts as a viewer briefly so the encoder produces a fresh
 * frame even when nobody is streaming. */
esp_err_t api_video_frame_get(httpd_req_t *req)
{
    /* A still of the screen is a picture like the stream is, so the viewing
       token reaches it - but a session still has to pass the agent switch,
       which is what governs the rest of the machine-facing API. */
    esp_err_t gate;
    if (!kvm_auth_token_ok(req) && !agent_allowed(req, &gate)) {
        return gate;
    }
    /* Either codec: on H.264 the device encodes the held frame as a JPEG once. */
    uint8_t *jpeg = NULL;
    size_t len = 0;
    const esp_err_t err = capture_snapshot_jpeg(&jpeg, &len, 3000);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, "503 Service Unavailable", "no video signal");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable", "no frame came (is the device too hot?)");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_send(req, (const char *)jpeg, len);
    free(jpeg);
    return r;
}

/*
 * Present the keyboard and mouse to the target again, as if the cable had been
 * pulled and put back. Needed when this side restarted while the target stayed
 * on: it kept the address it gave us and we forgot it, so every report goes
 * nowhere. The firmware already does this once at boot and retries it a few
 * times on its own; this is the operator's way to ask for it at any point,
 * without restarting either machine. No control lease: it fixes input rather
 * than being input, and the operator who is locked out is exactly who needs it.
 */
esp_err_t api_hid_reattach_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    usb_hid_reattach();
    return send_ok(req);
}

esp_err_t api_hid_move_post(httpd_req_t *req)
{
    esp_err_t gate;
    if (!agent_allowed(req, &gate)) {
        return gate;
    }
    if (!usb_hid_ready()) {
        return send_json_error(req, "409 Conflict", "no USB target attached");
    }
    cJSON *j = read_json_body(req);
    const cJSON *jx = j ? cJSON_GetObjectItem(j, "x") : NULL;
    const cJSON *jy = j ? cJSON_GetObjectItem(j, "y") : NULL;
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "x and y are required (0..32767)");
    }
    usb_hid_mouse_abs(0, clamp_abs(jx->valuedouble), clamp_abs(jy->valuedouble), 0, 0);
    cJSON_Delete(j);
    return send_ok(req);
}

esp_err_t api_hid_click_post(httpd_req_t *req)
{
    esp_err_t gate;
    if (!agent_allowed(req, &gate)) {
        return gate;
    }
    if (!usb_hid_ready()) {
        return send_json_error(req, "409 Conflict", "no USB target attached");
    }
    cJSON *j = read_json_body(req);
    const cJSON *jx = j ? cJSON_GetObjectItem(j, "x") : NULL;
    const cJSON *jy = j ? cJSON_GetObjectItem(j, "y") : NULL;
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "x and y are required (0..32767)");
    }
    const uint16_t x = clamp_abs(jx->valuedouble);
    const uint16_t y = clamp_abs(jy->valuedouble);
    const uint8_t btn = parse_button(j);
    cJSON_Delete(j);
    /* Press then release at the same spot. Button edges are never coalesced, so
     * the two reports both reach the target as a real click. */
    usb_hid_mouse_abs(btn, x, y, 0, 0);
    usb_hid_mouse_abs(0, x, y, 0, 0);
    return send_ok(req);
}

/* POST {modifier?, keys:[usage,...], hold_ms?} - press a chord, then release.
 * Raw HID usage codes; the caller (e.g. an MCP server) owns the names. */
esp_err_t api_hid_key_post(httpd_req_t *req)
{
    esp_err_t gate;
    if (!agent_allowed(req, &gate)) {
        return gate;
    }
    if (!usb_hid_ready()) {
        return send_json_error(req, "409 Conflict", "no USB target attached");
    }
    cJSON *j = read_json_body(req);
    if (!j) {
        return send_json_error(req, "400 Bad Request", "expected JSON {keys:[...]}");
    }
    const cJSON *jm = cJSON_GetObjectItem(j, "modifier");
    const uint8_t modifier = cJSON_IsNumber(jm) ? (uint8_t)jm->valueint : 0;
    uint8_t kc[6] = {0};
    int n = 0;
    const cJSON *keys = cJSON_GetObjectItem(j, "keys");
    if (cJSON_IsArray(keys)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, keys)
        {
            if (n < 6 && cJSON_IsNumber(e)) {
                kc[n++] = (uint8_t)e->valueint;
            }
        }
    }
    cJSON_Delete(j);
    if (n == 0 && modifier == 0) {
        return send_json_error(req, "400 Bad Request", "no keys or modifier given");
    }
    /* Enqueue press then release and return at once; the HID worker paces the
     * two reports over USB (~one poll apart), which is enough for the target to
     * register the chord. Never block the server task here - it handles every
     * request, so a wait stalls the whole interface and piles up TLS
     * handshakes behind it. */
    const uint8_t none[6] = {0};
    usb_hid_keyboard(modifier, kc);
    usb_hid_keyboard(0, none);
    return send_ok(req);
}

/*
 * POST {press:["a"] | "a", hat:"up", ms:100} - tap gamepad buttons and/or a hat
 * direction: press, hold (default 100 ms, at most 2 s), let go. Or {buttons, hat,
 * lx, ly, rx, ry} as numbers to set the whole state and leave it held; send
 * {buttons:0} to let go. Names are the Switch's: a b x y l r zl zr minus plus
 * lstick rstick home capture; the face buttons are positions (b is the bottom
 * one, an Xbox pad's A), and Xbox words (lb, rt, start, guide...) work too.
 */
esp_err_t api_hid_pad_post(httpd_req_t *req)
{
    esp_err_t gate;
    if (!agent_allowed(req, &gate)) {
        return gate;
    }
    if (!usb_hid_pad_present()) {
        return send_json_error(req, "409 Conflict", "no gamepad: set Settings -> Input -> Gamepad");
    }
    if (!usb_hid_ready()) {
        return send_json_error(req, "409 Conflict", "no USB target attached");
    }
    cJSON *j = read_json_body(req);
    if (!j) {
        return send_json_error(req, "400 Bad Request", "expected JSON {press:[\"a\"]}");
    }
    const cJSON *press = cJSON_GetObjectItem(j, "press");
    const cJSON *jhat = cJSON_GetObjectItem(j, "hat");
    uint8_t hat = USB_HID_PAD_HAT_NONE;
    if (cJSON_IsString(jhat)) {
        hat = usb_hid_pad_hat(jhat->valuestring);
        if (hat == USB_HID_PAD_HAT_NONE) {
            cJSON_Delete(j);
            return send_json_error(req, "400 Bad Request", "unknown hat direction");
        }
    } else if (cJSON_IsNumber(jhat)) {
        hat = (uint8_t)jhat->valueint;
    }
    if (press || (cJSON_IsString(jhat) && !cJSON_GetObjectItem(j, "buttons"))) {
        uint16_t bits = 0;
        const char *bad = NULL;
        if (cJSON_IsString(press)) {
            bits = usb_hid_pad_button(press->valuestring);
            bad = bits ? NULL : press->valuestring;
        } else if (cJSON_IsArray(press)) {
            const cJSON *e;
            cJSON_ArrayForEach(e, press)
            {
                const uint16_t b = cJSON_IsString(e) ? usb_hid_pad_button(e->valuestring) : 0;
                if (!b) {
                    bad = cJSON_IsString(e) ? e->valuestring : "?";
                    break;
                }
                bits |= b;
            }
        }
        if (bad) {
            char msg[64];
            snprintf(msg, sizeof(msg), "unknown button '%.24s'", bad);
            cJSON_Delete(j);
            return send_json_error(req, "400 Bad Request", msg);
        }
        const cJSON *ms = cJSON_GetObjectItem(j, "ms");
        const int hold = cJSON_IsNumber(ms) ? ms->valueint : 100;
        cJSON_Delete(j);
        usb_hid_pad_tap(bits, hat, (uint16_t)(hold < 1 ? 1 : (hold > 2000 ? 2000 : hold)));
        return send_ok(req);
    }
    const cJSON *jb = cJSON_GetObjectItem(j, "buttons");
    uint8_t axis[4] = {USB_HID_PAD_CENTER, USB_HID_PAD_CENTER, USB_HID_PAD_CENTER,
                       USB_HID_PAD_CENTER};
    static const char *const axis_names[4] = {"lx", "ly", "rx", "ry"};
    for (int i = 0; i < 4; i++) {
        const cJSON *a = cJSON_GetObjectItem(j, axis_names[i]);
        if (cJSON_IsNumber(a)) {
            axis[i] = (uint8_t)(a->valueint < 0 ? 0 : (a->valueint > 255 ? 255 : a->valueint));
        }
    }
    const uint16_t buttons = cJSON_IsNumber(jb) ? (uint16_t)jb->valueint : 0;
    cJSON_Delete(j);
    usb_hid_pad(buttons, hat, axis[0], axis[1], axis[2], axis[3]);
    return send_ok(req);
}

/* POST {text} - type a US-ASCII string. Capped to stay well within the HID
 * worker's queue (the handler enqueues two reports per character and returns);
 * longer text is sent in several calls. */
#define HID_TYPE_MAX 80
esp_err_t api_hid_type_post(httpd_req_t *req)
{
    esp_err_t gate;
    if (!agent_allowed(req, &gate)) {
        return gate;
    }
    if (!usb_hid_ready()) {
        return send_json_error(req, "409 Conflict", "no USB target attached");
    }
    cJSON *j = read_json_body(req);
    const cJSON *jt = j ? cJSON_GetObjectItem(j, "text") : NULL;
    if (!cJSON_IsString(jt)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "text is required");
    }
    const char *text = jt->valuestring;
    size_t len = strlen(text);
    if (len > HID_TYPE_MAX) {
        len = HID_TYPE_MAX;
    }
    /* Enqueue press/release per character and let the HID worker pace them over
     * USB (this is exactly what the console's paste does). No vTaskDelay here:
     * blocking the single server task stalls every other request and backs up
     * the TLS handshake pool. The length cap keeps the burst within the queue. */
    const uint8_t none[6] = {0};
    for (size_t i = 0; i < len; i++) {
        uint8_t usage;
        bool shift;
        if (!rb_ascii_usage(text[i], &usage, &shift)) {
            continue; /* silently skip characters the US layout can't type */
        }
        const uint8_t kc[6] = {usage, 0, 0, 0, 0, 0};
        usb_hid_keyboard(shift ? HID_MOD_LSHIFT : 0x00, kc); /* press */
        usb_hid_keyboard(0, none);                           /* release */
    }
    cJSON_Delete(j);
    return send_ok(req);
}
