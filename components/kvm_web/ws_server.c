/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebSocket sessions: the write locks every send goes through, the control
 * socket (/ws), session open and close, and the viewer list.
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

static const char *TAG = "web";

/* Control channel, version 2: the messages and the states are in kvm_proto.h,
 * which a client in C can use as well. */

static SemaphoreHandle_t s_ws_mu;
static int s_ws_fd = -1;
/* The mouse buttons as the controlling socket last set them. Pointer moves
   that come over a WebRTC data channel carry these, not their own. */
static volatile uint8_t s_ws_buttons;

/*
 * The sockets that may still be written to, and the lock that says so.
 *
 * esp_http_server frees a session's TLS context in httpd_sess_clear_ctx(),
 * immediately after it calls the close callback below. Sends do not all come
 * from the server's own task - the video senders have one, and the keyboard-LED
 * callback pushes status from the USB task - so a send could be inside
 * mbedtls_ssl_write() on a context that had just been freed. It was: a build
 * with heap poisoning caught the jump through ssl->f_send read back as
 * 0xfefefefe, the pattern IDF fills freed memory with.
 *
 * So every send goes through ws_send_frame(), which takes the session's write
 * lock (tx_lock) and checks the socket is still listed. http_sess_close_cb
 * takes the same lock to unlist it, which also waits out a send already under
 * way before the teardown continues. locked_ssl_send gives up on a write after
 * 5 s, so that wait is bounded.
 *
 * The table only has to hold one server's sessions: max_open_sockets is 12
 * plain and 7 with TLS, and the port 80 redirect server has its own config
 * with neither callback.
 */
#define TX_MAX_SOCKETS 16
static SemaphoreHandle_t s_tx_mu; /* the table; held only to read or change it */
static int s_tx_fds[TX_MAX_SOCKETS];
/* One write lock a session, so a slow viewer holds up only its own socket. A
 * single lock for all of them let one video frame to a slow Wi-Fi client stall
 * the server task's pongs and replies for seconds. The generation tells a
 * waiting sender that its session closed and the slot was reused meanwhile. */
static SemaphoreHandle_t s_tx_slot_mu[TX_MAX_SOCKETS];
static uint32_t s_tx_gen[TX_MAX_SOCKETS];
/*
 * The write lock is a binary semaphore with the recursion counted here, not a
 * FreeRTOS mutex. A mutex lends its holder the priority of whoever waits on
 * ANY mutex it holds, and takes the boost back only when the holder has
 * released all of them. Inside a send, lwIP's core lock is waited on by the
 * Wi-Fi link's priority-22 tasks; the sender got 22, kept it for the rest of
 * a 100 KB frame because this lock was still held, and encrypted at 22 on a
 * core for long enough that the idle task starved: a watchdog reset.
 */
static TaskHandle_t s_tx_slot_owner[TX_MAX_SOCKETS];
static int s_tx_slot_depth[TX_MAX_SOCKETS];

static void tx_slot_take(int slot)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (s_tx_slot_owner[slot] == me) { /* only the owner ever sees itself here */
        s_tx_slot_depth[slot]++;
        return;
    }
    (void)xSemaphoreTake(s_tx_slot_mu[slot], portMAX_DELAY);
    s_tx_slot_owner[slot] = me;
    s_tx_slot_depth[slot] = 1;
}

static void tx_slot_give(int slot)
{
    if (--s_tx_slot_depth[slot] == 0) {
        s_tx_slot_owner[slot] = NULL;
        xSemaphoreGive(s_tx_slot_mu[slot]);
    }
}

/* Take the write lock of fd's session. Returns its slot, or -1 when the
 * session is not listed (closed, or never opened by us). */
static int tx_lock(int fd)
{
    if (!s_tx_mu || fd < 0) {
        return -1;
    }
    int slot = -1;
    uint32_t gen = 0;
    (void)xSemaphoreTakeRecursive(s_tx_mu, portMAX_DELAY);
    for (int i = 0; i < TX_MAX_SOCKETS; i++) {
        if (s_tx_fds[i] == fd) {
            slot = i;
            gen = s_tx_gen[i];
            break;
        }
    }
    xSemaphoreGiveRecursive(s_tx_mu);
    if (slot < 0) {
        return -1;
    }
    tx_slot_take(slot);
    (void)xSemaphoreTakeRecursive(s_tx_mu, portMAX_DELAY);
    const bool same = s_tx_fds[slot] == fd && s_tx_gen[slot] == gen;
    xSemaphoreGiveRecursive(s_tx_mu);
    if (!same) {
        tx_slot_give(slot);
        return -1;
    }
    return slot;
}

static void tx_unlock(int slot)
{
    if (slot >= 0) {
        tx_slot_give(slot);
    }
}
/* The control WebSockets, which is where a notice for everybody goes. */
static int s_ctrl_fds[TX_MAX_SOCKETS];

/**
 * Send one frame, unless the session has started closing. Returns what
 * httpd_ws_send_frame_async() returned, or an error if there was nobody left
 * to send to - callers drop the client either way.
 */
/*
 * The TLS server's send, under the session's write lock.
 *
 * s_tx_fds only covers the sends made from our own tasks. The server writes to
 * the same sessions from its task too - the reply to a WebSocket ping or close,
 * every HTTP response - and a TLS context is not safe to write from two tasks
 * at once. With dynamic buffers it was worse than garbled records: a write
 * frees its output buffer when it finishes, so the other write copied into
 * freed memory. That was the crash in the video pump that came back after the
 * August fix (memcpy inside mbedtls_ssl_write, a wild ssl->out_msg). Every
 * write to a TLS session now comes through here, whoever makes it.
 *
 * It does what the server's own httpd_ssl_send does. The session's transport
 * context is esp_https_server's private struct, whose first member is the
 * esp_tls_t - that part of its layout has not changed since IDF 4.
 */
typedef struct {
    esp_tls_t *tls;
} https_transport_head_t;

/* Waits until the socket can take more, or the deadline passes. The sockets
 * can be non-blocking (the /stream one is), and retrying a full one at once
 * kept the task off the CPU's idle task for seconds: a watchdog reset with
 * a slow viewer. */
static void wait_sendable(int sockfd, bool for_read, int64_t deadline)
{
    const int64_t left = deadline - esp_timer_get_time();
    if (left <= 0) {
        return;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);
    struct timeval tv = {.tv_sec = left / 1000000, .tv_usec = left % 1000000};
    (void)select(sockfd + 1, for_read ? &fds : NULL, for_read ? NULL : &fds, NULL, &tv);
}

/*
 * One TLS write encrypts at a time, device-wide. The cipher runs in software
 * and a write encrypts as much as the socket takes. With one video sender a
 * viewer, three viewers plus a handshake kept both cores busy, and the idle
 * task starved: a watchdog reset. A write gives its turn back while it waits
 * for its socket, so a slow viewer does not hold up the others.
 */
static SemaphoreHandle_t s_tls_write_turn;
/* Set in the video and /stream senders. Only they wait for the turn: one
 * write of theirs is a whole frame (150 KB of MJPEG at 1080p, ~70 ms to
 * encrypt), and a pong or an API reply from the server's task waited behind
 * it while the pointer moves piled up unread. */
__thread bool t_stream_sender;

static int locked_ssl_send(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags)
{
    (void)flags;
    const https_transport_head_t *ctx = httpd_sess_get_transport_ctx(hd, sockfd);
    if (!ctx || !ctx->tls) {
        return HTTPD_SOCK_ERR_FAIL;
    }
    /* httpd ignores a short count and sends the next piece, so the viewer gets
     * a cut WebSocket frame and loses the framing. A slow link (a VPN) times
     * out the 400 ms send often. Finish the buffer, or fail and close. */
    /* Unlisted: only the server's own task writes to it, so no lock needed. */
    const int slot = tx_lock(sockfd);
    const int64_t deadline = esp_timer_get_time() + 5000000; /* 100 KB at 20 KB/s */
    size_t done = 0;
    /* A big write from elsewhere (a page, a download) takes its turn too. */
    const bool turn = s_tls_write_turn && (t_stream_sender || buf_len > 16 * 1024);
    while (done < buf_len && esp_timer_get_time() < deadline) {
        if (turn) {
            (void)xSemaphoreTake(s_tls_write_turn, portMAX_DELAY);
        }
        const ssize_t ret = esp_tls_conn_write(ctx->tls, buf + done, buf_len - done);
        if (turn) {
            xSemaphoreGive(s_tls_write_turn);
        }
        if (ret > 0) {
            done += (size_t)ret;
        } else if (ret != 0 && ret != ESP_TLS_ERR_SSL_WANT_WRITE && ret != ESP_TLS_ERR_SSL_WANT_READ) {
            break;
        } else {
            wait_sendable(sockfd, ret == ESP_TLS_ERR_SSL_WANT_READ, deadline);
        }
    }
    tx_unlock(slot);
    return done == buf_len ? (int)done : HTTPD_SOCK_ERR_FAIL;
}

/* The same for plain HTTP (the hotspot): a blocking send() with a timeout
 * also returns a short count. Found by Zachary Leahan in his fork (3585933). */
static int locked_http_send(httpd_handle_t hd, int sockfd, const char *buf, size_t buf_len, int flags)
{
    (void)hd;
    const int slot = tx_lock(sockfd);
    const int64_t deadline = esp_timer_get_time() + 5000000;
    size_t done = 0;
    while (done < buf_len && esp_timer_get_time() < deadline) {
        const ssize_t ret = send(sockfd, buf + done, buf_len - done, flags);
        if (ret > 0) {
            done += (size_t)ret;
        } else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        } else {
            wait_sendable(sockfd, false, deadline);
        }
    }
    tx_unlock(slot);
    return done == buf_len ? (int)done : HTTPD_SOCK_ERR_FAIL;
}

esp_err_t ws_send_frame(httpd_handle_t server, int fd, httpd_ws_frame_t *frame)
{
    if (!server || fd < 0 || !s_tx_mu) {
        return ESP_FAIL;
    }
    const int slot = tx_lock(fd);
    if (slot < 0) {
        return ESP_ERR_INVALID_STATE; /* closed under us */
    }
    const esp_err_t err = httpd_ws_send_frame_async(server, fd, frame);
    tx_unlock(slot);
    return err;
}

void ws_send_binary(int fd, const uint8_t *data, size_t len)
{
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    (void)ws_send_frame(s_httpd, fd, &frame);
}

esp_err_t kvm_web_ws_send_binary(int fd, const uint8_t *data, size_t len)
{
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    return ws_send_frame(s_httpd, fd, &frame);
}

static void ws_send_pong(int fd)
{
    const uint8_t pong[] = {WS_D2C_PONG};
    ws_send_binary(fd, pong, sizeof(pong));
}

/** Tell a client whether it holds control (see CTRL_* / WS_D2C_CONTROL). */
static void ws_send_control(int fd, uint8_t state)
{
    const uint8_t msg[] = {WS_D2C_CONTROL, state};
    ws_send_binary(fd, msg, sizeof(msg));
}

/* Remember a control socket, so a notice can reach every console. */
static void ctrl_client_add(int fd)
{
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    int free_slot = -1;
    for (int i = 0; i < TX_MAX_SOCKETS; i++) {
        if (s_ctrl_fds[i] == fd) {
            free_slot = -1;
            break;
        }
        if (s_ctrl_fds[i] == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        s_ctrl_fds[free_slot] = fd;
    }
    xSemaphoreGive(s_ws_mu);
}

/** Tell every console what the update is doing. */
void ws_broadcast_update(uint8_t phase, uint8_t percent)
{
    int fds[TX_MAX_SOCKETS];
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    memcpy(fds, s_ctrl_fds, sizeof(fds));
    xSemaphoreGive(s_ws_mu);
    const uint8_t msg[] = {WS_D2C_UPDATE, phase, percent};
    for (int i = 0; i < TX_MAX_SOCKETS; i++) {
        if (fds[i] > 0) {
            ws_send_binary(fds[i], msg, sizeof(msg));
        }
    }
}

/** Target-attached state and keyboard LEDs, to one console. */
static void ws_send_status_to(int fd)
{
    if (fd < 0) {
        return;
    }
    /* bit0: the target has enumerated us. bit1: there is a live bus at all -
       without it the console would say "USB is not active" for a target port
       with no power and for a host that stopped listening alike, and only one
       of those is worth pressing re-plug over. */
    const uint8_t flags = (uint8_t)((usb_hid_ready() ? 1u : 0u) | (usb_hid_bus_alive() ? 2u : 0u));
    const uint8_t msg[] = {WS_D2C_STATUS, flags, usb_hid_leds()};
    ws_send_binary(fd, msg, sizeof(msg));
}

/*
 * The same, to every console. It used to go to the one in control only, so a
 * console that was watching showed the USB icon grey - "no power on the
 * target's port" - until someone pressed "take control", however well the
 * target saw the keyboard.
 */
static void ws_send_status(void)
{
    int fds[TX_MAX_SOCKETS];
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    memcpy(fds, s_ctrl_fds, sizeof(fds));
    xSemaphoreGive(s_ws_mu);
    for (int i = 0; i < TX_MAX_SOCKETS; i++) {
        if (fds[i] > 0) {
            ws_send_status_to(fds[i]);
        }
    }
}

void on_hid_leds(uint8_t leds, void *user)
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
/** A new session may be written to from now on. See s_tx_fds. */
/*
 * ts_only: while the device is on the tailnet, answer only over it. The
 * setup hotspot stays open as the way back in, and so does every link while
 * Tailscale is down: a console that can be reached from nowhere is worse
 * than one that can be reached from the LAN.
 */
static void peer_ip(int fd, char *out, size_t cap);

static bool tailnet_gate_ok(int sockfd)
{
    if (!kvm_setting_bool("ts_only") || !kvm_setting_bool("ts_enable")) {
        return true;
    }
    char ip[48];
    peer_ip(sockfd, ip, sizeof(ip));
    unsigned a = 0, b = 0;
    if (sscanf(ip, "%u.%u.", &a, &b) == 2) {
        if (a == 100 && b >= 64 && b <= 127) {
            return true; /* 100.64.0.0/10, the tailnet */
        }
        if (a == 127) {
            return true;
        }
        if (kvm_wifi_setup_ap_active() || kvm_setting_int("net_mode") == KVM_NET_WIFI_AP) {
            if (strncmp(ip, "192.168.4.", 10) == 0) {
                return true; /* the hotspot */
            }
        }
    } else if (strncasecmp(ip, "fd7a:115c:a1e0:", 15) == 0) {
        return true; /* the tailnet's IPv6 range */
    }
    kvm_ts_status_t st;
    kvm_ts_status(&st);
    if (!st.up) {
        return true;
    }
    ESP_LOGW(TAG, "refused %s: the console answers only over Tailscale", ip);
    return false;
}

esp_err_t http_sess_open_cb(httpd_handle_t hd, int sockfd)
{
    if (sockfd >= 0 && !tailnet_gate_ok(sockfd)) {
        return ESP_FAIL;
    }
    /* No Nagle: a pong, an input echo or the tail of a frame otherwise waits
     * for the ACK of what went before, and the viewer delays that ACK - a
     * steady 200-450 ms on every small message. */
    if (sockfd >= 0) {
        const int one = 1;
        (void)setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    /* A new session starts as the most recently used one, so a burst of
     * connections made an open websocket the oldest and the LRU purge closed
     * it (a viewer dropped by a page load elsewhere). Websockets stay newer. */
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(hd, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (fds[i] != sockfd && httpd_ws_get_fd_info(hd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                (void)httpd_sess_update_lru_counter(hd, fds[i]);
            }
        }
    }
    if (!s_tx_mu || sockfd < 0 || xSemaphoreTakeRecursive(s_tx_mu, portMAX_DELAY) != pdTRUE) {
        return ESP_OK;
    }
    /* Every write to this session - ours and the server's own - now takes its
     * write lock and is finished in full. See locked_ssl_send. */
    (void)httpd_sess_set_send_override(hd, sockfd,
        httpd_sess_get_transport_ctx(hd, sockfd) ? locked_ssl_send : locked_http_send);
    bool listed = false;
    for (int i = 0; i < TX_MAX_SOCKETS; i++) {
        if (s_tx_fds[i] == sockfd) {
            listed = true;
            break;
        }
    }
    if (!listed) {
        for (int i = 0; i < TX_MAX_SOCKETS; i++) {
            if (s_tx_fds[i] < 0) {
                s_tx_fds[i] = sockfd;
                s_tx_gen[i]++;
                break;
            }
        }
    }
    xSemaphoreGiveRecursive(s_tx_mu);
    return ESP_OK;
}

void http_sess_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    bool was_control_session = false;

    /*
     * Nothing may write into this session from here on: the server frees its
     * TLS context as soon as this returns. Unlisting the socket under s_tx_mu
     * stops the next send and waits out the one that may be running. See
     * s_tx_fds.
     */
    const int slot = tx_lock(sockfd);
    if (slot >= 0) {
        (void)xSemaphoreTakeRecursive(s_tx_mu, portMAX_DELAY);
        s_tx_fds[slot] = -1;
        xSemaphoreGiveRecursive(s_tx_mu);
        tx_unlock(slot);
    }

    /*
     * Waited for rather than attempted, like video_remove_client: this one must
     * not fail. The lock is only ever held for the few instructions that read or
     * write s_ws_fd, so it cannot block for long, and giving up costs far more
     * than the wait. s_ws_fd is a bare descriptor number, and the close() below
     * hands that number straight back to the system - so a release that gives up
     * leaves control held by a socket that no longer exists, and the next
     * connection to be given the same number silently inherits it. Either way
     * the operator's keystrokes are dropped with no way back but "Take control".
     */
    if (s_ws_mu && xSemaphoreTake(s_ws_mu, portMAX_DELAY) == pdTRUE) {
        was_control_session = (sockfd == s_ws_fd);
        if (was_control_session) {
            s_ws_fd = -1;
        }
        for (int i = 0; i < TX_MAX_SOCKETS; i++) {
            if (s_ctrl_fds[i] == sockfd) {
                s_ctrl_fds[i] = 0;
            }
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
    serial_api_drop(sockfd);
    audio_api_drop(sockfd);
    kvm_auth_forget_socket(sockfd);
}

esp_err_t ws_input_handler(httpd_req_t *req)
{
    if (!kvm_auth_socket_ok(httpd_req_to_sockfd(req))) {
        return kvm_auth_reject_ws(req);
    }

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
        /* Its payload stays unread, and the next frame would be read from the
         * middle of it: close instead. */
        ESP_LOGW(TAG, "ws frame too large %zu: closing", (size_t)pkt.len);
        return ESP_FAIL;
    }
    if (pkt.len) {
        memset(buf, 0, sizeof(buf));
        pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const int my_fd = httpd_req_to_sockfd(req);
    ctrl_client_add(my_fd);
    const uint8_t op = pkt.len ? buf[0] : 0;

    /* A view-only sign-in watches: it hears the state and the pong, and its
     * input, takeover included, goes nowhere. Checked before the control logic
     * so it can never claim control that happens to be free. */
    if (kvm_auth_socket_view_only(my_fd)) {
        ws_send_control(my_fd, CTRL_VIEW_ONLY);
        if (op == WS_C2D_PING) {
            ws_send_pong(my_fd);
            ws_send_status_to(my_fd);
        }
        return ESP_OK;
    }
    const bool is_input = (op >= WS_C2D_MOUSE_ABS && op <= WS_C2D_RELEASE_ALL) || op == WS_C2D_PAD;

    /*
     * One controlling client at a time, held first-come rather than last-write:
     * a second viewer no longer silently steals the session (or fights the first
     * for it). Control is claimed by the first input from whoever holds it or,
     * when it is free, from whoever acts first; a ping never claims it, so just
     * watching does not take control from someone using it. A client that wants
     * it while another holds it must ask - WS_C2D_TAKEOVER, the console's "Take
     * control" button - which demotes the previous holder to a viewer rather
     * than disconnecting it.
     */
    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_OK;
    }
    int demoted = -1;
    uint8_t ctrl_state;
    if (op == WS_C2D_TAKEOVER) {
        if (s_ws_fd >= 0 && s_ws_fd != my_fd) demoted = s_ws_fd;
        s_ws_fd = my_fd;
        ctrl_state = CTRL_YOU;
    } else if (s_ws_fd == my_fd) {
        ctrl_state = CTRL_YOU;
    } else if (s_ws_fd < 0) {
        /* Free: an input takes it; a ping only reports that it is available. */
        if (is_input) {
            s_ws_fd = my_fd;
            ctrl_state = CTRL_YOU;
        } else {
            ctrl_state = CTRL_FREE;
        }
    } else {
        ctrl_state = CTRL_HELD_OTHER;
    }
    xSemaphoreGive(s_ws_mu);

    /* Whoever was just demoted can no longer lift what it was holding, so lift
     * it here; the new holder re-asserts its own state on the next move. */
    if (demoted >= 0) {
        ws_send_control(demoted, CTRL_HELD_OTHER);
        usb_hid_release_all();
        s_ws_buttons = 0;
    }

    /* Let this client know where it stands, so the console can show a "someone
     * else is in control" state instead of input that silently does nothing. */
    ws_send_control(my_fd, ctrl_state);

    if (ctrl_state != CTRL_YOU) {
        /* View-only: answer a ping so the keepalive and status poll still work,
         * but drop any input. */
        if (op == WS_C2D_PING) {
            ws_send_pong(my_fd);
            ws_send_status_to(my_fd);
        }
        return ESP_OK;
    }

    /* Dispatch on `op` (0 for a zero-length frame), never a raw read of buf[0]:
     * the buffer is only initialised when pkt.len > 0, so switching on buf[0]
     * would act on uninitialised stack for an empty frame - and the unguarded
     * cases below (release-all, ping, takeover) would then fire on garbage. */
    switch (op) {
    case WS_C2D_MOUSE_ABS:
        if (pkt.len >= 8) {
            /* Coordinates are 0..32767 on both axes, so the client never has to
             * know the capture resolution and a mode change mid-drag cannot
             * shift the pointer. */
            const uint16_t x = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
            const uint16_t y = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
            s_ws_buttons = buf[1];
            usb_hid_mouse_abs(buf[1], x, y, (int8_t)buf[6], (int8_t)buf[7]);
        }
        break;
    case WS_C2D_MOUSE_REL:
        if (pkt.len >= 8) {
            const int16_t dx = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
            const int16_t dy = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
            s_ws_buttons = buf[1];
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
    case WS_C2D_PAD:
        if (pkt.len >= 8) {
            usb_hid_pad((uint16_t)buf[1] | ((uint16_t)buf[2] << 8), buf[3], buf[4], buf[5], buf[6],
                        buf[7]);
        }
        break;
    case WS_C2D_RELEASE_ALL:
        usb_hid_release_all();
        s_ws_buttons = 0;
        break;
    case WS_C2D_PING:
        ws_send_pong(my_fd);
        /* Doubles as "what is the current state?", which is what a client wants
         * right after connecting. */
        ws_send_status_to(my_fd);
        break;
    case WS_C2D_TAKEOVER:
        /* Control was already transferred above; nothing more to do. */
        break;
    default:
        ESP_LOGD(TAG, "unknown ws message 0x%02x", buf[0]);
        break;
    }

    return ESP_OK;
}

/*
 * Pointer moves over the console's WebRTC data channel: UDP, unordered, never
 * resent, so a lost packet costs one move rather than holding back the ones
 * behind it, as a lost TCP segment does on the WebSocket. Only moves come this
 * way. Buttons, the wheel and keys stay on the WebSocket, and a move from here
 * takes the buttons the WebSocket last set: a move that comes late can shift
 * the pointer, never press or release anything. It counts only while a socket
 * of the same sign-in holds control.
 */
void kvm_web_webrtc_input(const uint8_t owner[32], const uint8_t *msg, size_t len)
{
    if (len < 8 || (msg[0] != WS_C2D_MOUSE_ABS && msg[0] != WS_C2D_MOUSE_REL) || !s_ws_mu) {
        return;
    }
    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    uint8_t holder[32];
    const bool ours = s_ws_fd >= 0 && kvm_auth_socket_session(s_ws_fd, holder) &&
                      memcmp(holder, owner, 32) == 0;
    const uint8_t buttons = s_ws_buttons;
    xSemaphoreGive(s_ws_mu);
    if (!ours) {
        return;
    }
    if (msg[0] == WS_C2D_MOUSE_ABS) {
        const uint16_t x = (uint16_t)msg[2] | ((uint16_t)msg[3] << 8);
        const uint16_t y = (uint16_t)msg[4] | ((uint16_t)msg[5] << 8);
        usb_hid_mouse_abs(buttons, x, y, 0, 0);
    } else {
        const int16_t dx = (int16_t)((uint16_t)msg[2] | ((uint16_t)msg[3] << 8));
        const int16_t dy = (int16_t)((uint16_t)msg[4] | ((uint16_t)msg[5] << 8));
        usb_hid_mouse_rel(buttons, dx, dy, 0, 0);
    }
}

/* The peer's address as text: IPv4 also when it came in mapped into IPv6. */
static void peer_ip(int fd, char *out, size_t cap)
{
    snprintf(out, cap, "?");
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) {
        return;
    }
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, cap);
    } else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&ss;
        const uint8_t *b = (const uint8_t *)&a6->sin6_addr;
        static const uint8_t v4map[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, v4map, sizeof(v4map)) == 0) {
            snprintf(out, cap, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, out, cap);
        }
    }
}

void kvm_web_peer_ip(int fd, char *out, size_t cap)
{
    peer_ip(fd, out, cap);
}

/*
 * Who has the console open: one entry for each open console tab (its control
 * socket), with its address, whether it is in control, whether it signed in
 * as the viewer, and whether it is the asker's own sign-in.
 */
esp_err_t api_viewers_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    uint8_t mine[32];
    const bool know_mine = kvm_auth_required() && kvm_auth_session_of(req, mine);

    int fds[TX_MAX_SOCKETS];
    int holder = -1;
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
        return send_json_error(req, "503 Service Unavailable", "busy");
    }
    memcpy(fds, s_ctrl_fds, sizeof(fds));
    holder = s_ws_fd;
    xSemaphoreGive(s_ws_mu);

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "viewers");
    for (int i = 0; i < TX_MAX_SOCKETS; i++) {
        if (fds[i] <= 0) {
            continue;
        }
        char ip[48];
        peer_ip(fds[i], ip, sizeof(ip));
        uint8_t theirs[32];
        const bool known = kvm_auth_socket_session(fds[i], theirs);
        cJSON *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "ip", ip);
        cJSON_AddBoolToObject(v, "control", fds[i] == holder);
        cJSON_AddBoolToObject(v, "viewOnly", known && kvm_auth_session_view_only(theirs));
        cJSON_AddBoolToObject(v, "you", know_mine && known && memcmp(mine, theirs, 32) == 0);
        /* Over the tailnet the address names a device and its owner. */
        kvm_ts_peer_t tp;
        if (kvm_ts_peer_by_ip(ip, &tp)) {
            cJSON *t = cJSON_AddObjectToObject(v, "tailnet");
            cJSON_AddStringToObject(t, "name", tp.name);
            cJSON_AddStringToObject(t, "user", tp.user);
            cJSON_AddStringToObject(t, "userName", tp.user_display);
        }
        cJSON_AddItemToArray(list, v);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t r = httpd_resp_sendstr(req, body);
    free(body);
    return r;
}

/* The locks every send takes; false (and logged) when one could not be made.
 * Runs once, before the server starts. */
bool ws_server_init(void)
{
    if (!s_ws_mu) {
        s_ws_mu = xSemaphoreCreateMutex();
        if (!s_ws_mu) {
            ESP_LOGE(TAG, "ws mutex");
            return false;
        }
    }
    if (!s_tx_mu) {
        for (int i = 0; i < TX_MAX_SOCKETS; i++) {
            s_tx_fds[i] = -1;
        }
        bool slots = true;
        for (int i = 0; i < TX_MAX_SOCKETS; i++) {
            s_tx_slot_mu[i] = xSemaphoreCreateBinary();
            if (s_tx_slot_mu[i]) {
                xSemaphoreGive(s_tx_slot_mu[i]); /* a binary semaphore starts taken */
            }
            slots = slots && s_tx_slot_mu[i];
        }
        s_tx_mu = slots ? xSemaphoreCreateRecursiveMutex() : NULL;
        /* Binary, not a mutex: a mutex would pass on priority boosts from
         * lwIP's core lock taken inside the write (see s_tx_slot_owner). */
        s_tls_write_turn = xSemaphoreCreateBinary();
        if (s_tls_write_turn) {
            xSemaphoreGive(s_tls_write_turn);
        }
        if (!s_tx_mu) {
            /* Without it every send would be racing the session teardown. */
            ESP_LOGE(TAG, "tx mutex");
            return false;
        }
    }
    return true;
}
