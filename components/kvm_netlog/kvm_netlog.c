/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_netlog.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "kvm_caps.h"
#include "kvm_notify.h"
#include "kvm_settings.h"

static const char *TAG = "netlog";

#define NL_TASK_STACK 3584
/* Above the web server: a kernel panic is a burst of hundreds of lines, and
 * lwIP holds only a few packets per socket - whatever is not read at once is
 * dropped, which is the right thing under a flood but a pity in a panic. */
#define NL_TASK_PRIO 6
#define NL_PACKET_MAX 1472
#define NL_LINE_MAX 200
/* One notification per this long at most, however many lines match. */
#define NL_ALERT_GAP_US (30LL * 1000 * 1000)

static char *s_ring;
static uint64_t s_head;
static SemaphoreHandle_t s_mu;
static volatile int s_sock = -1;
static volatile uint32_t s_gen; /* bumped by apply; the task reopens on a change */
static kvm_netlog_status_t s_st;
static char s_line[NL_LINE_MAX];
static size_t s_line_len;
static int64_t s_last_alert_us;

static void ring_append(const char *data, size_t len)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        s_ring[(s_head + i) % KVM_NETLOG_RING_BYTES] = data[i];
    }
    s_head += len;
    xSemaphoreGive(s_mu);
}

/* Case-insensitive substring. */
static bool contains(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (!n) {
        return false;
    }
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < n && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

/* A whole line arrived: does it hold one of the nc_match phrases? The text is
 * whatever anyone on the network sent, so it only ever becomes a notification,
 * and says where it came from. */
static void check_line(const char *line, const char *from)
{
    char phrases[160];
    snprintf(phrases, sizeof(phrases), "%s", kvm_setting_str("nc_match"));
    char *save = NULL;
    for (char *p = strtok_r(phrases, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        while (*p == ' ') {
            p++;
        }
        size_t len = strlen(p);
        while (len && p[len - 1] == ' ') {
            p[--len] = '\0';
        }
        if (!len || !contains(line, p)) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (s_last_alert_us && now - s_last_alert_us < NL_ALERT_GAP_US) {
            return;
        }
        s_last_alert_us = now;
        char body[300];
        snprintf(body, sizeof(body), "From %s (unverified network log): %.200s", from, line);
        kvm_notify_send("Target log alert", body, false);
        ESP_LOGW(TAG, "alert: \"%s\" in a line from %s", p, from);
        return;
    }
}

/* Lines for the phrase check; the bytes themselves go to the ring as they are. */
static void scan_lines(const char *data, size_t len, const char *from)
{
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\n' || c == '\r') {
            if (s_line_len) {
                s_line[s_line_len] = '\0';
                check_line(s_line, from);
                s_line_len = 0;
            }
        } else if (s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = c;
        }
    }
}

/* nc_from: comma-separated addresses that may send. Empty takes anyone. */
static bool allowed(const char *ip)
{
    const char *list = kvm_setting_str("nc_from");
    if (!list || !list[0]) {
        return true;
    }
    const size_t n = strlen(ip);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') {
            e++;
        }
        if ((size_t)(e - p) == n && strncmp(p, ip, n) == 0) {
            return true;
        }
        p = e;
    }
    return false;
}

static int open_socket(int port)
{
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    /* A bounded wait, so a settings change is noticed within a second. */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return s;
}

static void netlog_task(void *arg)
{
    (void)arg;
    static char buf[NL_PACKET_MAX];
    uint32_t gen = UINT32_MAX;
    for (;;) {
        if (gen != s_gen) {
            gen = s_gen;
            if (s_sock >= 0) {
                close(s_sock);
                s_sock = -1;
            }
            s_st.running = false;
            const bool on = kvm_setting_bool("nc_enable");
            const int port = (int)kvm_setting_int("nc_port");
            s_st.port = port;
            if (!on) {
                kvm_cap_report(KVM_CAP_NETLOG, false, "switched off in settings");
            } else if ((s_sock = open_socket(port)) < 0) {
                ESP_LOGE(TAG, "could not listen on UDP %d", port);
                kvm_cap_report(KVM_CAP_NETLOG, false, "UDP port %d is not free", port);
            } else {
                s_st.running = true;
                ESP_LOGI(TAG, "listening for netconsole / syslog on UDP %d", port);
                kvm_cap_report(KVM_CAP_NETLOG, true, NULL);
            }
        }
        if (s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        const int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            continue; /* the one-second wait ran out, or the socket was closed */
        }
        char ip[16];
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        if (!allowed(ip)) {
            s_st.refused++;
            continue;
        }
        s_st.packets++;
        strlcpy(s_st.last_from, ip, sizeof(s_st.last_from));
        ring_append(buf, (size_t)n);
        scan_lines(buf, (size_t)n, ip);
        /* A syslog datagram is one message with no newline; netconsole ends its
           own lines. Either way each packet stays a line of its own. */
        if (buf[n - 1] != '\n') {
            ring_append("\n", 1);
            scan_lines("\n", 1, ip);
        }
    }
}

esp_err_t kvm_netlog_init(void)
{
    if (s_ring) {
        return ESP_OK;
    }
    s_ring = heap_caps_calloc(1, KVM_NETLOG_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mu = xSemaphoreCreateMutex();
    if (!s_ring || !s_mu) {
        return ESP_ERR_NO_MEM;
    }
    /* Stack in PSRAM: internal RAM is what TLS sessions need, and this task
     * never runs while the flash cache is off. */
    if (xTaskCreateWithCaps(netlog_task, "netlog", NL_TASK_STACK, NULL, NL_TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void kvm_netlog_apply(void)
{
    s_gen++;
}

void kvm_netlog_status(kvm_netlog_status_t *out)
{
    if (!out) {
        return;
    }
    *out = s_st;
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        out->rx_bytes = s_head;
        xSemaphoreGive(s_mu);
    }
}

size_t kvm_netlog_read_from(uint64_t *pos, char *out, size_t cap, uint64_t *head)
{
    if (!s_ring || !pos || !out || !cap) {
        if (head) {
            *head = 0;
        }
        return 0;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const uint64_t oldest = s_head > KVM_NETLOG_RING_BYTES ? s_head - KVM_NETLOG_RING_BYTES : 0;
    if (*pos < oldest || *pos > s_head) {
        *pos = *pos > s_head ? s_head : oldest;
    }
    size_t n = (size_t)(s_head - *pos);
    if (n > cap) {
        n = cap;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(*pos + i) % KVM_NETLOG_RING_BYTES];
    }
    *pos += n;
    if (head) {
        *head = s_head;
    }
    xSemaphoreGive(s_mu);
    return n;
}
