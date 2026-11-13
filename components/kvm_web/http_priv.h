/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the files http_server.c was split into: the helpers they all use and
 * the handlers http_server.c registers. Names kept as they were.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "screentext.h"

/** Body limit for settings writes; the whole table serialises well under this. */
#define API_BODY_MAX 4096

/* Enough for any real screen: a menu has one highlighted row, a dialog two. */
#define HIGHLIGHT_RUNS_MAX 128

/* http_server.c */
esp_err_t send_json_owned(httpd_req_t *req, char *json);
esp_err_t send_ok(httpd_req_t *req);
cJSON *read_json_body(httpd_req_t *req);
bool viewing_allowed(httpd_req_t *req, esp_err_t *out);
extern httpd_handle_t s_httpd;

/* system_api.c */
esp_err_t api_capabilities_get(httpd_req_t *req);
esp_err_t api_settings_schema_get(httpd_req_t *req);
esp_err_t api_pins_get(httpd_req_t *req);
esp_err_t api_token_post(httpd_req_t *req);
esp_err_t api_token_delete(httpd_req_t *req);
esp_err_t api_settings_get(httpd_req_t *req);
esp_err_t api_settings_put(httpd_req_t *req);
esp_err_t api_video_status_get(httpd_req_t *req);
esp_err_t api_system_info_get(httpd_req_t *req);
esp_err_t api_wifi_scan_post(httpd_req_t *req);
esp_err_t api_wifi_scan_get(httpd_req_t *req);
esp_err_t api_wifi_coproc_update_post(httpd_req_t *req);
esp_err_t api_system_usbprobe_get(httpd_req_t *req);
esp_err_t api_system_log_get(httpd_req_t *req);
esp_err_t api_system_coredump_get(httpd_req_t *req);
esp_err_t api_system_coredump_delete(httpd_req_t *req);
esp_err_t api_system_restart_post(httpd_req_t *req);
esp_err_t api_power_wake_post(httpd_req_t *req);
esp_err_t api_power_usb_post(httpd_req_t *req);
esp_err_t api_power_click_post(httpd_req_t *req);
esp_err_t api_power_hold_post(httpd_req_t *req);
esp_err_t api_power_reset_post(httpd_req_t *req);
esp_err_t api_settings_reset_post(httpd_req_t *req);

/* screentext_api.c */
char *screentext_utf8_alloc(const screentext_grid_t *g);
int highlight_runs(const screentext_grid_t *grid, uint16_t out[][3]);
void json_add_highlight(cJSON *o, const screentext_grid_t *grid);
void json_add_alert(cJSON *o);
void json_add_geometry(cJSON *o, const screentext_grid_t *grid, uint32_t age_ms);
esp_err_t api_screen_text_get(httpd_req_t *req);

/* update_api.c */
esp_err_t api_system_update_post(httpd_req_t *req);
esp_err_t api_system_install_post(httpd_req_t *req);
esp_err_t api_system_install_get(httpd_req_t *req);
esp_err_t api_system_update_keep_post(httpd_req_t *req);
esp_err_t api_system_update_rollback_post(httpd_req_t *req);
esp_err_t api_system_boot_slot_post(httpd_req_t *req);

/* storage_api.c */
esp_err_t api_storage_images_get(httpd_req_t *req);
esp_err_t api_storage_upload_post(httpd_req_t *req);
esp_err_t api_storage_delete_post(httpd_req_t *req);
esp_err_t api_storage_rescue_post(httpd_req_t *req);

/* hid_api.c */
esp_err_t api_video_frame_get(httpd_req_t *req);
esp_err_t api_hid_reattach_post(httpd_req_t *req);
esp_err_t api_hid_move_post(httpd_req_t *req);
esp_err_t api_hid_click_post(httpd_req_t *req);
esp_err_t api_hid_key_post(httpd_req_t *req);
esp_err_t api_hid_pad_post(httpd_req_t *req);
esp_err_t api_hid_type_post(httpd_req_t *req);

/* automation_api.c */
esp_err_t api_runbooks_status_get(httpd_req_t *req);
esp_err_t api_runbooks_run_post(httpd_req_t *req);
esp_err_t api_video_reconnect_post(httpd_req_t *req);
esp_err_t api_runbooks_stop_post(httpd_req_t *req);
esp_err_t api_schedules_status_get(httpd_req_t *req);
esp_err_t api_schedules_run_post(httpd_req_t *req);
esp_err_t api_notify_status_get(httpd_req_t *req);
esp_err_t api_notify_chats_post(httpd_req_t *req);
esp_err_t api_notify_test_post(httpd_req_t *req);

/* ws_server.c */
extern __thread bool t_stream_sender;
esp_err_t ws_send_frame(httpd_handle_t server, int fd, httpd_ws_frame_t *frame);
void ws_send_binary(int fd, const uint8_t *data, size_t len);
void ws_broadcast_update(uint8_t phase, uint8_t percent);
void on_hid_leds(uint8_t leds, void *user);
esp_err_t http_sess_open_cb(httpd_handle_t hd, int sockfd);
void http_sess_close_cb(httpd_handle_t hd, int sockfd);
esp_err_t ws_input_handler(httpd_req_t *req);
esp_err_t api_viewers_get(httpd_req_t *req);
bool ws_server_init(void);

/* tls_api.c */
esp_err_t api_tls_get(httpd_req_t *req);
esp_err_t api_tls_acme_post(httpd_req_t *req);
esp_err_t api_tls_cert_put(httpd_req_t *req);
esp_err_t api_tls_cert_delete(httpd_req_t *req);

/* video_ws.c */
extern uint8_t s_video_client_count;
extern uint8_t s_text_client_count;
extern volatile int s_stream_workers;
void video_remove_client(int fd);
esp_err_t ws_pre_handshake(httpd_req_t *req);
esp_err_t video_ws_handler(httpd_req_t *req);
esp_err_t stream_get(httpd_req_t *req);
void video_ws_init(httpd_handle_t h);
