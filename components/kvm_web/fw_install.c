/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fetching a published release and writing it to the spare slot.
 *
 * The shape is deliberately the same as the upload path in http_server.c - open
 * the slot, stream into it, verify, arm the boot partition, restart - so the two
 * fail the same way and the rollback protection that already exists covers this
 * as well: the image boots as pending, and confirms itself once the web server
 * is up. An image that cannot get that far is reverted by the bootloader with
 * nothing for the operator to do.
 */
#include "cJSON.h"
#include "fw_install.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_board_header.h"
#include "kvm_display.h"
#include "kvm_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "fw_install";

/* Where the images live. Not a setting: this points the device at somewhere to
 * download and run code from, so it is a compile-time constant and the only
 * thing the operator supplies is which published tag to take. */
#define RELEASE_URL_FMT "https://github.com/espkvm/espkvm/releases/download/%s/espkvm-%s-%s.bin"

/* Long enough for the URL above with the longest tag and board id. */
#define URL_MAX 256
#define CHUNK 2048
#define TASK_STACK 8192
#define TASK_PRIO 5
/* How long to wait on a socket that has gone quiet before giving up. The image
 * is ~1.8 MB and GitHub is not always brisk about starting one. */
#define HTTP_TIMEOUT_MS 20000
/* github.com -> the storage host is one hop; a couple spare costs nothing. */
#define MAX_REDIRECTS 5
/* Header buffers, both ways - see the note in the client config below. */
#define HTTP_BUF 2048
/* Silences in a row before the download is called dead, each HTTP_TIMEOUT_MS
 * long. Generous on purpose: the far end is a CDN, not the LAN. */
#define MAX_STALLS 3

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static fw_install_status_t s_status;
static char s_version[FW_INSTALL_VERSION_MAX];

/*
 * The lock is a spinlock, so what happens under it has to be short - it runs
 * with interrupts off on this core. The string is therefore laid out first and
 * only copied inside; formatting in there would hold interrupts for as long as
 * snprintf takes, on a device that is meanwhile decoding video and serving a
 * web interface.
 */
static void set_state(fw_install_state_t state, int percent, const char *message)
{
    char text[sizeof(s_status.message)];
    const size_t len = message ? strlcpy(text, message, sizeof(text)) + 1 : 0;
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.percent = percent;
    if (len) {
        memcpy(s_status.message, text, len < sizeof(text) ? len : sizeof(text));
    }
    portEXIT_CRITICAL(&s_lock);
}

void fw_install_get_status(fw_install_status_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool fw_install_busy(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_status.state == FW_INSTALL_RUNNING;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

/*
 * A tag, and nothing else.
 *
 * This string is pasted into a URL that the device then downloads and boots, so
 * the check is a whitelist rather than a search for bad characters: "v." then
 * digits, dots and dashes. No slashes, no dots doubled up - nothing that can
 * climb out of the release path or reach a different host.
 */
static bool version_ok(const char *v)
{
    if (!v || strncmp(v, "v.", 2) != 0) {
        return false;
    }
    const size_t n = strlen(v);
    if (n < 4 || n >= FW_INSTALL_VERSION_MAX) {
        return false;
    }
    if (!isdigit((unsigned char)v[2])) {
        return false;
    }
    for (size_t i = 2; i < n; i++) {
        const char c = v[i];
        const bool allowed = isdigit((unsigned char)c) || c == '.' || c == '-' ||
                             (c >= 'a' && c <= 'z');
        if (!allowed) {
            return false;
        }
        if (c == '.' && v[i - 1] == '.') {
            return false;
        }
    }
    return true;
}

static void fail(esp_http_client_handle_t http, esp_ota_handle_t ota, const char *why)
{
    if (ota) {
        esp_ota_abort(ota);
    }
    if (http) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    ESP_LOGE(TAG, "%s", why);
    kvm_display_notice("ROLLBACK", "failed", -1, 10000);
    set_state(FW_INSTALL_FAILED, -1, why);
}

static void install_task(void *arg)
{
    (void)arg;

    char url[URL_MAX];
    snprintf(url, sizeof(url), RELEASE_URL_FMT, s_version, s_version, kvm_board_id());
    ESP_LOGW(TAG, "installing %s from %s", s_version, url);
    kvm_display_notice("ROLLBACK", "fetching", 0, 60000);
    set_state(FW_INSTALL_RUNNING, -1, "asking GitHub for the image");

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        fail(NULL, 0, "no second app slot to write into");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /*
         * Room for the redirect.
         *
         * The client's default header buffer is 512 bytes, and the Location
         * GitHub sends is a signed storage URL several hundred characters long -
         * with CONFIG_ESP_HTTP_CLIENT_STRICT_HEADER_BUFFER on, that is a hard
         * "Out of buffer" rather than a truncation. The transmit side needs the
         * room too: after the redirect, that whole URL goes back out in the
         * request line.
         */
        .buffer_size = HTTP_BUF,
        .buffer_size_tx = HTTP_BUF,
        /* Redirects are followed in the loop below, not here: this setting
         * only reaches esp_http_client_perform(), which this path does not use. */
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        fail(NULL, 0, "could not start an HTTPS client");
        vTaskDelete(NULL);
        return;
    }

    /*
     * Follow the redirect by hand.
     *
     * The download URL answers 302 and the bytes live on a storage host, and
     * `disable_auto_redirect` only means anything to esp_http_client_perform().
     * This path opens the connection itself and reads from it, so nothing
     * follows the hop for us - the first attempt at this got a 302 and stopped
     * there. set_redirection() takes the Location and re-points the client;
     * the connection has to be reopened onto the new host.
     */
    esp_err_t err = ESP_OK;
    int64_t total = 0;
    int status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        err = esp_http_client_open(http, 0);
        if (err != ESP_OK) {
            fail(http, 0, "could not reach GitHub - is the device online?");
            vTaskDelete(NULL);
            return;
        }
        total = esp_http_client_fetch_headers(http);
        status = esp_http_client_get_status_code(http);
        if (status != 301 && status != 302 && status != 303 && status != 307 &&
            status != 308) {
            break;
        }
        if (hop == MAX_REDIRECTS) {
            fail(http, 0, "the download redirected too many times");
            vTaskDelete(NULL);
            return;
        }
        esp_http_client_set_redirection(http);
        esp_http_client_close(http);
    }

    if (status == 404) {
        fail(http, 0, "that release has no image for this board");
        vTaskDelete(NULL);
        return;
    }
    if (status != 200) {
        char why[64];
        snprintf(why, sizeof(why), "the download answered %d", status);
        fail(http, 0, why);
        vTaskDelete(NULL);
        return;
    }
    if (total > 0 && (size_t)total > target->size) {
        fail(http, 0, "that image is larger than the app slot");
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(target, total > 0 ? (size_t)total : OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        fail(http, 0, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "writing %lld bytes into %s", (long long)total, target->label);
    set_state(FW_INSTALL_RUNNING, total > 0 ? 0 : -1, "downloading");

    char *chunk = malloc(CHUNK);
    if (!chunk) {
        fail(http, ota, "out of memory");
        vTaskDelete(NULL);
        return;
    }

    int64_t received = 0;
    int shown = -1;
    int stalls = 0;
    while (total <= 0 || received < total) {
        const int n = esp_http_client_read(http, chunk, CHUNK);
        if (n < 0) {
            /*
             * A read that timed out with nothing to show is "not yet", not
             * "gone". esp_http_client_read returns -ESP_ERR_HTTP_EAGAIN for
             * that and ESP_FAIL for a transport that has actually broken -
             * both negative. Taking the first for the second is what cut a
             * download short with "the download broke off" on a link that was
             * merely quiet for a moment; the upload path has guarded the same
             * distinction all along (kvm_recv_stalled).
             */
            if (n == -ESP_ERR_HTTP_EAGAIN && ++stalls <= MAX_STALLS) {
                ESP_LOGW(TAG, "quiet at %lld of %lld bytes (%d/%d)", (long long)received,
                         (long long)total, stalls, MAX_STALLS);
                continue;
            }
            free(chunk);
            char why[80];
            snprintf(why, sizeof(why), "the download broke off after %lld of %lld bytes",
                     (long long)received, (long long)total);
            fail(http, ota, why);
            vTaskDelete(NULL);
            return;
        }
        stalls = 0;
        if (n == 0) {
            /* A clean end. With a known length that is short, and short means
             * a truncated image - esp_ota_end would refuse it anyway, but the
             * reason is clearer said here. */
            if (total > 0 && received < total) {
                free(chunk);
                fail(http, ota, "the download stopped part-way");
                vTaskDelete(NULL);
                return;
            }
            break;
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        if (err != ESP_OK) {
            free(chunk);
            fail(http, ota, esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        received += n;
        if (total > 0) {
            const int pct = (int)(received * 100 / total);
            if (pct / 5 != shown) {
                shown = pct / 5;
                set_state(FW_INSTALL_RUNNING, pct, "downloading");
                kvm_display_notice("ROLLBACK", "fetching", pct, 60000);
            }
        }
    }
    free(chunk);
    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    set_state(FW_INSTALL_RUNNING, 100, "checking the image");
    kvm_display_notice("ROLLBACK", "verifying", 100, 30000);
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        fail(NULL, 0, err == ESP_ERR_OTA_VALIDATE_FAILED
                          ? "what arrived is not a valid firmware image"
                          : esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        fail(NULL, 0, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "%s written to %s, restarting", s_version, target->label);
    kvm_display_notice("ROLLBACK", "restarting", 100, 20000);
    set_state(FW_INSTALL_DONE, 100, "installed; restarting");
    /*
     * Time for the console to read the status above and put its restart screen
     * up. Nothing is lost if it does not - the image is armed either way, and
     * an image that will not run is reverted by the bootloader.
     */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/*
 * Read the update manifest and hand back the version it names.
 *
 * The console has always done this from the browser, which is fine while
 * somebody is looking at it. Home Assistant is not: for the device to say "a
 * newer build exists" on its own, the device has to ask. Same URL as the
 * console reads (the upd_url setting), same rule as the rest of this file - the
 * device only reaches outside when fw_fetch says it may.
 *
 * Small answer, one request, no redirect handling: the manifest is served by
 * Pages from the URL as given.
 */
esp_err_t fw_latest_version(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!kvm_setting_bool("fw_fetch")) {
        return ESP_ERR_NOT_ALLOWED;
    }
    const char *url = kvm_setting_str("upd_url");
    if (!url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF,
        .buffer_size_tx = HTTP_BUF,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t res = ESP_FAIL;
    char body[512] = {0};
    if (esp_http_client_open(http, 0) == ESP_OK) {
        const int64_t len = esp_http_client_fetch_headers(http);
        (void)len;
        if (esp_http_client_get_status_code(http) == 200) {
            const int n = esp_http_client_read(http, body, sizeof(body) - 1);
            if (n > 0) {
                body[n] = '\0';
                cJSON *j = cJSON_Parse(body);
                if (j) {
                    const cJSON *v = cJSON_GetObjectItem(j, "version");
                    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
                        snprintf(out, out_len, "%s", v->valuestring);
                        res = ESP_OK;
                    }
                    cJSON_Delete(j);
                }
            }
        }
        esp_http_client_close(http);
    }
    esp_http_client_cleanup(http);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "update check failed (%s)", url);
    }
    return res;
}

esp_err_t fw_install_start(const char *version)
{
    if (!version_ok(version)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fw_install_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The device reaching the internet by itself is opt-in, because for most of
     * the places one of these lives it is the wrong thing to do. */
    if (!kvm_setting_bool("fw_fetch")) {
        return ESP_ERR_NOT_ALLOWED;
    }
    strlcpy(s_version, version, sizeof(s_version));
    char ver[sizeof(s_status.version)];
    const size_t vlen = strlcpy(ver, version, sizeof(ver)) + 1;
    portENTER_CRITICAL(&s_lock);
    s_status.state = FW_INSTALL_RUNNING;
    s_status.percent = -1;
    memcpy(s_status.version, ver, vlen < sizeof(ver) ? vlen : sizeof(ver));
    portEXIT_CRITICAL(&s_lock);
    set_state(FW_INSTALL_RUNNING, -1, "starting");

    if (xTaskCreate(install_task, "fw_install", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        set_state(FW_INSTALL_FAILED, -1, "could not start the download task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
