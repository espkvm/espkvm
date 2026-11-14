/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings, device information, Wi-Fi, logs, restart and power: the REST routes
 * that describe and run the device itself.
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
#include "kvm_net_guard.h"
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

/* Copy @p in as the inside of a JSON string: quotes, backslashes and control
 * bytes escaped. Stops short rather than cutting an escape in half. */
static void json_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
}

esp_err_t api_capabilities_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_json_owned(req, kvm_caps_to_json());
}

esp_err_t api_settings_schema_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    return send_json_owned(req, kvm_settings_schema_json());
}

/* Add a fixed-peripheral pin to the reserved list (skipped when absent, -1). */
static void pins_add_reserved(cJSON *arr, int pin, const char *use)
{
    if (pin < 0) {
        return;
    }
    /* One pin can carry two fixed uses: on the p4-eth GPIO 35 is the BOOT button
     * and Ethernet TXD1 both. The console draws a row per pin, so the second use
     * joins the first rather than showing the same pin twice. */
    cJSON *had = NULL;
    cJSON_ArrayForEach(had, arr) {
        const cJSON *p = cJSON_GetObjectItem(had, "pin");
        if (!cJSON_IsNumber(p) || p->valueint != pin) {
            continue;
        }
        const cJSON *was = cJSON_GetObjectItem(had, "use");
        if (!cJSON_IsString(was)) {
            return;
        }
        char both[96];
        snprintf(both, sizeof(both), "%s / %s", was->valuestring, use);
        cJSON *joined = cJSON_CreateString(both);
        if (joined) {
            cJSON_ReplaceItemInObject(had, "use", joined);
        }
        return;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddNumberToObject(o, "pin", pin);
    cJSON_AddStringToObject(o, "use", use);
    cJSON_AddItemToArray(arr, o);
}

/*
 * One column of a header, top to bottom.
 *
 * A pin is either a GPIO (`gpio`, and the console already knows what holds it)
 * or it is not (`label`: 3V3, GND, a co-processor's pin, NC). `note` carries
 * whatever an operator has to know before putting a wire there.
 */
static void pins_add_column(cJSON *parent, const char *key, const kvm_board_pin_t *pins,
                            uint8_t rows)
{
    if (!pins) {
        return;
    }
    cJSON *arr = cJSON_AddArrayToObject(parent, key);
    if (!arr) {
        return;
    }
    for (uint8_t i = 0; i < rows; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            return;
        }
        if (pins[i].gpio >= 0) {
            cJSON_AddNumberToObject(o, "gpio", pins[i].gpio);
        } else {
            cJSON_AddStringToObject(o, "label", pins[i].label);
        }
        if (pins[i].note) {
            cJSON_AddStringToObject(o, "note", pins[i].note);
        }
        cJSON_AddItemToArray(arr, o);
    }
}

/*
 * The GPIO map the console needs for its pin pickers and the Pins tab: the usable
 * range, and which pins the board's fixed peripherals hold (with labels). Pins
 * assigned to editable settings are known to the console already (they are
 * "pin"-flagged settings), so only the fixed ones are listed here.
 */
esp_err_t api_pins_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddNumberToObject(root, "usableMin", 0);
    cJSON_AddNumberToObject(root, "usableMax", 54);
    cJSON *r = cJSON_AddArrayToObject(root, "reserved");
    if (r) {
        /* The same list the settings check refuses a write against, so the pin
         * picker and the device cannot disagree about what is free. */
        size_t reserved_count = 0;
        const kvm_board_reserved_t *reserved = kvm_board_reserved(&reserved_count);
        for (size_t i = 0; i < reserved_count; i++) {
            pins_add_reserved(r, reserved[i].gpio, reserved[i].use);
        }
    }

    /*
     * And where those GPIOs actually are. A GPIO number tells an operator
     * nothing about where to put a wire, and half of them do not leave the
     * package on any given board, so the console draws the expansion header the
     * way it is printed - when the board is one we have a pinout for.
     */
    cJSON_AddStringToObject(root, "board", kvm_board_name());
    /* The id its published image is named with, so the console can tell which
       releases actually carry a build for this board. */
    cJSON_AddStringToObject(root, "boardId", kvm_board_id());
    size_t header_count = 0;
    const kvm_board_header_t *headers = kvm_board_headers(&header_count);
    if (headers) {
        cJSON_AddBoolToObject(root, "headerVerified", kvm_board_header_verified());
        cJSON *hs = cJSON_AddArrayToObject(root, "headers");
        for (size_t i = 0; hs && i < header_count; i++) {
            cJSON *h = cJSON_CreateObject();
            if (!h) {
                break;
            }
            cJSON_AddStringToObject(h, "name", headers[i].name);
            cJSON_AddBoolToObject(h, "numbered", headers[i].numbered);
            pins_add_column(h, "left", headers[i].left, headers[i].rows);
            pins_add_column(h, "right", headers[i].right, headers[i].rows);
            cJSON_AddItemToArray(hs, h);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return send_json_owned(req, out);
}

/*
 * Make a viewing token, and say what it is - once.
 *
 * The device keeps only a hash of it, so this is the one moment it can be read.
 * The console says so plainly; there is no endpoint that can hand it over
 * again, on purpose.
 */
esp_err_t api_token_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char token[KVM_AUTH_TOKEN_CHARS + 1];
    const esp_err_t err = kvm_auth_token_create(token, sizeof(token));
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not store the token");
    }
    ESP_LOGW(TAG, "a viewing token was created");
    char body[96];
    snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t api_token_delete(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    if (kvm_auth_token_revoke() != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not revoke the token");
    }
    ESP_LOGW(TAG, "the viewing token was revoked");
    return send_json_owned(req, strdup("{\"status\":\"revoked\"}"));
}

esp_err_t api_settings_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    /* ?profile=default: Default's own values under a device profile (an export). */
    char q[32], prof[16];
    const bool def = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
                     httpd_query_key_value(q, "profile", prof, sizeof(prof)) == ESP_OK &&
                     strcmp(prof, "default") == 0;
    return send_json_owned(req, def ? kvm_settings_default_values_json() : kvm_settings_values_json());
}

esp_err_t api_settings_put(httpd_req_t *req)
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
    int got = 0, stalls = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (kvm_recv_stalled(n) && ++stalls <= KVM_RECV_MAX_STALLS) {
            continue;
        }
        if (n <= 0) {
            free(body);
            return send_json_error(req, "400 Bad Request", "truncated body");
        }
        stalls = 0;
        got += n;
    }
    body[got] = '\0';

    char err[128];
    kvm_net_guard_before_put(body, req); /* keeps the way back for a link change */
    esp_err_t res = kvm_settings_apply_json(body, err, sizeof(err));
    kvm_net_guard_after_put(res == ESP_OK);
    free(body);
    if (res != ESP_OK) {
        return send_json_error(req, res == ESP_ERR_NOT_FOUND ? "404 Not Found" : "400 Bad Request",
                               err[0] ? err : "invalid settings");
    }
    return send_json_owned(req, kvm_settings_values_json());
}

esp_err_t api_video_status_get(httpd_req_t *req)
{
    esp_err_t gate;
    if (!viewing_allowed(req, &gate)) {
        return gate;
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

    /* Whether this mode could be read as characters at all - the same test the
     * scanner itself applies, so the console never has to keep its own copy of
     * the rule. It kept one, and the two drifted: the console still recognised
     * only 80 columns of 8x16 long after the scanner learned the 128-column
     * UEFI console, and greyed out Select and Copy on a screen the device could
     * read perfectly well. This says nothing about whether text is on file now
     * - that is what /api/v1/screen/text answers - only that asking is worth it. */
    const bool text_mode = st.signal && screentext_mode_supported(st.hres, st.vres);

    kvm_atx_status_t atx;
    kvm_atx_status(&atx);

    kvm_record_status_t rec;
    kvm_record_status(&rec);
    /* Blocked reasons are fixed strings from the firmware: no quoting needed. */
    const char *rec_blocked = rec.recording ? NULL : kvm_record_blocked();
    const char *shot_blocked = kvm_record_screenshot_blocked();

    char age[12] = "null";
    if (st.frame_age_ms != UINT32_MAX) {
        snprintf(age, sizeof(age), "%u", (unsigned)st.frame_age_ms);
    }

    /* The CSI link as last sampled: does the bridge send at all. */
    char csi[128] = "";
    capture_csi_link_t link;
    uint32_t link_age;
    if (capture_csi_link_last(&link, &link_age)) {
        snprintf(csi, sizeof(csi),
                 ",\"csi\":{\"clkHs\":%u,\"stop\":[%u,%u],\"stopClk\":%u,\"phyErr\":%" PRIu32
                 ",\"ageMs\":%" PRIu32 "}",
                 link.clk_hs, link.stop_d0, link.stop_d1, link.stop_clk, link.phy_err, link_age);
    }

    /* Worst case with every string field full is about 1560. */
    char body[1720];
    int n = snprintf(body, sizeof(body),
                     "{\"knowsDdc5v\":%s,\"signal\":%s,\"width\":%u,\"height\":%u,\"interlaced\":%s,"
                     "\"inputHz\":%u,\"tooFast\":%s,\"audioHz\":%u,"
                     "\"fps\":%u.%02u,\"skippedFps\":%u.%02u,\"kbps\":%u,"
                     "\"encodeUs\":%u,\"ppaUs\":%u,\"encoderBusyPct\":%u,"
                     "\"modeChanges\":%u,\"sysStatus\":%u,\"viewers\":%d,"
                     "\"wsClients\":%u,\"textClients\":%u,\"imgClients\":%d,\"codec\":\"%s\","
                     /* How long the picture has been one flat colour: the shape
                        of a stop screen or a blanked output, neither of which
                        can be read as text. 0 means it is a picture. */
                     "\"textMode\":%s,\"flatMs\":%u,"
                     /* Frames the capture delivered since boot, and how long ago
                        the last one came: a lock with nothing behind it stops
                        the count even while "signal" stays true. */
                     "\"frames\":%u,\"frameAgeMs\":%s,\"h264Cpu\":%s,"
                     /* Through the device, ms: landed -> taken, encode (the
                        two encode figures above), published -> sent, and
                        landed -> sent with the second's worst. */
                     "\"lagMs\":{\"wait\":%u,\"send\":%u,\"total\":%u,\"totalMax\":%u},"
                     "\"linkPct\":%u,"
                     /* The recorder rides along: the console polls this anyway. */
                     "\"record\":{\"on\":%s,\"file\":\"%s\",\"seconds\":%u,\"bytes\":%llu,"
                     "\"dropped\":%u,\"stopped\":\"%s\",\"blocked\":%s%s%s,"
                     "\"event\":%s,\"dashcam\":%s,\"dashcamNoMemory\":%s,\"prerollSeconds\":%u,"
                     "\"timelapse\":%u,\"clipSecondsLeft\":%u,\"clipsConverting\":%u,"
                     "\"lastClip\":\"%s\"},"
                     "\"screenshotBlocked\":%s%s%s,"
                     /* The device may switch profiles by itself: the console
                        reloads the settings when this moves. */
                     "\"profile\":%u,"
                     /* The target's HDD LED, null when it is not wired. */
                     "\"hdd\":%s%s}",
                     capture_source_power_known() ? "true" : "false",
                     st.signal ? "true" : "false", (unsigned)st.hres, (unsigned)st.vres,
                     st.interlaced ? "true" : "false", (unsigned)st.input_hz,
                     st.too_fast ? "true" : "false", (unsigned)st.audio_hz,
                     (unsigned)(st.fps_x100 / 100u),
                     (unsigned)(st.fps_x100 % 100u), (unsigned)(st.skipped_fps_x100 / 100u),
                     (unsigned)(st.skipped_fps_x100 % 100u), (unsigned)st.kbps,
                     (unsigned)st.encode_us, (unsigned)st.ppa_us, (unsigned)st.encoder_busy_pct,
                     (unsigned)st.mode_changes, (unsigned)st.sys_status,
                     video_frame_viewer_count(), (unsigned)s_video_client_count,
                     (unsigned)s_text_client_count, s_stream_workers, codec,
                     text_mode ? "true" : "false",
                     (unsigned)st.flat_ms, (unsigned)st.frames, age,
                     st.h264_cpu ? "true" : "false", (unsigned)st.lag_wait_ms,
                     (unsigned)st.lag_send_ms, (unsigned)st.lag_total_ms,
                     (unsigned)st.lag_total_max_ms, (unsigned)st.link_pct,
                     rec.recording ? "true" : "false", rec.file,
                     (unsigned)rec.seconds, (unsigned long long)rec.bytes, (unsigned)rec.dropped,
                     rec.stopped, rec_blocked ? "\"" : "", rec_blocked ? rec_blocked : "null",
                     rec_blocked ? "\"" : "", rec.event ? "true" : "false",
                     rec.dashcam ? "true" : "false", rec.dashcam_no_memory ? "true" : "false",
                     (unsigned)rec.preroll_seconds,
                     (unsigned)rec.timelapse_every, (unsigned)rec.clip_seconds_left,
                     (unsigned)rec.clips_converting, rec.last_clip, shot_blocked ? "\"" : "",
                     shot_blocked ? shot_blocked : "null", shot_blocked ? "\"" : "",
                     (unsigned)kvm_profiles_active(),
                     !atx.have_hdd ? "null" : atx.hdd_busy ? "true" : "false", csi);
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
/* OTA image state as a short, stable token the console maps to a badge. */
static const char *ota_state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

/*
 * The OTA app slots as a JSON array: each slot's stored version and image state,
 * and which one is running now and which the bootloader will start next. A slot
 * with no valid image reports an empty version. Written into `out`; returns the
 * number of bytes used (never more than cap-1).
 */
static int ota_slots_json(char *out, size_t cap)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    int p = snprintf(out, cap, "[");
    bool first = true;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *part = esp_partition_get(it);
        if (part->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
            part->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            continue; /* only ota_0..ota_15, not a factory or test image */
        }
        esp_app_desc_t d;
        const char *ver =
            (esp_ota_get_partition_description(part, &d) == ESP_OK) ? d.version : "";
        esp_ota_img_states_t st;
        const char *state =
            (esp_ota_get_state_partition(part, &st) == ESP_OK) ? ota_state_name(st) : "undefined";
        p += snprintf(out + p, cap - (size_t)p,
                      "%s{\"label\":\"%s\",\"version\":\"%s\",\"state\":\"%s\","
                      "\"running\":%s,\"boot\":%s}",
                      first ? "" : ",", part->label, ver, state,
                      (running && part->address == running->address) ? "true" : "false",
                      (boot && part->address == boot->address) ? "true" : "false");
        first = false;
    }
    esp_partition_iterator_release(it);
    p += snprintf(out + p, cap - (size_t)p, "]");
    return p;
}

/*
 * Size of the crash dump kept in flash, 0 when there is none.
 *
 * Checking one means a CRC over the whole thing, and system info is polled
 * while the console is open, so the answer is worked out once. It cannot change
 * on its own: a dump is written by the panic handler, which reboots, and the
 * only way to lose one without a restart is the erase below.
 */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
static unsigned s_dump_bytes;
static bool s_dump_known;
#endif

static unsigned crash_dump_bytes(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (!s_dump_known) {
        size_t addr = 0, size = 0;
        if (esp_core_dump_image_check() == ESP_OK &&
            esp_core_dump_image_get(&addr, &size) == ESP_OK) {
            s_dump_bytes = (unsigned)size;
        }
        s_dump_known = true;
    }
    return s_dump_bytes;
#else
    return 0;
#endif
}

esp_err_t api_system_info_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const float temp_c = kvm_thermal_celsius();
    bool net_up = false;
    int net_mbps = 0;
    kvm_eth_link(&net_up, &net_mbps);
    kvm_wifi_status_t wifi;
    kvm_wifi_status(&wifi);
    const char *net_mode = wifi.mode == KVM_NET_WIFI_AP     ? "ap"
                           : wifi.mode == KVM_NET_WIFI_STA ? "wifi"
                           : wifi.mode == KVM_NET_AUTO     ? "auto"
                                                           : "ethernet";
#if CONFIG_KVM_ETH_ENABLE
    const bool has_eth = true;
#else
    const bool has_eth = false;
#endif
    /* The link carrying traffic now; differs from the mode only in "auto". */
    const char *net_active = !wifi.active                   ? "ethernet"
                             : wifi.mode == KVM_NET_WIFI_AP ? "ap"
                                                            : "wifi";
    kvm_atx_status_t atx;
    kvm_atx_status(&atx);
    bool mqtt_on = false, mqtt_conn = false;
    kvm_mqtt_status(&mqtt_on, &mqtt_conn);
    kvm_ts_status_t ts;
    kvm_ts_status(&ts);
    kvm_wg_status_t wg;
    kvm_wg_status(&wg);

    /*
     * What the active link is, in the terms an operator needs to reach the device
     * or write it into a router: the name it answers to, the IPv4 address, the MAC
     * a DHCP reservation is keyed on, and the autoconfigured IPv6 addresses.
     *
     * The browser knows the address it dialled, but that is not the same fact: it
     * may have arrived over the VPN, over mDNS, or over a v6 address, and none of
     * those tell you what the device holds on the LAN.
     */
    const char *hostname = kvm_setting_str("net_hostname");
    if (!hostname || !hostname[0]) {
        hostname = CONFIG_KVM_MDNS_HOSTNAME;
    }
    /* An SSID is any 32 bytes, and an old hostname was not checked. */
    char hostname_j[6 * 31 + 1];
    char ssid_j[6 * 32 + 1];
    json_esc(hostname, hostname_j, sizeof(hostname_j));
    /* Where another device holds the name, what this one got instead. */
    char taken_lan[64], taken_ts[64], taken_lan_j[6 * 63 + 1], taken_ts_j[6 * 63 + 1];
    kvm_net_name_taken(taken_lan, sizeof(taken_lan), taken_ts, sizeof(taken_ts));
    json_esc(taken_lan, taken_lan_j, sizeof(taken_lan_j));
    json_esc(taken_ts, taken_ts_j, sizeof(taken_ts_j));
    json_esc(wifi.ssid, ssid_j, sizeof(ssid_j));
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip4_info;
    char ip4[16] = "";
    if (netif && esp_netif_get_ip_info(netif, &ip4_info) == ESP_OK && ip4_info.ip.addr) {
        snprintf(ip4, sizeof(ip4), IPSTR, IP2STR(&ip4_info.ip));
    }
    /* In "auto" the device answers on the standby link's address as well. */
    char ip4_backup[16] = "";
    if (wifi.mode == KVM_NET_AUTO && !wifi.active) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta && esp_netif_get_ip_info(sta, &ip4_info) == ESP_OK && ip4_info.ip.addr) {
            snprintf(ip4_backup, sizeof(ip4_backup), IPSTR, IP2STR(&ip4_info.ip));
        }
    }
    uint8_t mac_raw[6] = {0};
    char mac_str[18] = "";
    if (netif && esp_netif_get_mac(netif, mac_raw) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac_raw[0], mac_raw[1],
                 mac_raw[2], mac_raw[3], mac_raw[4], mac_raw[5]);
    }

    /* The IPv6 addresses the active link holds, most routable first. Autoconfigured,
     * so nothing else tells the operator what they turned out to be. */
    char ip6[KVM_IP6_MAX_ADDRS][KVM_IP6_STRLEN];
    const int ip6_count = kvm_ipv6_addrs(ip6, KVM_IP6_MAX_ADDRS);
    char ip6_json[KVM_IP6_MAX_ADDRS * (KVM_IP6_STRLEN + 4) + 4];
    int q = snprintf(ip6_json, sizeof(ip6_json), "[");
    for (int i = 0; i < ip6_count; i++) {
        q += snprintf(ip6_json + q, sizeof(ip6_json) - (size_t)q, "%s\"%s\"", i ? "," : "", ip6[i]);
    }
    snprintf(ip6_json + q, sizeof(ip6_json) - (size_t)q, "]");

    char ota_json[512];
    ota_slots_json(ota_json, sizeof(ota_json));

    const unsigned dump_bytes = crash_dump_bytes();

    /*
     * Internal RAM twice: how much is free, and the longest run of it. The
     * second is the one that decides whether the H.264 encoder can be built -
     * it wants ~155 KB in one piece - and a device can have 300 KB free with no
     * run longer than 130. Nothing else reported it, and it took a log line in
     * the encoder to find out.
     */
    /* The Wi-Fi chip's own firmware, where this build carries one to install. */
    char coproc_json[256] = "null";
    kvm_coproc_status_t cp;
    kvm_coproc_status(&cp);
    if (cp.bundled) {
        static const char *const cp_states[] = {"idle", "updating", "done", "failed"};
        snprintf(coproc_json, sizeof(coproc_json),
                 "{\"fw\":\"%s\",\"bundled\":\"%s\",\"running\":%s,\"update\":%s,"
                 "\"state\":\"%s\",\"percent\":%d,\"msg\":\"%s\"}",
                 cp.fw, cp.bundled_fw, cp.running ? "true" : "false",
                 cp.update ? "true" : "false", cp_states[cp.state], cp.percent, cp.msg);
    }

    /* A second thermometer, where a clock chip brings one: the air by the board. */
    char board_temp[16] = "null";
    float bt;
    if (kvm_rtc_temperature(&bt)) {
        snprintf(board_temp, sizeof(board_temp), "%.2f", (double)bt);
    }

    char install_id[KVM_INSTALL_ID_LEN + 1];
    kvm_install_id(install_id);

    kvm_tools_status_t tools;
    kvm_tools_status(&tools);

    /* The P4's silicon revision, major * 100 + minor: 301 is v3.1. */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    /* Settings, keys and certificates all live in NVS; a full one loses writes. */
    nvs_stats_t nvs = {0};
    (void)nvs_get_stats(NULL, &nvs);

    char body[3328];
    int n = snprintf(body, sizeof(body),
                     "{\"project\":\"%s\",\"version\":\"%s\",\"built\":\"%s %s\","
                     "\"boardId\":\"%s\","
                     "\"idf\":\"%s\",\"partition\":\"%s\",\"updatable\":%s,\"ota\":%s,"
                     "\"trialSeconds\":%u,\"netTrialSeconds\":%u,\"netReverted\":\"%s\","
                     "\"uptimeSeconds\":%llu,\"heapFree\":%u,\"psramFree\":%u,\"psramLargest\":%u,"
                     "\"internalFree\":%u,\"internalLargest\":%u,\"internalMin\":%u,\"dmaFree\":%u,"
                     "\"tempC\":%d.%01u,\"thermal\":\"%s\","
                     "\"net\":{\"up\":%s,\"mbps\":%d,\"mode\":\"%s\",\"active\":\"%s\","
                     "\"hasEth\":%s,\"wifiUp\":%s,"
                     "\"rssi\":%d,\"ssid\":\"%s\",\"apClients\":%d,"
                     "\"hostname\":\"%s\",\"ip4\":\"%s\",\"ip4Backup\":\"%s\",\"mac\":\"%s\","
                     "\"ipv6\":%s,\"nameTaken\":{\"lan\":\"%s\",\"tailnet\":\"%s\"}},"
                     "\"atx\":{\"enabled\":%s,\"known\":%s,\"on\":%s},"
                     "\"mqtt\":{\"enabled\":%s,\"connected\":%s},"
                     "\"wg\":{\"enabled\":%s,\"up\":%s,\"address\":\"%s\",\"publicKey\":\"%s\"},"
                     "\"ts\":{\"enabled\":%s,\"up\":%s,\"address\":\"%s\",\"peers\":%d,"
                     "\"keyExpiry\":%lld,\"keyExpired\":%s},"
                     "\"jiggler\":{\"everyS\":%d,\"nudges\":%u},\"usbPad\":\"%s\","
                     "\"tools\":{\"port\":%s,\"connected\":%s},"
                     "\"coproc\":%s,\"rtc\":%s,\"rtcChip\":\"%s\",\"boardTempC\":%s,"
                     "\"crashDumpBytes\":%u,\"installId\":\"%s\",\"chipRev\":%u,"
                     "\"nvs\":{\"used\":%u,\"free\":%u,\"total\":%u}}",
                     app->project_name, app->version, app->date, app->time, kvm_board_id(),
                     app->idf_ver,
                     running ? running->label : "?", next ? "true" : "false", ota_json,
                     (unsigned)kvm_ota_trial_left(), (unsigned)kvm_net_guard_trial_left(),
                     kvm_net_guard_last_revert(), /* a fixed text, no quotes */
                     (unsigned long long)(esp_timer_get_time() / 1000000),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), (int)temp_c,
                     (unsigned)((temp_c < 0 ? -temp_c : temp_c) * 10.0f) % 10u,
                     kvm_thermal_state_name(kvm_thermal_state()),
                     net_up ? "true" : "false", net_mbps, net_mode, net_active,
                     has_eth ? "true" : "false",
                     wifi.up ? "true" : "false", wifi.rssi, ssid_j, wifi.ap_clients, hostname_j,
                     ip4, ip4_backup, mac_str, ip6_json, taken_lan_j, taken_ts_j,
                     atx.enabled ? "true" : "false", atx.have_led ? "true" : "false",
                     atx.power_on ? "true" : "false", mqtt_on ? "true" : "false",
                     mqtt_conn ? "true" : "false", wg.enabled ? "true" : "false",
                     wg.up ? "true" : "false", wg.address, wg.public_key,
                     ts.enabled ? "true" : "false", ts.up ? "true" : "false", ts.address,
                     ts.peers, (long long)ts.key_expiry, ts.key_expired ? "true" : "false",
                     (int)kvm_setting_int("jiggle_s"),
                     (unsigned)usb_hid_jiggler_nudges(), usb_hid_pad_kind(),
                     tools.port ? "true" : "false", tools.connected ? "true" : "false",
                     coproc_json,
                     kvm_rtc_present() ? "true" : "false", kvm_rtc_name(), board_temp, dump_bytes,
                     install_id, (unsigned)chip.revision, (unsigned)nvs.used_entries,
                     (unsigned)nvs.free_entries, (unsigned)nvs.total_entries);
    if (n <= 0 || n >= (int)sizeof(body)) {
        return send_json_error(req, "500 Internal Server Error", "system info too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

/* Start a WiFi scan (async - it can take several seconds, so it runs on a
 * worker and the client polls the GET below). */
esp_err_t api_wifi_scan_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t err = kvm_wifi_scan_start();
    /* ESP_ERR_INVALID_STATE just means one is already running - report scanning. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "WiFi scan unavailable on this device");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
}

/* Poll the WiFi scan status and results. */
esp_err_t api_wifi_scan_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char body[1600]; /* up to 24 APs, ~60 bytes each */
    kvm_wifi_scan_json(body, sizeof(body));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/* Install the Wi-Fi chip firmware this build carries (async; progress is in
 * system/info under "coproc"). The device restarts when it is done. */
esp_err_t api_wifi_coproc_update_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t err = kvm_coproc_update_start();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_error(req, "404 Not Found", "this build carries no Wi-Fi chip firmware");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict",
                               "the Wi-Fi chip is not running (switch to Wi-Fi first) or is busy");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"updating\"}");
}

/* The current host's USB enumeration trace, for target-OS fingerprinting. */
esp_err_t api_system_usbprobe_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char trace[500];
    usb_hid_probe_trace(trace, sizeof(trace));
    char body[600];
    /* `bus` is whether the target's port is live at all, `mounted` whether it
       has enumerated us. A dead keyboard is one of those two, and they want
       opposite things done, so both belong next to the trace. */
    int n = snprintf(body, sizeof(body),
                     "{\"os\":\"%s\",\"trace\":\"%s\",\"bus\":%s,\"mounted\":%s}",
                     usb_hid_target_os(), trace, usb_hid_bus_alive() ? "true" : "false",
                     usb_hid_ready() ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(body)) {
        n = (int)sizeof(body) - 1; /* truncated: send only what actually fits */
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

/*
 * The device's own log, as a plain text file.
 *
 * Behind the login, because it carries the network's name, the addresses and the
 * MAC - not secrets (a value marked KVM_SF_SECRET must never reach a log line)
 * but not for strangers either. It is offered as a download rather than a page
 * because its destination is a bug report.
 *
 * The copy is taken in PSRAM: 12 KB out of internal memory is 12 KB the video
 * encoder may want, and this endpoint is not worth that.
 */
/*
 * GET the device's log. With ?since=N, only what was logged after position N,
 * for the console's live view: the reply carries X-Log-Pos, the position to ask
 * from next time. Without it, the whole of the live copy (64 KB) as a file, or
 * the RTC ring if the copy never started.
 */
esp_err_t api_system_log_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    char q[48] = {0};
    char v[24] = {0};
    const bool has_q = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK;
    const bool live = has_q && httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK;
    const size_t tail_cap = kvm_log_tail_capacity();
    char *tail = heap_caps_malloc(tail_cap, MALLOC_CAP_SPIRAM);
    if (tail) {
        uint64_t pos = live ? strtoull(v, NULL, 10) : 0;
        uint64_t head = 0;
        const size_t n = kvm_log_tail(&pos, tail, tail_cap, &head);
        if (head > 0 || live) {
            char hdr[24];
            snprintf(hdr, sizeof(hdr), "%llu", (unsigned long long)pos);
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_set_hdr(req, "X-Log-Pos", hdr);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            if (!live) {
                httpd_resp_set_hdr(req, "Content-Disposition",
                                   "attachment; filename=\"espkvm-log.txt\"");
            }
            const esp_err_t err = httpd_resp_send(req, tail, (ssize_t)n);
            free(tail);
            return err;
        }
        free(tail);
    }
    const size_t cap = kvm_log_capacity() + 1;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        return send_json_error(req, "503 Service Unavailable", "no memory for the log");
    }
    kvm_log_read(buf, cap);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"espkvm-log.txt\"");
    const esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
/*
 * The crash dump the panic handler left behind, as a file.
 *
 * A device that panics reboots in a second or two and the operator sees a
 * dropped connection - the evidence lived in the serial log nobody has a cable
 * for. This hands over what the chip wrote to flash instead: registers and task
 * stacks as they were, which `esp-coredump` turns back into a backtrace against
 * the firmware's ELF.
 *
 * It is read straight out of the partition in chunks. The whole thing is at
 * most 56 KB, but it is not worth a buffer of that size when the answer can go
 * out as it is read.
 */
esp_err_t api_system_coredump_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    size_t addr = 0, size = 0;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0 ||
        size > part->size) {
        return send_json_error(req, "404 Not Found", "no crash dump kept");
    }

    char name[96];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(name, sizeof(name), "attachment; filename=\"espkvm-%s-%s.dump\"", app->version,
             kvm_board_id());
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", name);

    char chunk[1024];
    for (size_t off = 0; off < size;) {
        const size_t n = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
        if (esp_partition_read(part, off, chunk, n) != ESP_OK) {
            /* Headers are already out, so the only way to say "broken" is to
             * cut the body short - a truncated chunked body is an error to the
             * browser, which is exactly what this is. */
            ESP_LOGE(TAG, "crash dump: read failed at %u", (unsigned)off);
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            return ESP_FAIL;
        }
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/*
 * Throw the dump away.
 *
 * A dump that has been collected is noise from then on: it keeps the warning on
 * the console and in every boot line until something else crashes over it.
 */
esp_err_t api_system_coredump_delete(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not erase the dump");
    }
    s_dump_bytes = 0;
    s_dump_known = true;
    ESP_LOGW(TAG, "crash dump erased");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"erased\"}");
}
#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */

/*
 * Restart on request.
 *
 * Useful in itself - a setting marked "takes effect after a restart" needs a
 * way to take effect - and it is the only way to reach the password-reset
 * window without a hand on the board: a software restart leaves the boot
 * strapping pins as they were, so the button can already be held when it
 * happens, which a hardware reset would read as "boot into the ROM loader".
 */
esp_err_t api_system_restart_post(httpd_req_t *req)
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

/*
 * Wake the target with a Wake-on-LAN magic packet. Needs no ATX wiring - just
 * the target's MAC and a target that keeps standby power with WoL enabled.
 */
esp_err_t api_power_wake_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    const char *mac = kvm_setting_str("pwr_wol_mac");
    if (!mac || !mac[0]) {
        return send_json_error(req, "409 Conflict", "no target MAC set (Settings -> Power)");
    }
    esp_err_t err = kvm_wol_send(mac);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "400 Bad Request", "the target MAC is not a valid address");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not send the packet");
    }
    ESP_LOGW(TAG, "Wake-on-LAN sent to %s", mac);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"sent\"}");
}

/*
 * Power the target on over USB: the wake signal a keyboard sends. Wakes a
 * sleeping target, and one that is off if its BIOS powers on from the keyboard.
 * The answer says whether the bus came back within 3 s.
 */
esp_err_t api_power_usb_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    static const char *const status[] = {
        [USB_HID_POWER_AWAKE] = "{\"status\":\"awake\"}",
        [USB_HID_POWER_WOKE] = "{\"status\":\"woke\"}",
        [USB_HID_POWER_NO_ANSWER] = "{\"status\":\"no answer\"}",
    };
    const usb_hid_power_t r = usb_hid_power_on();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, status[r]);
}

/*
 * ATX power control. Each action queues a single button pulse on the device and
 * returns at once; the pulse itself (up to a five-second hard-off hold) runs on
 * the ATX worker task, not here, so the response is never held for its length.
 * The console confirms the destructive ones (hard off, reset) before calling.
 */
static esp_err_t atx_action(httpd_req_t *req, esp_err_t (*action)(void), const char *what)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    esp_err_t err = action();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict",
                               "ATX control is off or its GPIOs are not set (Settings -> Power)");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return send_json_error(req, "429 Too Many Requests", "a power action is already in progress");
    }
    if (err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "could not queue the action");
    }
    ESP_LOGW(TAG, "ATX %s requested", what);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t api_power_click_post(httpd_req_t *req)
{
    return atx_action(req, kvm_atx_power_click, "power click");
}

esp_err_t api_power_hold_post(httpd_req_t *req)
{
    return atx_action(req, kvm_atx_power_hold, "power hold (hard off)");
}

esp_err_t api_power_reset_post(httpd_req_t *req)
{
    return atx_action(req, kvm_atx_reset, "reset");
}

esp_err_t api_settings_reset_post(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }
    (void)kvm_settings_reset();
    kvm_net_guard_forget(); /* nothing to put back over a reset */
    return send_json_owned(req, kvm_settings_values_json());
}
