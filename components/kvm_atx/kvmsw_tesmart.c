/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A TESmart KVM switch over the network. It takes 6-byte commands on TCP port
 * 5000, with no sign-in:
 *
 *   AA BB 03 01 CH EE   select machine CH, from 1
 *   AA BB 03 10 00 EE   which machine is selected? The reply is 6 bytes, the
 *                       machine in byte 4, from 0
 *
 * The bytes follow PiKVM's TESmart driver (kvmd/plugins/ugpio/tesmart.py),
 * which opens a connection for each command and closes it after, as here. A
 * switch may also send reports of its own, so the reply is looked for in what
 * comes back rather than taken as the first six bytes.
 *
 * Not tried on a real switch yet.
 */
#include "kvmsw_tesmart.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "tesmart";

#define TIMEOUT_MS 3000
#define POLL_MS 10000
/* A switch takes a moment to settle after a change, PiKVM waits 1 s. */
#define AFTER_SWITCH_MS 1000

static QueueHandle_t s_q; /* int: the machine to select, from 0 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_host[64];
static int s_port = 5000;
static int s_ports = 2;
static bool s_on;
static int s_machine;     /* from 0, as the switch last said */
static bool s_known;      /* the switch has answered at least once */
static bool s_error;      /* the last command got no answer */
static volatile int s_pending;

static int connect_to(const char *host, int port)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    if (lwip_getaddrinfo(host, port_s, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "cannot resolve %s", host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        lwip_freeaddrinfo(res);
        return -1;
    }
    /* A connect to an address where nothing answers would wait far too long:
       non-blocking, with a timeout. */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    lwip_freeaddrinfo(res);
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r != 0) {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(fd, &wr);
        struct timeval tv = {.tv_sec = TIMEOUT_MS / 1000, .tv_usec = 0};
        int err = 0;
        socklen_t len = sizeof(err);
        if (select(fd + 1, NULL, &wr, NULL, &tv) != 1 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv = {.tv_sec = TIMEOUT_MS / 1000, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* Send one command. With @p reply, wait for the answer to the query and put
   its machine byte there. False when the switch did not take it. */
static bool command(const char *host, int port, uint8_t a, uint8_t b, int *reply)
{
    int fd = connect_to(host, port);
    if (fd < 0) {
        return false;
    }
    const uint8_t cmd[6] = {0xAA, 0xBB, 0x03, a, b, 0xEE};
    bool ok = send(fd, cmd, sizeof(cmd), 0) == (int)sizeof(cmd);
    if (ok && reply) {
        /* Up to 64 bytes: the answer may come split, or after a report. */
        uint8_t buf[64];
        int got = 0;
        ok = false;
        while (!ok && got < (int)sizeof(buf)) {
            int n = recv(fd, buf + got, sizeof(buf) - got, 0);
            if (n <= 0) {
                break;
            }
            got += n;
            /* Any AA BB 03 .. .. EE frame, machine in byte 4: what PiKVM
               reads too, without looking at byte 3. */
            for (int i = 0; i + 6 <= got; i++) {
                if (buf[i] == 0xAA && buf[i + 1] == 0xBB && buf[i + 2] == 0x03 &&
                    buf[i + 5] == 0xEE) {
                    *reply = buf[i + 4];
                    ok = true;
                    break;
                }
            }
        }
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return ok;
}

static void query(const char *host, int port, int ports)
{
    int m = -1;
    const bool ok = command(host, port, 0x10, 0x00, &m);
    taskENTER_CRITICAL(&s_mux);
    s_error = !ok;
    if (ok && m >= 0 && m < ports) {
        s_machine = m;
        s_known = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!ok) {
        ESP_LOGW(TAG, "%s:%d does not answer", host, port);
    }
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        int want = -1;
        const bool got = xQueueReceive(s_q, &want, pdMS_TO_TICKS(POLL_MS)) == pdTRUE;
        char host[64];
        int port, ports;
        bool on;
        taskENTER_CRITICAL(&s_mux);
        memcpy(host, s_host, sizeof(host));
        port = s_port;
        on = s_on;
        ports = s_ports;
        taskEXIT_CRITICAL(&s_mux);
        if (!on || !host[0]) {
            if (got) {
                taskENTER_CRITICAL(&s_mux);
                s_pending = 0;
                taskEXIT_CRITICAL(&s_mux);
            }
            continue;
        }
        if (got) {
            /* Only the newest wish counts. */
            int newer;
            while (xQueueReceive(s_q, &newer, 0) == pdTRUE) {
                want = newer;
            }
            ESP_LOGI(TAG, "machine %d", want + 1);
            if (!command(host, port, 0x01, (uint8_t)(want + 1), NULL)) {
                taskENTER_CRITICAL(&s_mux);
                s_error = true;
                s_pending = 0;
                taskEXIT_CRITICAL(&s_mux);
                ESP_LOGW(TAG, "%s:%d did not take the command", host, port);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(AFTER_SWITCH_MS));
            taskENTER_CRITICAL(&s_mux);
            s_machine = want; /* until the query below says otherwise */
            s_known = true;
            taskEXIT_CRITICAL(&s_mux);
        }
        query(host, port, ports);
        if (got) {
            taskENTER_CRITICAL(&s_mux);
            s_pending = 0;
            taskEXIT_CRITICAL(&s_mux);
        }
    }
}

esp_err_t kvmsw_tesmart_apply(bool on, const char *host, int port, int ports)
{
    if (!s_q) {
        s_q = xQueueCreate(4, sizeof(int));
        if (!s_q || xTaskCreate(task, "tesmart", 3072, NULL, 4, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    taskENTER_CRITICAL(&s_mux);
    const bool changed = !s_on || strncmp(s_host, host ? host : "", sizeof(s_host)) != 0 ||
                         s_port != port;
    s_on = on && host && host[0];
    strlcpy(s_host, host ? host : "", sizeof(s_host));
    s_port = port > 0 ? port : 5000;
    s_ports = ports;
    if (changed) {
        s_known = false;
        s_error = false;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void kvmsw_tesmart_status(int *machine, bool *busy, bool *error)
{
    taskENTER_CRITICAL(&s_mux);
    *machine = s_known ? s_machine : -1;
    *busy = s_pending > 0;
    *error = s_error;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t kvmsw_tesmart_select(int machine)
{
    if (!s_q) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_mux);
    s_pending = 1;
    taskEXIT_CRITICAL(&s_mux);
    if (xQueueSend(s_q, &machine, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
