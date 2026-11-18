/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_http_server.h"

httpd_handle_t http_server_start(void);

/* A viewer that is not a console tab, such as a VNC client, for the list
 * /api/v1/viewers gives. */
typedef struct {
    char ip[48];
    const char *kind; /* "vnc" */
    bool encrypted;
    int64_t since_us; /* esp_timer time it connected */
} kvm_web_viewer_t;

/* list fills up to max entries and returns how many. One source is kept. */
void kvm_web_viewers_source(int (*list)(kvm_web_viewer_t *out, int max));

/** Put the headers every answer carries on this response, before its body. */
void kvm_web_security_headers(httpd_req_t *req);
