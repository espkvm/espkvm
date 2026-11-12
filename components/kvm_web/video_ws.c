/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The picture: the /video WebSocket, the multipart /stream, and the screen text
 * pushed to text-mode clients.
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

/* Live video WebSocket subscribers. api_video_status_get (system_api.c) reports
 * it too: a viewer the device still believes in should be visible from the API,
 * not just as an encoder that will not go idle. */
uint8_t s_video_client_count;
/* And those being sent the screen as characters instead. Kept apart because
   they are not viewers: no encoder runs for them. */
uint8_t s_text_client_count;
/** Multipart /stream responses in flight, counted separately from WebSocket
 *  subscribers so "who is holding the encoder busy" has an answer. */
volatile int s_stream_workers;

/* Long MJPEG response must not run on the httpd select() thread; see httpd_req_async_handler_begin(). */
#define STREAM_WORKER_STACK (12 * 1024)
#define STREAM_WORKER_PRIO (tskIDLE_PRIORITY + 5)

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

    t_stream_sender = true;
    video_frame_viewer_enter();
    s_stream_workers++;
    /* Send whatever is on screen right now rather than waiting for the next
     * publish: on a still screen the encoder deliberately stops publishing, so
     * a new viewer would otherwise stare at nothing. The loop below then waits
     * on the sequence number as usual, so this replays the frame only once. */
    uint32_t last_seq = video_frame_seq() - 1u;
    /* The opening boundary rides with the first part's headers; every part after
     * that is opened by the boundary the part before it already sent. */
    bool opened = false;

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
            vTaskDelay(1); /* >=1 tick; pdMS_TO_TICKS(5) is 0 ticks at 100 Hz */
            continue;
        }

        /*
         * The boundary that ENDS a part goes out with the part, not with the
         * next one. A browser showing multipart/x-mixed-replace in an <img>
         * paints a frame when it sees the boundary after it, so leaving that
         * boundary for the next frame means the picture on screen is always the
         * frame before the last one sent. That is invisible at 30 fps and very
         * visible on a still screen, where the encoder publishes only when
         * something changes: in a BIOS every keystroke showed the screen as it
         * was before the keystroke, until the 5-second keepalive caught up.
         */
        int hl = snprintf(hdr, sizeof(hdr),
                          "%s"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          opened ? "" : "--frame\r\n", f.len);
        esp_err_t se = ESP_FAIL;
        if (hl > 0 && hl < (int)sizeof(hdr)) {
            se = httpd_resp_send_chunk(req, hdr, hl);
            if (se == ESP_OK) {
                opened = true;
                se = httpd_resp_send_chunk(req, (const char *)f.data, f.len);
            }
            if (se == ESP_OK) {
                se = httpd_resp_send_chunk(req, "\r\n--frame\r\n", 11);
            }
        }
        if (se == ESP_OK && f.src_us > 0) {
            const int64_t now = esp_timer_get_time();
            capture_status_add_sent((uint32_t)(now - f.at_us), (uint32_t)(now - f.src_us));
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
    vTaskDeleteWithCaps(NULL);
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
 * Every frame is prefixed with a fixed 12-byte header, laid out in kvm_proto.h.
 *
 * The type is per frame rather than per connection because the codec can
 * change under a running viewer, and a decoder fed the wrong bytes fails in
 * ways that are hard to read.
 */

#define VIDEO_MAX_CLIENTS 4

/* How long a client's socket may stay full before it is dropped. A viewer whose
 * TCP send buffer stays full is not reading (a stalled decoder, a backgrounded
 * tab, a dead link); 3 s rides out normal flow control, and a viewer truly gone
 * stops keeping the encoder running. */
#define VIDEO_STALL_DROP_MS 3000


static int s_video_fds[VIDEO_MAX_CLIENTS];
/**
 * A viewer that has not been sent a decodable frame yet. H.264 P-frames are
 * meaningless without the IDR they refer to, so a client that joins mid-GOP
 * waits here - a few tens of milliseconds - until the keyframe it asked for
 * comes out of the encoder.
 */
static bool s_video_need_key[VIDEO_MAX_CLIENTS];
/* Frames sent and not yet acked, for a viewer that acks (WS_C2D_VIDEO_ACK).
 * Without this a viewer on a slow link had a whole TCP buffer of old frames
 * queued in front of the newest: hundreds of ms behind. */
static bool s_video_acks[VIDEO_MAX_CLIENTS];
static int s_video_unacked[VIDEO_MAX_CLIENTS];
/* Its acks stopped (the 500 ms timeout fired): treated as a viewer without
 * acks, gated by its socket, until it can take a frame again. */
static bool s_video_ack_lost[VIDEO_MAX_CLIENTS];
/*
 * How far an H.264 viewer that acks may run ahead. An H.264 frame needs the
 * one before it, so a frame skipped costs a keyframe - the biggest frame
 * there is - and over a VPN that keyframe kept the link full for the next
 * frame too: a picture every 2.5 s, one per keyframe. MJPEG frames stand
 * alone, so skipping one is free and its window stays at VIDEO_ACK_WINDOW.
 */
#define VIDEO_ACK_WINDOW_H264 6
static int64_t s_video_sent_at[VIDEO_MAX_CLIENTS];
/*
 * A subscriber on this socket wants the screen read as characters, not encoded
 * as pictures. It is the same socket and the same table because it is the same
 * question - "show me the target" - answered in the cheaper of two ways, and a
 * second WebSocket would mean a second authentication, a second slot and a
 * second thing to close.
 */
static bool s_text_mode[VIDEO_MAX_CLIENTS];
/* A subscriber that has no reading yet cannot be sent the rows that changed. */
static bool s_text_need_full[VIDEO_MAX_CLIENTS];
/* Bumped each time a slot gets a picture viewer, so that viewer's sender task
 * can tell its slot from a later viewer's on the same fd number. */
static uint32_t s_video_gen[VIDEO_MAX_CLIENTS];
/* When the frame on its way to this viewer started (0: not sending), and how
 * long a send to it usually takes: when it will want the next frame. */
static int64_t s_video_send_t0[VIDEO_MAX_CLIENTS];
static int64_t s_video_send_avg_us[VIDEO_MAX_CLIENTS];
static SemaphoreHandle_t s_video_mu;
static void video_sender_start(int index, int fd, uint32_t gen);

/* Whether this socket is already subscribed, and to the picture rather than to
 * the text reading. A second message from such a client is a resync request. */
static bool video_client_watches_picture(int fd)
{
    bool found = false;
    if (!s_video_mu || xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] == fd) {
            found = !s_text_mode[i];
            break;
        }
    }
    xSemaphoreGive(s_video_mu);
    return found;
}

static void video_add_client(int fd, bool text)
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
    int sender = -1;
    uint32_t gen = 0;
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] < 0) {
            s_video_fds[i] = fd;
            s_video_need_key[i] = true;
            s_video_acks[i] = false;
            s_video_unacked[i] = 0;
            s_video_ack_lost[i] = false;
            s_video_send_t0[i] = 0;
            s_video_send_avg_us[i] = 0;
            s_text_mode[i] = text;
            s_text_need_full[i] = text;
            /* The socket stays non-blocking, as esp-tls left it: locked_ssl_send
             * waits for room without holding the TLS write turn, and the
             * sender's wait for room drops a viewer that stopped reading. */
            if (text) {
                /* Deliberately not a viewer: the encoder stays idle, which is
                   the whole saving text mode exists for. What the reader does
                   need is the capture side reading the screen promptly. */
                s_text_client_count++;
                screentext_stream_enter();
            } else {
                if (s_video_client_count++ == 0) {
                    /* First viewer: the encoder is idle until someone is reading. */
                    video_frame_viewer_enter();
                }
                sender = i;
                gen = ++s_video_gen[i];
            }
            break;
        }
    }
    xSemaphoreGive(s_video_mu);
    if (sender >= 0) {
        video_sender_start(sender, fd, gen);
    }
    if (!text) {
        /* Also when this is not the first viewer: the newcomer needs a frame it
         * can decode from, whoever else is already watching. */
        video_frame_request_keyframe();
    }
}

static void video_drop_client_locked(int index)
{
    const bool was_text = s_text_mode[index];
    s_video_fds[index] = -1;
    s_video_need_key[index] = false;
    s_video_acks[index] = false;
    s_video_unacked[index] = 0;
    s_video_ack_lost[index] = false;
    s_text_mode[index] = false;
    s_text_need_full[index] = false;
    if (was_text) {
        if (s_text_client_count > 0) {
            s_text_client_count--;
        }
        screentext_stream_leave();
        return;
    }
    if (s_video_client_count > 0 && --s_video_client_count == 0) {
        video_frame_viewer_leave();
    }
}

void video_remove_client(int fd)
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

/* ---- the screen as characters, pushed ---------------------------------- */

/*
 * A reading is a couple of kilobytes where a picture is megabits a second, and
 * that is the whole reason text mode exists - so a reading goes out the way a
 * picture does, the moment there is one, rather than being fetched by a console
 * asking again and again. Each ask was a fresh HTTPS request with a handshake
 * on it, which is what made walking a boot menu feel like it was not listening.
 *
 * Only the rows that changed are sent. Moving a highlight one line changes two
 * of them, and a 1080p console is 240 columns by 56 rows - fourteen kilobytes
 * to say what four hundred bytes can.
 *
 * The polled /api/v1/screen/text stays exactly as it was: scripts, Home
 * Assistant and the MCP adapter use it, and it is the fallback for a console
 * whose socket will not open.
 */
static screentext_grid_t *s_push_grid; /* PSRAM, made once and kept */
static char *s_push_sent;              /* the reading subscribers already hold */
static uint16_t s_push_cols, s_push_rows;
static uint32_t s_push_seq;
/* The highlight subscribers already hold, beside the characters. Kept because a
   menu being walked changes only this - see highlight_runs(). */
static uint16_t s_push_runs[HIGHLIGHT_RUNS_MAX][3];
static int s_push_runs_n;

/** Row @p index of a '\n'-separated reading, and how long it is. */
static char *line_at(char *text, uint16_t index, size_t *len)
{
    char *p = text;
    for (uint16_t i = 0; i < index && p; i++) {
        p = strchr(p, '\n');
        if (p) {
            p++;
        }
    }
    if (!p || !*p) {
        *len = 0;
        return NULL;
    }
    const char *nl = strchr(p, '\n');
    *len = nl ? (size_t)(nl - p) : strlen(p);
    return p;
}

/**
 * The rows where two readings differ, as [row, text] pairs.
 *
 * @p cur is written to and put back: a line is terminated in place so cJSON can
 * take it, which costs nothing where copying every row out would cost the very
 * kilobytes this is here to save.
 */
static cJSON *changed_lines(char *cur, char *prev, uint16_t rows)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    for (uint16_t i = 0; i < rows; i++) {
        size_t cl = 0, pl = 0;
        char *c = line_at(cur, i, &cl);
        char *pv = line_at(prev, i, &pl);
        if (!c) {
            break;
        }
        if (pv && cl == pl && memcmp(c, pv, cl) == 0) {
            continue;
        }
        const char save = c[cl];
        c[cl] = '\0';
        cJSON *pair = cJSON_CreateArray();
        cJSON *text = cJSON_CreateString(c);
        c[cl] = save;
        if (!pair || !text) {
            cJSON_Delete(pair);
            cJSON_Delete(text);
            break;
        }
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(i));
        cJSON_AddItemToArray(pair, text);
        cJSON_AddItemToArray(arr, pair);
    }
    return arr;
}

static void text_send(httpd_handle_t server, const int *fds, int count, const char *json)
{
    if (!json || count <= 0) {
        return;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    for (int i = 0; i < count; i++) {
        if (ws_send_frame(server, fds[i], &frame) != ESP_OK) {
            video_remove_client(fds[i]);
        }
    }
}

/** Send whatever is new to whoever is subscribed. Returns at once when neither. */
static void text_push(httpd_handle_t server)
{
    if (s_text_client_count == 0) {
        /* Let the copy go, and make the next subscriber start from a whole
           reading rather than from rows that changed against nothing. */
        free(s_push_sent);
        s_push_sent = NULL;
        s_push_runs_n = 0;
        return;
    }

    int fresh[VIDEO_MAX_CLIENTS], caught_up[VIDEO_MAX_CLIENTS];
    int fresh_n = 0, caught_up_n = 0;
    if (!s_video_mu || xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] < 0 || !s_text_mode[i]) {
            continue;
        }
        if (s_text_need_full[i]) {
            fresh[fresh_n++] = s_video_fds[i];
            s_text_need_full[i] = false;
        } else {
            caught_up[caught_up_n++] = s_video_fds[i];
        }
    }
    xSemaphoreGive(s_video_mu);

    const uint32_t seq = screentext_seq();
    if (seq == s_push_seq && fresh_n == 0) {
        return; /* nothing read since the last send, and nobody new */
    }

    if (!s_push_grid) {
        s_push_grid = heap_caps_malloc(sizeof(*s_push_grid), MALLOC_CAP_SPIRAM);
        if (!s_push_grid) {
            return;
        }
    }
    uint32_t age_ms = 0;
    if (!screentext_latest(s_push_grid, &age_ms)) {
        /* The screen stopped being characters - it booted, or drew a picture.
           Said plainly, because the alternative is a console showing text from
           a screen that is gone. */
        int all[VIDEO_MAX_CLIENTS];
        int all_n = 0;
        for (int i = 0; i < fresh_n; i++) {
            all[all_n++] = fresh[i];
        }
        for (int i = 0; i < caught_up_n; i++) {
            all[all_n++] = caught_up[i];
        }
        text_send(server, all, all_n, "{\"kind\":\"text\",\"gone\":true}");
        free(s_push_sent);
        s_push_sent = NULL;
        s_push_runs_n = 0;
        s_push_seq = seq;
        return;
    }

    char *text = screentext_utf8_alloc(s_push_grid);
    if (!text) {
        return;
    }
    const bool same_shape = s_push_sent && s_push_cols == s_push_grid->cols &&
                            s_push_rows == s_push_grid->rows;

    uint16_t runs[HIGHLIGHT_RUNS_MAX][3];
    const int runs_n = highlight_runs(s_push_grid, runs);
    const bool highlight_moved =
        runs_n != s_push_runs_n || memcmp(runs, s_push_runs, (size_t)runs_n * sizeof(runs[0])) != 0;

    /* The rows that changed, for everyone who already has a reading. */
    if (caught_up_n > 0 && same_shape) {
        cJSON *lines = changed_lines(text, s_push_sent, s_push_grid->rows);
        if (lines) {
            if (cJSON_GetArraySize(lines) == 0 && !highlight_moved) {
                /* Nothing a reader would see. The characters alone are not that
                 * test: walking a boot menu moves the highlight bar and leaves
                 * every character where it was, so comparing only the text threw
                 * the whole update away and the menu looked frozen - while the
                 * picture, when anyone switched to it, showed the selection
                 * exactly where the operator had scrolled to. */
                cJSON_Delete(lines);
            } else {
                cJSON *o = cJSON_CreateObject();
                if (o) {
                    cJSON_AddStringToObject(o, "kind", "text");
                    cJSON_AddBoolToObject(o, "full", false);
                    cJSON_AddNumberToObject(o, "ageMs", (double)age_ms);
                    cJSON_AddItemToObject(o, "lines", lines);
                    json_add_highlight(o, s_push_grid);
                    json_add_alert(o);
                    char *json = cJSON_PrintUnformatted(o);
                    cJSON_Delete(o);
                    text_send(server, caught_up, caught_up_n, json);
                    free(json);
                } else {
                    cJSON_Delete(lines);
                }
            }
        }
    } else {
        /* No base to send rows against - a different screen shape, or the first
           reading of this run. Everyone gets the whole thing. */
        for (int i = 0; i < caught_up_n; i++) {
            fresh[fresh_n++] = caught_up[i];
        }
        caught_up_n = 0;
    }

    /* The whole reading, for anyone who has just subscribed. */
    if (fresh_n > 0) {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "kind", "text");
            cJSON_AddBoolToObject(o, "full", true);
            json_add_geometry(o, s_push_grid, age_ms);
            cJSON_AddStringToObject(o, "text", text);
            json_add_highlight(o, s_push_grid);
            json_add_alert(o);
            char *json = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            text_send(server, fresh, fresh_n, json);
            free(json);
        }
    }

    free(s_push_sent);
    s_push_sent = text;
    s_push_cols = s_push_grid->cols;
    s_push_rows = s_push_grid->rows;
    s_push_seq = seq;
    memcpy(s_push_runs, runs, (size_t)runs_n * sizeof(runs[0]));
    s_push_runs_n = runs_n;
}

/*
 * One task pushes to every subscribed socket. The websocket upgrade is handled
 * by esp_http_server itself, so unlike the multipart stream this must not be
 * wrapped in an async request handler - doing so leaves the handshake
 * unfinished and no frame ever reaches the client.
 */
/* Repair IDRs share one 300 ms limit with the WebRTC sender. */
static void video_request_repair_idr(void)
{
    video_frame_request_repair_keyframe();
}

/* Text-mode readings go out from here; pictures from each viewer's sender. */
static void text_pump_task(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;
    for (;;) {
        /* Cheap unless there is a new reading and somebody to send it to: a
           sequence compared against the last one sent. A reader of characters
           is waited on more closely than an empty device: this check is what
           turns a settled screen into an update. */
        text_push(server);
        vTaskDelay(pdMS_TO_TICKS(s_text_client_count ? 20 : 100));
    }
}

typedef struct {
    httpd_handle_t server;
    int index;
    int fd;
    uint32_t gen;
} video_sender_t;

/* Under s_video_mu: the slot still holds the viewer this sender was made for. */
static bool video_sender_owns_locked(const video_sender_t *v)
{
    return s_video_fds[v->index] == v->fd && s_video_gen[v->index] == v->gen;
}

static bool video_sender_owns(const video_sender_t *v)
{
    if (xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool owns = video_sender_owns_locked(v);
    xSemaphoreGive(s_video_mu);
    return owns;
}

/*
 * A viewer that does not ack is paced by its socket: wait until the socket can
 * take a frame, then send the newest. Sending into a full buffer would queue a
 * frame behind older ones. lwIP calls a socket writable only with half its
 * send buffer free. A viewer that acks skips this: its acks are the flow
 * control. Returns false when the viewer is gone or was dropped for not reading.
 */
static bool video_sender_wait_room(const video_sender_t *v)
{
    if (xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (!video_sender_owns_locked(v)) {
        xSemaphoreGive(s_video_mu);
        return false;
    }
    const bool paced = !s_video_acks[v->index] || s_video_ack_lost[v->index];
    xSemaphoreGive(s_video_mu);
    if (!paced) {
        return true;
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(v->fd, &wfds);
    struct timeval tv = {0, 0};
    if (select(v->fd + 1, NULL, &wfds, NULL, &tv) > 0) {
        return true;
    }
    /* Each frame that comes out while it waits is one its link could not
     * carry: what turns the quality down (capture_link_pct). */
    const uint32_t seq0 = video_frame_seq();
    FD_ZERO(&wfds);
    FD_SET(v->fd, &wfds);
    tv = (struct timeval){.tv_sec = VIDEO_STALL_DROP_MS / 1000, .tv_usec = (VIDEO_STALL_DROP_MS % 1000) * 1000};
    const int r = select(v->fd + 1, NULL, &wfds, NULL, &tv);
    for (uint32_t n = video_frame_seq() - seq0; n > 0; n--) {
        capture_link_miss();
    }
    if (r > 0) {
        return true;
    }
    if (xSemaphoreTake(s_video_mu, portMAX_DELAY) == pdTRUE) {
        if (video_sender_owns_locked(v)) {
            video_drop_client_locked(v->index);
        }
        xSemaphoreGive(s_video_mu);
    }
    return false;
}

/*
 * One per picture viewer, for as long as it watches. The frames used to go out
 * from a single task, one viewer after another, so a viewer on slow Wi-Fi held
 * every other viewer to its pace: a console on Ethernet fell from 17 to 5 fps.
 * Now a slow send only delays its own viewer, which then takes the newest
 * frame when it is done.
 */
static void video_sender_task(void *arg)
{
    video_sender_t v = *(video_sender_t *)arg;
    free(arg);
    const int i = v.index;
    uint8_t *packet = NULL;
    size_t packet_cap = 0;
    uint32_t last_seq = 0;

    t_stream_sender = true;
    for (;;) {
        if (!video_sender_wait_room(&v)) {
            break;
        }
        /*
         * Block until the store publishes something newer - do NOT poll. A
         * 5 ms poll is 0 ticks at CONFIG_FREERTOS_HZ=100, and the loop then
         * busy-spins between frames, starving the idle task (task_wdt) and the
         * capture monitor. The semaphore wait sleeps properly at any tick rate.
         */
        if (!video_frame_wait_new(last_seq, 500)) {
            if (!video_sender_owns(&v)) {
                break;
            }
            continue;
        }

        video_frame_ref_t f;
        if (!video_frame_acquire(&f)) {
            /* The front slot is momentarily invalid - e.g. a codec/resolution
             * reinit (video_frame_install) set it aside and bumped the sequence.
             * At least one full tick: pdMS_TO_TICKS(5) is 0 ticks at 100 Hz. */
            vTaskDelay(1);
            last_seq = video_frame_seq();
            continue;
        }
        /*
         * The store hands out the newest frame, so falling behind a burst (a
         * window opening is a run of large deltas) skips some. A skipped H.264
         * delta breaks this viewer's reference chain until a keyframe.
         */
        const bool missed = last_seq != 0 && (uint32_t)(f.seq - last_seq) > 1u;
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
        const kvm_video_hdr_t hdr = {
            .type = (uint8_t)f.payload,
            .width = (uint16_t)vs.hres,
            .height = (uint16_t)vs.vres,
            .seq = f.seq,
            .pts_ms = (uint16_t)pts,
        };
        kvm_video_hdr_write(packet, &hdr);
        memcpy(packet + VIDEO_HDR_LEN, f.data, f.len);
        const size_t packet_len = VIDEO_HDR_LEN + f.len;
        const bool keyframe = f.keyframe;
        const int f_payload = f.payload;
        const int64_t f_at_us = f.at_us, f_src_us = f.src_us;
        video_frame_release(&f);

        /* Decide under the lock, send outside it: a send to a slow link blocks
         * for as long as the socket takes, and holding the table across that
         * stalls every session close for the same duration. */
        if (xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!video_sender_owns_locked(&v)) {
            xSemaphoreGive(s_video_mu);
            break;
        }
        bool send = false;
        bool repair = false;
        /* Ask for a keyframe but keep sending deltas: the picture smudges
         * until it comes. Holding them back froze the picture (#85). A 1080p
         * keyframe takes longer to send than one frame lasts, so the frame
         * after it was skipped, the viewer waited for the next keyframe, and
         * the one asked for went by while it was still sending. */
        if (missed && f_payload == VIDEO_PAYLOAD_H264) {
            repair = true;
        }
        const int window = f_payload == VIDEO_PAYLOAD_H264 ? VIDEO_ACK_WINDOW_H264 : VIDEO_ACK_WINDOW;
        if (s_video_acks[i] && s_video_unacked[i] >= window &&
            esp_timer_get_time() - s_video_sent_at[i] > 500000) {
            s_video_unacked[i] = 0; /* an ack went missing: do not wait forever */
            s_video_ack_lost[i] = true;
        }
        if (s_video_need_key[i] && !keyframe) {
            /* still waiting for a decodable frame */
        } else if (s_video_acks[i] && s_video_unacked[i] >= window) {
            /* Still showing what it has: the newest frame goes next time,
             * rather than this one waiting in a buffer. */
            if (f_payload == VIDEO_PAYLOAD_H264 && !s_video_need_key[i]) {
                s_video_need_key[i] = true;
                repair = true;
            }
        } else {
            s_video_ack_lost[i] = false;
            s_video_need_key[i] = false; /* decodable frame (a keyframe if it was waiting) */
            if (s_video_acks[i]) {
                s_video_unacked[i]++;
                s_video_sent_at[i] = esp_timer_get_time();
            }
            s_video_send_t0[i] = esp_timer_get_time();
            send = true;
        }
        xSemaphoreGive(s_video_mu);
        if (repair) {
            video_request_repair_idr();
        }
        if (!send) {
            continue;
        }

        httpd_ws_frame_t frame = {
            .final = true,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = packet,
            .len = packet_len,
        };
        const int64_t send_t0 = esp_timer_get_time();
        const esp_err_t send_r = ws_send_frame(v.server, v.fd, &frame);
        const int64_t send_ms = (esp_timer_get_time() - send_t0) / 1000;
        if (send_ms > 500) {
            ESP_LOGW(TAG, "slow video send fd=%d len=%zu key=%d took %lldms", v.fd, packet_len, keyframe,
                     (long long)send_ms);
        }
        if (xSemaphoreTake(s_video_mu, portMAX_DELAY) == pdTRUE) {
            if (video_sender_owns_locked(&v)) {
                s_video_send_t0[i] = 0;
                s_video_send_avg_us[i] += (send_ms * 1000 - s_video_send_avg_us[i]) / 4;
            }
            xSemaphoreGive(s_video_mu);
        }
        if (send_r != ESP_OK) {
            /* A viewer that has gone stops being counted, which is also what
             * lets the encoder go idle again. By slot, not fd: the fd number
             * may already belong to a new session. */
            if (xSemaphoreTake(s_video_mu, portMAX_DELAY) == pdTRUE) {
                if (video_sender_owns_locked(&v)) {
                    video_drop_client_locked(i);
                }
                xSemaphoreGive(s_video_mu);
            }
            break;
        }
        if (f_src_us > 0) {
            const int64_t now = esp_timer_get_time();
            capture_status_add_sent((uint32_t)(now - f_at_us), (uint32_t)(now - f_src_us));
        }
    }
    free(packet);
    vTaskDeleteWithCaps(NULL);
}

/*
 * The encoder asks before an MJPEG frame (video_frame_wanted): is any viewer
 * free, or done sending within @p lead_us? A frame encoded earlier would only
 * wait, older, for a sender to come back. Never blocks the capture loop: a
 * table that is busy answers yes.
 */
static bool video_demand(int64_t lead_us)
{
    if (!s_video_mu || xSemaphoreTake(s_video_mu, 0) != pdTRUE) {
        return true;
    }
    const int64_t now = esp_timer_get_time();
    bool want = true;
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] < 0 || s_text_mode[i]) {
            continue;
        }
        want = s_video_need_key[i] || s_video_send_t0[i] == 0 ||
               s_video_send_t0[i] + s_video_send_avg_us[i] - now <= lead_us;
        if (want) {
            break;
        }
    }
    xSemaphoreGive(s_video_mu);
    return want;
}

static void video_sender_start(int index, int fd, uint32_t gen)
{
    video_sender_t *v = malloc(sizeof(*v));
    if (v) {
        *v = (video_sender_t){.server = s_httpd, .index = index, .fd = fd, .gen = gen};
        /* The stack the single pump had: the TLS write is the deepest thing here.
           In PSRAM: one per viewer, and internal RAM is what SDIO to the
           Wi-Fi chip and the AES/I2S DMA run out of first. */
        if (xTaskCreateWithCaps(video_sender_task, "kvm_video", 4096, v, STREAM_WORKER_PRIO, NULL,
                                MALLOC_CAP_SPIRAM) == pdPASS) {
            return;
        }
        free(v);
    }
    /* No task, no frames: do not leave it counted (the encoder would run for it). */
    ESP_LOGW(TAG, "no memory for a video sender, fd=%d dropped", fd);
    if (xSemaphoreTake(s_video_mu, portMAX_DELAY) == pdTRUE) {
        if (s_video_fds[index] == fd && s_video_gen[index] == gen) {
            video_drop_client_locked(index);
        }
        xSemaphoreGive(s_video_mu);
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
/*
 * A console tab left open after its session expired retries every 30 seconds
 * for as long as the browser is running, and every attempt is refused here.
 * Logged plainly that is two lines a refusal, which flushed the 200-line ring
 * in under an hour on a device watched overnight - 875 refusals, and the fault
 * the log was kept for was long gone from it. So: say it, then say it again at
 * most once a minute, with a count of what was skipped.
 */
static void log_refusal(const char *why)
{
    static int64_t last_us;
    static uint32_t skipped;
    const int64_t now = esp_timer_get_time();

    if (last_us != 0 && now - last_us < 60 * 1000 * 1000) {
        skipped++;
        return;
    }
    if (skipped) {
        ESP_LOGW(TAG, "websocket refused: %s (and %" PRIu32 " more)", why, skipped);
    } else {
        ESP_LOGW(TAG, "websocket refused: %s", why);
    }
    skipped = 0;
    last_us = now;
}

esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    /*
     * A WebSocket upgrade carries the cookie like any other request, and the
     * page that opened it says where it came from. A socket that would drive
     * somebody's keyboard is worth being sure about, so an Origin that is not
     * this device is refused before the session is even looked at.
     */
    if (!kvm_auth_origin_ok(req)) {
        log_refusal("another site opened it");
        return ESP_FAIL;
    }
    if (!kvm_auth_check(req)) {
        log_refusal("no session");
        return ESP_FAIL;
    }
    /* Frames carry no headers, so the socket itself is the credential from
     * here on. */
    uint8_t session[32];
    (void)kvm_auth_session_of(req, session);
    if (!kvm_auth_mark_socket(httpd_req_to_sockfd(req), session)) {
        log_refusal("too many sockets");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t kvm_web_ws_pre_handshake(httpd_req_t *req)
{
    return ws_pre_handshake(req);
}

static void video_client_acked(int fd)
{
    if (!s_video_mu || xSemaphoreTake(s_video_mu, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        if (s_video_fds[i] == fd) {
            s_video_acks[i] = true;
            if (s_video_unacked[i] > 0) {
                s_video_unacked[i]--;
            }
            break;
        }
    }
    xSemaphoreGive(s_video_mu);
}

esp_err_t video_ws_handler(httpd_req_t *req)
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
    /*
     * What the first message says is what the client wants: 2 asks for the
     * screen read as characters, anything else (a byte of 1, or nothing at all)
     * for the picture. A byte rather than a word because the console has always
     * sent a byte here, and a firmware that met an older console would otherwise
     * have to guess.
     */
    uint8_t hello[8] = {0};
    if (pkt.len > sizeof(hello)) {
        return ESP_FAIL; /* left unread it would desync the socket: close */
    }
    if (pkt.len) {
        pkt.payload = hello;
        if (httpd_ws_recv_frame(req, &pkt, sizeof(hello)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    const int fd = httpd_req_to_sockfd(req);
    if (pkt.len >= 1 && hello[0] == WS_C2D_VIDEO_ACK) {
        video_client_acked(fd);
        return ESP_OK;
    }
    /*
     * A second message from a client that is already watching asks for a
     * keyframe. The console needs that when its own decoder has lost the
     * reference chain - a tab that Chrome throttled in the background drops
     * frames on the way into the decoder, and the device cannot see that
     * happen: it sent them. Without this the picture stays in blocks until the
     * next scheduled IDR repairs it, and a decoder that is still behind drops
     * that one too.
     *
     * Deliberately not a new opcode: an older console sends its subscribe byte
     * again and gets the same repair.
     */
    if (video_client_watches_picture(fd)) {
        video_frame_request_keyframe();
        return ESP_OK;
    }
    const bool text = hello[0] == WS_SUBSCRIBE_TEXT;
    video_add_client(fd, text);
    if (!text) {
        const uint8_t acks = WS_D2C_VIDEO_ACKS;
        ws_send_binary(fd, &acks, 1);
    }
    return ESP_OK;
}

esp_err_t stream_get(httpd_req_t *req)
{
    esp_err_t gate;
    if (!viewing_allowed(req, &gate)) {
        return gate;
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

    /* One per open stream: the stack goes to PSRAM (see video_sender_start). */
    BaseType_t created = xTaskCreateWithCaps(stream_worker_task, "kvm_stream", STREAM_WORKER_STACK, async_req,
                                             STREAM_WORKER_PRIO, NULL, MALLOC_CAP_SPIRAM);
    if (created != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "stream task");
    }
    return ESP_OK;
}

/* The video clients' table and locks, and the text pump. Runs once the server
 * @p h is up. */
void video_ws_init(httpd_handle_t h)
{
    for (int i = 0; i < VIDEO_MAX_CLIENTS; i++) {
        s_video_fds[i] = -1;
    }
    s_xmit_mu = xSemaphoreCreateMutex();
    s_video_mu = xSemaphoreCreateMutex();
    if (s_video_mu) {
        xTaskCreate(text_pump_task, "kvm_text", 4096, h, STREAM_WORKER_PRIO, NULL);
        video_frame_set_demand_cb(video_demand);
    }
}
