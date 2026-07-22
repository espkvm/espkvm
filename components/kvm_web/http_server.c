/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "esp_https_server.h"

#include "capture.h"
#include "kvm_auth.h"
#include "kvm_caps.h"
#include "kvm_settings.h"
#include "kvm_thermal.h"
#include "kvm_tls.h"
#include "usb_hid.h"
#include "video_frame.h"

static const char *TAG = "web";

/** Body limit for settings writes; the whole table serialises well under this. */
#define API_BODY_MAX 4096

/** Send a malloc'd JSON document and free it. NULL means the generator failed. */
static esp_err_t send_json_owned(httpd_req_t *req, char *json)
{
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[192];
    /* cJSON for a single short field is not worth the allocation, but the message
     * still has to survive quoting. */
    size_t o = 0;
    body[o++] = '{';
    o += (size_t)snprintf(body + o, sizeof(body) - o, "\"error\":\"");
    for (const char *p = message; *p && o < sizeof(body) - 8; p++) {
        if (*p == '"' || *p == '\\') {
            body[o++] = '\\';
        }
        body[o++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    }
    o += (size_t)snprintf(body + o, sizeof(body) - o, "\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, body, o);
}

static esp_err_t api_capabilities_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_json_owned(req, kvm_caps_to_json());
}

static esp_err_t api_settings_schema_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_json_owned(req, kvm_settings_schema_json());
}

static esp_err_t api_settings_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_json_owned(req, kvm_settings_values_json());
}

static esp_err_t api_settings_put(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (req->content_len <= 0 || req->content_len >= API_BODY_MAX) {
        return send_json_error(req, "413 Payload Too Large", "settings body too large");
    }
    char *body = malloc((size_t)req->content_len + 1u);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (n <= 0) {
            free(body);
            return send_json_error(req, "400 Bad Request", "truncated body");
        }
        got += n;
    }
    body[got] = '\0';

    char err[128];
    esp_err_t res = kvm_settings_apply_json(body, err, sizeof(err));
    free(body);
    if (res != ESP_OK) {
        return send_json_error(req, res == ESP_ERR_NOT_FOUND ? "404 Not Found" : "400 Bad Request",
                               err[0] ? err : "invalid settings");
    }
    return send_json_owned(req, kvm_settings_values_json());
}

/* Defined with the video WebSocket below; reported here so a viewer that the
 * device still believes in is visible from the API rather than only as an
 * encoder that will not go idle. */
static uint8_t s_video_client_count;
/** Multipart /stream responses in flight, counted separately from WebSocket
 *  subscribers so "who is holding the encoder busy" has an answer. */
static volatile int s_stream_workers;

static esp_err_t api_video_status_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    kvm_video_status_t st;
    capture_status_get(&st);

    /* The codec that is running, which is not always the one that was asked
     * for: an encoder that fails to start falls back rather than going dark. */
    const char *codec = "none";
    switch (video_frame_payload()) {
    case VIDEO_PAYLOAD_JPEG:
        codec = "mjpeg";
        break;
    case VIDEO_PAYLOAD_H264:
        codec = "h264";
        break;
    default:
        break;
    }

    char body[288];
    int n = snprintf(body, sizeof(body),
                     "{\"signal\":%s,\"width\":%u,\"height\":%u,\"interlaced\":%s,"
                     "\"fps\":%u.%02u,\"skippedFps\":%u.%02u,\"kbps\":%u,"
                     "\"encodeUs\":%u,\"encoderBusyPct\":%u,"
                     "\"modeChanges\":%u,\"sysStatus\":%u,\"viewers\":%d,"
                     "\"wsClients\":%u,\"imgClients\":%d,\"codec\":\"%s\"}",
                     st.signal ? "true" : "false", (unsigned)st.hres, (unsigned)st.vres,
                     st.interlaced ? "true" : "false", (unsigned)(st.fps_x100 / 100u),
                     (unsigned)(st.fps_x100 % 100u), (unsigned)(st.skipped_fps_x100 / 100u),
                     (unsigned)(st.skipped_fps_x100 % 100u), (unsigned)st.kbps,
                     (unsigned)st.encode_us, (unsigned)st.encoder_busy_pct,
                     (unsigned)st.mode_changes, (unsigned)st.sys_status,
                     video_frame_viewer_count(), (unsigned)s_video_client_count,
                     s_stream_workers, codec);
    if (n <= 0 || n >= (int)sizeof(body)) {
        return send_json_error(req, "500 Internal Server Error", "status too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

/*
 * What is running and whether it can be replaced. The console needs the
 * version to tell the operator what they have before offering an update, and
 * needs to know an update is possible at all - a single-slot partition table
 * makes the question moot.
 */
static esp_err_t api_system_info_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const float temp_c = kvm_thermal_celsius();

    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"project\":\"%s\",\"version\":\"%s\",\"built\":\"%s %s\","
                     "\"idf\":\"%s\",\"partition\":\"%s\",\"updatable\":%s,"
                     "\"uptimeSeconds\":%llu,\"heapFree\":%u,\"psramFree\":%u,"
                     "\"tempC\":%d.%01u,\"thermal\":\"%s\"}",
                     app->project_name, app->version, app->date, app->time, app->idf_ver,
                     running ? running->label : "?", next ? "true" : "false",
                     (unsigned long long)(esp_timer_get_time() / 1000000),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (int)temp_c,
                     (unsigned)((temp_c < 0 ? -temp_c : temp_c) * 10.0f) % 10u,
                     kvm_thermal_state_name(kvm_thermal_state()));
    if (n <= 0 || n >= (int)sizeof(body)) {
        return send_json_error(req, "500 Internal Server Error", "system info too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

/*
 * Receive a firmware image and boot it next.
 *
 * The image is streamed straight into the inactive slot - it is larger than
 * any buffer this device can spare - and the active slot is untouched until
 * the whole thing has been written and verified. Combined with the rollback
 * the bootloader performs when a new image never confirms itself, the worst
 * outcome of a bad upload is one reboot back into what was running before.
 */
static esp_err_t api_system_update_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return send_json_error(req, "409 Conflict", "no second app slot to write into");
    }
    if (req->content_len <= 0) {
        return send_json_error(req, "400 Bad Request", "empty body");
    }
    if ((size_t)req->content_len > target->size) {
        return send_json_error(req, "413 Payload Too Large", "image larger than the app slot");
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, (size_t)req->content_len, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "firmware update: %d bytes into %s", req->content_len, target->label);

    char chunk[2048];
    int received = 0;
    while (received < req->content_len) {
        const int want = (int)sizeof(chunk) < (req->content_len - received)
                             ? (int)sizeof(chunk)
                             : (req->content_len - received);
        const int n = httpd_req_recv(req, chunk, (size_t)want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /* A gap in a multi-megabyte upload is normal, not a failure: the
             * sender is simply slower than the socket timeout. */
            continue;
        }
        if (n <= 0) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "update aborted after %d of %d bytes (recv %d)", received,
                     req->content_len, n);
            return send_json_error(req, "400 Bad Request", "upload was cut short");
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
        }
        received += n;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        /* Most often the image is not a valid application: a truncated upload
         * or the wrong file entirely. */
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return send_json_error(req, "400 Bad Request",
                               err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a valid firmware image"
                                                                  : esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "update written to %s, restarting", target->label);
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"status\":\"written\",\"restarting\":true}");

    /* Let the response reach the browser before the device disappears. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return sent;
}

/*
 * Restart on request.
 *
 * Useful in itself - a setting marked "takes effect after a restart" needs a
 * way to take effect - and it is the only way to reach the password-reset
 * window without a hand on the board: a software restart leaves the boot
 * strapping pins as they were, so the button can already be held when it
 * happens, which a hardware reset would read as "boot into the ROM loader".
 */
static esp_err_t api_system_restart_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    ESP_LOGW(TAG, "restart requested");
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    /* Let the answer reach the browser before the device goes away. */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return sent;
}

static esp_err_t api_settings_reset_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    (void)kvm_settings_reset();
    return send_json_owned(req, kvm_settings_values_json());
}

/* Long MJPEG response must not run on the httpd select() thread; see httpd_req_async_handler_begin(). */
#define STREAM_WORKER_STACK (12 * 1024)
#define STREAM_WORKER_PRIO (tskIDLE_PRIORITY + 5)

/* The console is embedded already compressed: it costs a third of the flash
 * and a third of the transfer, and the browser does the decompression. */
extern const char index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const char index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const char favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const char favicon_ico_end[] asm("_binary_favicon_ico_end");

/*
 * Control channel, version 2. All frames are binary and fixed length.
 *
 *   client -> device
 *     0x01 abs mouse   buttons, x:u16, y:u16 (0..32767), wheel:i8, pan:i8
 *     0x02 rel mouse   buttons, dx:i16, dy:i16, wheel:i8, pan:i8
 *     0x03 keyboard    modifier, six key usages
 *     0x04 consumer    usage:u16
 *     0x05 release all
 *     0x06 ping
 *   device -> client
 *     0x81 status      flags:u8 (bit0 target attached), leds:u8
 *     0x82 pong
 */
enum {
    WS_C2D_MOUSE_ABS = 0x01,
    WS_C2D_MOUSE_REL = 0x02,
    WS_C2D_KEYBOARD = 0x03,
    WS_C2D_CONSUMER = 0x04,
    WS_C2D_RELEASE_ALL = 0x05,
    WS_C2D_PING = 0x06,

    WS_D2C_STATUS = 0x81,
    WS_D2C_PONG = 0x82,
};

static SemaphoreHandle_t s_ws_mu;
static int s_ws_fd = -1;
static httpd_handle_t s_httpd;
/** Port 80, answering only with redirects while TLS is on. */
static httpd_handle_t s_redirect_httpd;

/** Defined with the video channel below; the session close callback needs it. */
static void video_remove_client(int fd);

static void ws_send_binary(int fd, const uint8_t *data, size_t len)
{
    if (!s_httpd || fd < 0) {
        return;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    (void)httpd_ws_send_frame_async(s_httpd, fd, &frame);
}

static void ws_send_pong(int fd)
{
    const uint8_t pong[] = {WS_D2C_PONG};
    ws_send_binary(fd, pong, sizeof(pong));
}

/** Push target-attached state and keyboard LEDs to the connected client. */
static void ws_send_status(void)
{
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    const int fd = s_ws_fd;
    xSemaphoreGive(s_ws_mu);
    if (fd < 0) {
        return;
    }
    const uint8_t msg[] = {WS_D2C_STATUS, usb_hid_ready() ? 1u : 0u, usb_hid_leds()};
    ws_send_binary(fd, msg, sizeof(msg));
}

static void on_hid_leds(uint8_t leds, void *user)
{
    (void)leds;
    (void)user;
    ws_send_status();
}

/*
 * Registering a close callback makes the application responsible for the
 * socket: httpd_sess.c calls close(fd) only when no callback is set. Without
 * the close() below every connection leaks its descriptor, and after about
 * twenty of them - a handful of page reloads - the device still answers ping
 * but the web interface is gone for good.
 */
static void http_sess_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    bool was_control_session = false;

    if (s_ws_mu && xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(500)) == pdTRUE) {
        was_control_session = (sockfd == s_ws_fd);
        if (was_control_session) {
            s_ws_fd = -1;
        }
        xSemaphoreGive(s_ws_mu);
    }

    /*
     * The application closes it, always. esp_http_server hands that duty over
     * as soon as a close callback is registered, and not doing it exhausted
     * the session pool once already - and again, worse, when this was made
     * conditional on TLS: esp-tls frees its own context but leaves the
     * descriptor, so the whole process ran out of sockets and the device went
     * silent on both ports while still answering ping.
     */
    close(sockfd);

    /* A dropped control session leaves whatever was held down still pressed on
     * the target, and there is no longer anyone able to release it. */
    if (was_control_session) {
        usb_hid_release_all();
    }
    video_remove_client(sockfd);
    kvm_auth_forget_socket(sockfd);
}

static void ws_take_session(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (s_ws_fd >= 0 && s_ws_fd != fd) {
        httpd_sess_trigger_close(req->handle, s_ws_fd);
    }
    s_ws_fd = fd;
    xSemaphoreGive(s_ws_mu);
}

static esp_err_t ws_input_handler(httpd_req_t *req)
{
    if (!kvm_auth_socket_ok(httpd_req_to_sockfd(req))) {
        return kvm_auth_reject_ws(req);
    }
    /*
     * The control session is claimed by the first frame, not at the handshake:
     * the handler is never called for the upgrade, so a session claimed there
     * would never be claimed at all - which is how the device ended up with
     * nowhere to send target and LED state, and the console showed "no target
     * on USB" for a target that was plainly attached.
     */
    ws_take_session(req);

    httpd_ws_frame_t pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "ws frame len query: %s", esp_err_to_name(ret));
        return ret;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        return ESP_OK;
    }

    uint8_t buf[32];
    if (pkt.len > sizeof(buf)) {
        ESP_LOGW(TAG, "ws frame too large %zu", (size_t)pkt.len);
        return ESP_OK;
    }
    if (pkt.len) {
        memset(buf, 0, sizeof(buf));
        pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_OK;
    }
    int my_fd = httpd_req_to_sockfd(req);
    bool ours;
    if (s_ws_fd == my_fd) {
        /* Already the registered owner. */
        ours = true;
    } else if (s_ws_fd < 0) {
        /* No owner registered (handshake's s_ws_fd was cleared by a spurious
         * close_fn, e.g. esp_http_server's internal session recycling).
         * Lazily claim this connection so HID input is not lost. */
        s_ws_fd = my_fd;
        ours = true;
    } else {
        /* A different client is the active owner, keep single-client enforcement. */
        ours = false;
    }
    xSemaphoreGive(s_ws_mu);

    if (!ours) {
        return ESP_OK;
    }

    switch (buf[0]) {
    case WS_C2D_MOUSE_ABS:
        if (pkt.len >= 8) {
            /* Coordinates are 0..32767 on both axes, so the client never has to
             * know the capture resolution and a mode change mid-drag cannot
             * shift the pointer. */
            const uint16_t x = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
            const uint16_t y = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
            usb_hid_mouse_abs(buf[1], x, y, (int8_t)buf[6], (int8_t)buf[7]);
        }
        break;
    case WS_C2D_MOUSE_REL:
        if (pkt.len >= 8) {
            const int16_t dx = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
            const int16_t dy = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
            usb_hid_mouse_rel(buf[1], dx, dy, (int8_t)buf[6], (int8_t)buf[7]);
        }
        break;
    case WS_C2D_KEYBOARD:
        if (pkt.len >= 8) {
            usb_hid_keyboard(buf[1], &buf[2]);
        }
        break;
    case WS_C2D_CONSUMER:
        if (pkt.len >= 3) {
            usb_hid_consumer((uint16_t)buf[1] | ((uint16_t)buf[2] << 8));
        }
        break;
    case WS_C2D_RELEASE_ALL:
        usb_hid_release_all();
        break;
    case WS_C2D_PING:
        ws_send_pong(my_fd);
        /* Doubles as "what is the current state?", which is what a client wants
         * right after connecting. */
        ws_send_status();
        break;
    default:
        ESP_LOGD(TAG, "unknown ws message 0x%02x", buf[0]);
        break;
    }

    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_gz_end - index_html_gz_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, index_html_gz_start, len);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    const size_t len = (size_t)(favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, favicon_ico_start, len);
}

/** Serialises multipart senders; see where it is taken for why. */
static SemaphoreHandle_t s_xmit_mu;

/**
 * While httpd_req_async_handler_begin() is in effect, the session fd is not in the server's select()
 * set, so disconnects are invisible until we send or call httpd_req_async_handler_complete(), that
 * can exhaust session slots (accept errno 23 / ENFILE). Peek the TCP socket from this task instead.
 */
static bool stream_peer_disconnected(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return true;
    }
    unsigned char b;
    int n = recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
    if (n == 0) {
        return true;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return true;
    }
    return false;
}

static void stream_worker_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char hdr[96];

    video_frame_viewer_enter();
    s_stream_workers++;
    /* Send whatever is on screen right now rather than waiting for the next
     * publish: on a still screen the encoder deliberately stops publishing, so
     * a new viewer would otherwise stare at nothing. The loop below then waits
     * on the sequence number as usual, so this replays the frame only once. */
    uint32_t last_seq = video_frame_seq() - 1u;

    while (1) {
        if (!video_frame_wait_new(last_seq, 500)) {
            if (stream_peer_disconnected(req)) {
                break;
            }
            continue;
        }

        video_frame_ref_t f;
        if (!video_frame_acquire(&f)) {
            last_seq = video_frame_seq();
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last_seq = f.seq;
        if (f.payload != VIDEO_PAYLOAD_JPEG) {
            /*
             * The codec changed under this viewer. multipart/x-mixed-replace
             * carries still images and nothing else, so end the response rather
             * than hold a connection open that will never carry another frame:
             * the client reconnects and gets the 409 that explains itself.
             */
            video_frame_release(&f);
            ESP_LOGI(TAG, "stream ended: codec is no longer MJPEG");
            break;
        }

        /*
         * One sender at a time: esp_http_server keeps a single scratch buffer
         * per server, so two tasks writing chunks to different sockets
         * interleave into each other's output.
         */
        if (xSemaphoreTake(s_xmit_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
            video_frame_release(&f);
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        int hl = snprintf(hdr, sizeof(hdr),
                          "--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          f.len);
        esp_err_t se = ESP_FAIL;
        if (hl > 0 && hl < (int)sizeof(hdr)) {
            se = httpd_resp_send_chunk(req, hdr, hl);
            if (se == ESP_OK) {
                se = httpd_resp_send_chunk(req, (const char *)f.data, f.len);
            }
            if (se == ESP_OK) {
                se = httpd_resp_send_chunk(req, "\r\n", 2);
            }
        }
        video_frame_release(&f);
        xSemaphoreGive(s_xmit_mu);

        if (se != ESP_OK) {
            ESP_LOGD(TAG, "stream end %s", esp_err_to_name(se));
            break;
        }
    }
    s_stream_workers--;
    video_frame_viewer_leave();
    const int fd = httpd_req_to_sockfd(req);
    httpd_handle_t server = req->handle;
    httpd_resp_sendstr_chunk(req, NULL);
    if (httpd_req_async_handler_complete(req) != ESP_OK) {
        ESP_LOGW(TAG, "stream async complete failed on fd %d", fd);
    }
    /*
     * Completing the async handler hands the socket back to the server's select
     * set, but when the viewer has already gone the server sees ENOTCONN and
     * keeps the session anyway - the descriptor is never reused and the pool
     * runs out after a dozen page reloads, leaving the device pingable with a
     * dead web interface. A finished multipart stream is not reusable for
     * keep-alive either, so closing it here is right regardless.
     */
    if (server && fd >= 0) {
        (void)httpd_sess_trigger_close(server, fd);
    }
    vTaskDelete(NULL);
}

/*
 * Video over WebSocket.
 *
 * multipart/x-mixed-replace can only carry still images, so H.264 needs a
 * different channel anyway - and the same channel fixes a defect of the
 * multipart one: when the source stops sending, a browser holds the last
 * frame forever with no way to say so. Here the absence of a signal is a
 * message like any other.
 *
 * Every frame is prefixed with a fixed 12-byte header:
 *
 *   0      magic 'K'
 *   1      payload type: 1 JPEG, 2 H.264 Annex-B
 *   2..3   width, height of this frame
 *   4..7   sequence number, so a client can see what it missed
 *   8..11  milliseconds since boot, for latency measurement
 *
 * The type is per frame rather than per connection because the codec can
 * change under a running viewer, and a decoder fed the wrong bytes fails in
 * ways that are hard to read.
 */
#define VIDEO_HDR_LEN 12
#define VIDEO_MAGIC 0x4bu

#define VIDEO_MAX_CLIENTS 4

static int s_video_fds[VIDEO_MAX_CLIENTS];
/**
 * A viewer that has not been sent a decodable frame yet. H.264 P-frames are
 * meaningless without the IDR they refer to, so a client that joins mid-GOP
 * waits here - a few tens of milliseconds - until the keyframe it asked for
 * comes out of the encoder.
 */
static bool s_video_need_key[VIDEO_MAX_CLIENTS];
static uint8_t s_video_client_count;
static SemaphoreHandle_t s_video_mu;

static void video_add_client(int fd)
{
    /*
     * Waited for rather than attempted: the table is only ever held for the
     * few instructions that read or write it - never across a send - so this
     * cannot block for long, and a subscription silently dropped on a lock
     * timeout leaves a viewer receiving nothing.
     */
    if (!s_video_mu || xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] == fd) {
            xSemaphoreGive(s_video_mu);
            return;
        }
    }
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] < 0) {
            s_video_fds[i] = fd;
            s_video_need_key[i] = true;
            if (s_video_client_count++ == 0) {
                /* First viewer: the encoder is idle until someone is reading. */
                video_frame_viewer_enter();
            }
            break;
        }
    }
    xSemaphoreGive(s_video_mu);
    /* Also when this is not the first viewer: the newcomer needs a frame it
     * can decode from, whoever else is already watching. */
    video_frame_request_keyframe();
}

static void video_drop_client_locked(int index)
{
    s_video_fds[index] = -1;
    s_video_need_key[index] = false;
    if (s_video_client_count > 0 && --s_video_client_count == 0) {
        video_frame_viewer_leave();
    }
}

static void video_remove_client(int fd)
{
    /*
     * This one must not fail: it runs when a session closes, and a removal
     * that gives up leaves the encoder running for a viewer that has gone -
     * at 1080p that is the whole chip busy for nobody.
     */
    if (!s_video_mu || xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] == fd) {
            video_drop_client_locked(i);
            break;
        }
    }
    xSemaphoreGive(s_video_mu);
}

/*
 * One task pushes to every subscribed socket. The websocket upgrade is handled
 * by esp_http_server itself, so unlike the multipart stream this must not be
 * wrapped in an async request handler - doing so leaves the handshake
 * unfinished and no frame ever reaches the client.
 */
static void video_pump_task(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    uint8_t *packet = NULL;
    size_t packet_cap = 0;
    uint32_t last_seq = 0;

    for (;;) {
        if (s_video_client_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            last_seq = video_frame_seq();
            continue;
        }
        if (video_frame_seq() == last_seq) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        video_frame_ref_t f;
        if (!video_frame_acquire(&f)) {
            last_seq = video_frame_seq();
            continue;
        }
        last_seq = f.seq;

        if (packet_cap < VIDEO_HDR_LEN + f.len) {
            uint8_t *grown = heap_caps_realloc(packet, VIDEO_HDR_LEN + f.len, MALLOC_CAP_SPIRAM);
            if (!grown) {
                video_frame_release(&f);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            packet = grown;
            packet_cap = VIDEO_HDR_LEN + f.len;
        }

        kvm_video_status_t vs;
        capture_status_get(&vs);
        const uint32_t pts = (uint32_t)(esp_timer_get_time() / 1000);
        packet[0] = VIDEO_MAGIC;
        packet[1] = (uint8_t)f.payload;
        packet[2] = (uint8_t)(vs.hres & 0xff);
        packet[3] = (uint8_t)(vs.hres >> 8);
        packet[4] = (uint8_t)(vs.vres & 0xff);
        packet[5] = (uint8_t)(vs.vres >> 8);
        packet[6] = (uint8_t)(f.seq & 0xff);
        packet[7] = (uint8_t)((f.seq >> 8) & 0xff);
        packet[8] = (uint8_t)((f.seq >> 16) & 0xff);
        packet[9] = (uint8_t)((f.seq >> 24) & 0xff);
        packet[10] = (uint8_t)(pts & 0xff);
        packet[11] = (uint8_t)((pts >> 8) & 0xff);
        memcpy(packet + VIDEO_HDR_LEN, f.data, f.len);
        const size_t packet_len = VIDEO_HDR_LEN + f.len;
        const bool keyframe = f.keyframe;
        video_frame_release(&f);

        httpd_ws_frame_t frame = {
            .final = true,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = packet,
            .len = packet_len,
        };

        /*
         * Decide who to send to under the lock, then send outside it. A send
         * to a viewer on a slow link blocks for as long as the socket takes,
         * and holding the table across that stalls every session close for the
         * same duration - which is how a departed viewer stayed counted and
         * kept the encoder running.
         */
        int targets[VIDEO_MAX_CLIENTS];
        int target_count = 0;
        if (xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
            if (s_video_fds[i] < 0) {
                continue;
            }
            if (s_video_need_key[i]) {
                if (!keyframe) {
                    continue;
                }
                s_video_need_key[i] = false;
            }
            targets[target_count++] = s_video_fds[i];
        }
        xSemaphoreGive(s_video_mu);

        for (int i = 0; i < target_count; i++) {
            if (httpd_ws_send_frame_async(server, targets[i], &frame) != ESP_OK) {
                /* A viewer that has gone stops being counted, which is also
                 * what lets the encoder go idle again. */
                video_remove_client(targets[i]);
            }
        }
    }
}

/*
 * Websockets are authenticated here, before the upgrade is answered.
 *
 * esp_http_server does not call the URI handler for the handshake at all - it
 * responds and returns - so a handler has no chance to refuse a connection,
 * and a 401 written from one lands inside an already-open socket as a
 * malformed frame. This callback is the one place where the request still has
 * its headers and the answer can still be "no".
 */
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        ESP_LOGW(TAG, "websocket refused: no session");
        return ESP_FAIL;
    }
    /* Frames carry no headers, so the socket itself is the credential from
     * here on. */
    kvm_auth_mark_socket(httpd_req_to_sockfd(req));
    return ESP_OK;
}

static esp_err_t video_ws_handler(httpd_req_t *req)
{
    /* Only frames reach here; the handshake was answered, and authenticated,
     * by ws_pre_handshake(). A viewer subscribes with its first message. */
    if (!kvm_auth_socket_ok(httpd_req_to_sockfd(req))) {
        return kvm_auth_reject_ws(req);
    }

    httpd_ws_frame_t pkt = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        video_remove_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    uint8_t discard[8];
    if (pkt.len) {
        pkt.payload = discard;
        (void)httpd_ws_recv_frame(req, &pkt, sizeof(discard));
    }
    video_add_client(httpd_req_to_sockfd(req));
    return ESP_OK;
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (!video_frame_store_ready()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera starting");
    }
    if (video_frame_payload() != VIDEO_PAYLOAD_JPEG) {
        /* Said plainly rather than by holding a connection open that will never
         * carry a frame: this endpoint is images only. */
        return httpd_resp_send_custom_err(req, "409 Conflict",
                                          "the device is encoding H.264; use the /video "
                                          "WebSocket or select the MJPEG codec");
    }
    /* Browsers will often show only the first JPEG unless the stream is explicitly uncached. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    /* Hint for reverse proxies (harmless if unused). */
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) {
        return res;
    }

    httpd_req_t *async_req = NULL;
    res = httpd_req_async_handler_begin(req, &async_req);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "stream async begin: %s", esp_err_to_name(res));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream busy");
    }

    BaseType_t created =
        xTaskCreate(stream_worker_task, "kvm_stream", STREAM_WORKER_STACK, async_req, STREAM_WORKER_PRIO, NULL);
    if (created != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "stream task");
    }
    return ESP_OK;
}

/*
 * Everything that arrives on port 80 while TLS is on is answered with a
 * redirect and nothing else. No URI handler is registered on that server, so
 * every request lands in the 404 handler - which is exactly the "any request"
 * hook this needs.
 */
static esp_err_t redirect_to_https(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    char host[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || !host[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no Host header");
    }
    /* The Host header carries the port this server answers on; the browser must
     * be sent to the TLS one. */
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }

    char location[160];
    const char *uri = req->uri[0] ? req->uri : "/";
    int n = snprintf(location, sizeof(location), "https://%s%s", host, uri);
    if (n <= 0 || n >= (int)sizeof(location)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request too long to redirect");
    }
    httpd_resp_set_status(req, "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static httpd_handle_t start_redirect_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32769; /* the TLS server owns the default */
    cfg.max_uri_handlers = 1;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    cfg.max_req_hdr_len = 1024;

    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no redirect server on port 80");
        return NULL;
    }
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, redirect_to_https);
    return h;
}

httpd_handle_t http_server_start(void)
{
    if (!s_ws_mu) {
        s_ws_mu = xSemaphoreCreateMutex();
        if (!s_ws_mu) {
            ESP_LOGE(TAG, "ws mutex");
            return NULL;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Browsers send >1 KiB of headers; Vite dev proxy forwards them. Default 1024 -> 431. */
    cfg.max_req_hdr_len = 8192;
    cfg.server_port = 80;
    cfg.stack_size = 20 * 1024;
    /* Prefer draining TCP slightly above capture so multipart frames reach the browser. */
    cfg.task_priority = tskIDLE_PRIORITY + 6;
    cfg.send_wait_timeout = 30;
    /* Firmware uploads arrive in bursts; a short receive timeout turns a
     * normal gap into a failed update. */
    cfg.recv_wait_timeout = 30;
    cfg.keep_alive_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.close_fn = http_sess_close_cb;
    /*
     * Sessions must be reclaimable. Upstream disabled LRU purging to protect the
     * long-lived /stream from being evicted, but the server does not free a
     * session whose peer vanished without a clean close - it sees ENOTCONN and
     * keeps it. With purging off those sessions accumulate until the pool is
     * exhausted, and the device stays pingable with a dead web interface after
     * roughly twenty page loads. Purging costs at worst a reconnect of the
     * oldest viewer; not purging costs the whole interface, permanently.
     */
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 12;
    cfg.max_uri_handlers = 24;

    if (kvm_auth_init() != ESP_OK) {
        /* Without a working password store the only safe answer is not to
         * serve at all: a KVM that accepts every password is worse than one
         * that is unreachable. */
        ESP_LOGE(TAG, "authentication could not be initialised; refusing to start the server");
        return NULL;
    }

    httpd_handle_t h = NULL;
    /*
     * TLS is the default because this device is a way into another machine, and
     * because the console needs a secure page for WebCodecs - without it the
     * H.264 decoder does not exist in any browser. It can be switched off for a
     * trusted network, where the memory a TLS session costs is better spent on
     * frames.
     */
    if (kvm_setting_bool("sec_https")) {
        kvm_tls_identity_t id;
        esp_err_t cert_err = kvm_tls_identity_get(&id);
        if (cert_err != ESP_OK) {
            kvm_cap_report(KVM_CAP_HTTPS, false, "no certificate could be generated (%s)",
                           esp_err_to_name(cert_err));
        } else {
            httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();
            ssl.httpd = cfg;
            ssl.port_secure = 443;
            /* Each open TLS session holds buffers even with dynamic allocation
             * on, so fewer of them than plain HTTP allows. */
            ssl.httpd.max_open_sockets = 7;
            ssl.servercert = (const unsigned char *)id.cert_pem;
            ssl.servercert_len = id.cert_len;
            ssl.prvtkey_pem = (const unsigned char *)id.key_pem;
            ssl.prvtkey_len = id.key_len;
            ssl.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
            /*
             * A full handshake costs this chip about a third of a second, and
             * the console opens several connections - the page, the video
             * channel, the control channel. Resumption turns all but the first
             * into a few milliseconds.
             */
            ssl.session_tickets = true;

            esp_err_t err = httpd_ssl_start(&h, &ssl);
            /* esp_https_server copies the PEMs into its own configuration. */
            kvm_tls_identity_free(&id);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "httpd_ssl_start: %s", esp_err_to_name(err));
                h = NULL;
                kvm_cap_report(KVM_CAP_HTTPS, false, "the TLS server did not start (%s)",
                               esp_err_to_name(err));
            } else {
                kvm_cap_report(KVM_CAP_HTTPS, true, NULL);
                s_redirect_httpd = start_redirect_server();
                ESP_LOGI(TAG, "serving on https://, port 80 redirects");
            }
        }
    } else {
        kvm_cap_report(KVM_CAP_HTTPS, true, NULL);
    }

    if (!h && httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start");
        return NULL;
    }
    s_httpd = h;
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        s_video_fds[i] = -1;
    }
    s_xmit_mu = xSemaphoreCreateMutex();
    s_video_mu = xSemaphoreCreateMutex();
    if (s_video_mu) {
        xTaskCreate(video_pump_task, "kvm_video", 4096, h, STREAM_WORKER_PRIO, NULL);
    }
    usb_hid_set_led_callback(on_hid_leds, NULL);

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_register_uri_handler(h, &u_root);
    httpd_uri_t u_favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
    httpd_register_uri_handler(h, &u_favicon);
    httpd_uri_t u_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
    httpd_register_uri_handler(h, &u_stream);
    static const httpd_uri_t api_uris[] = {
        {.uri = "/api/capabilities", .method = HTTP_GET, .handler = api_capabilities_get},
        {.uri = "/api/v1/settings/schema", .method = HTTP_GET, .handler = api_settings_schema_get},
        {.uri = "/api/v1/video/status", .method = HTTP_GET, .handler = api_video_status_get},
        {.uri = "/api/v1/system/info", .method = HTTP_GET, .handler = api_system_info_get},
        {.uri = "/api/v1/system/update", .method = HTTP_POST, .handler = api_system_update_post},
        {.uri = "/api/v1/settings/reset", .method = HTTP_POST, .handler = api_settings_reset_post},
        {.uri = "/api/v1/system/restart", .method = HTTP_POST, .handler = api_system_restart_post},
        {.uri = "/api/v1/settings", .method = HTTP_GET, .handler = api_settings_get},
        {.uri = "/api/v1/settings", .method = HTTP_PUT, .handler = api_settings_put},
    };
    for (size_t i = 0; i < sizeof(api_uris) / sizeof(api_uris[0]); i++) {
        httpd_register_uri_handler(h, &api_uris[i]);
    }

    /* Before anything else is registered: a request that arrives while the
     * password store is still loading must not be served. */
    kvm_auth_register(h);

    httpd_uri_t u_video = {
        .uri = "/video",
        .method = HTTP_GET,
        .handler = video_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake,
    };
    httpd_register_uri_handler(h, &u_video);

    httpd_uri_t u_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_input_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake,
    };
    httpd_register_uri_handler(h, &u_ws);
    return h;
}
