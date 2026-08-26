/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_http_server.h"

httpd_handle_t http_server_start(void);

/** Put the headers every answer carries on this response, before its body. */
void kvm_web_security_headers(httpd_req_t *req);
