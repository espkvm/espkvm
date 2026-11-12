/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * GET /api/v1/screen/text: the screen read as characters, and the JSON pieces
 * the text push on /video shares with it.
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

/*
 * GET /api/v1/screen/text - what the screen says, when it says it in text.
 *
 * Present only while the target is in a character mode and the picture has
 * settled: a BIOS, a boot loader, a console. Everything else answers 204, which
 * the console reads as "there is nothing to copy" rather than an error - it is
 * the normal state of a machine that has finished booting.
 *
 * The grid comes with the reading so the console can lay a selectable text layer
 * over the video without guessing where the cells are.
 */

/*
 * The reading as UTF-8, one line per row, in PSRAM - the caller frees it.
 *
 * Three bytes per cell is the worst case: the box-drawing characters a BIOS
 * frames its panes with are three bytes of UTF-8 each. A 240x68 grid is 49 KB
 * of it, big and short-lived, so it does not belong in the internal heap the
 * TLS sessions live in.
 */
char *screentext_utf8_alloc(const screentext_grid_t *g)
{
    const size_t cap = (size_t)g->cols * g->rows * 3u + g->rows + 1u;
    char *text = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!text) {
        return NULL;
    }
    screentext_to_utf8(g, text, cap);
    return text;
}

/*
 * Which cells are drawn the other way round from the rest of the screen - a
 * menu's selected row, a highlighted button - as runs of [row, column, length].
 * Text alone loses the one thing a character screen says about selection, which
 * is exactly what somebody navigating a boot menu needs to see.
 *
 * Runs rather than a bitmap: a screen has one or two of them, and a bitmap would
 * be 250 bytes of mostly zeroes.
 */
/**
 * The marked runs on this grid, as {row, first column, length}.
 *
 * Taken out of the JSON builder because the push path has to COMPARE it too: on
 * a boot menu the highlight bar is the only thing that moves, so a reading whose
 * characters are identical is still a different screen to look at.
 *
 * @return how many runs were written, at most HIGHLIGHT_RUNS_MAX
 */
int highlight_runs(const screentext_grid_t *grid, uint16_t out[][3])
{
    int made = 0;
    for (uint16_t r = 0; r < grid->rows && made < HIGHLIGHT_RUNS_MAX; r++) {
        uint16_t c = 0;
        while (c < grid->cols && made < HIGHLIGHT_RUNS_MAX) {
            if (!screentext_marked(grid, (size_t)r * grid->cols + c)) {
                c++;
                continue;
            }
            const uint16_t start = c;
            while (c < grid->cols && screentext_marked(grid, (size_t)r * grid->cols + c)) {
                c++;
            }
            out[made][0] = r;
            out[made][1] = start;
            out[made][2] = (uint16_t)(c - start);
            made++;
        }
    }
    return made;
}

static void json_add_highlight_runs(cJSON *o, const uint16_t runs[][3], int n)
{
    if (n <= 0) {
        return;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return;
    }
    for (int i = 0; i < n; i++) {
        const int run[3] = {runs[i][0], runs[i][1], runs[i][2]};
        cJSON *item = cJSON_CreateIntArray(run, 3);
        if (!item) {
            break;
        }
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(o, "highlight", arr);
}

void json_add_highlight(cJSON *o, const screentext_grid_t *grid)
{
    uint16_t runs[HIGHLIGHT_RUNS_MAX][3];
    json_add_highlight_runs(o, runs, highlight_runs(grid, runs));
}

/* What the watch found on this screen, if it was asked to look. */
void json_add_alert(cJSON *o)
{
    char alert[SCREENTEXT_ALERT_MAX];
    if (screentext_alert_get(alert, sizeof(alert), NULL)) {
        cJSON_AddStringToObject(o, "alert", alert);
    }
}

/*
 * Where the grid sits in the frame, and how big that frame was. A UEFI console
 * centres its text rather than filling the screen, so an overlay that assumes
 * the grid starts at 0,0 is off by the margin.
 */
void json_add_geometry(cJSON *o, const screentext_grid_t *grid, uint32_t age_ms)
{
    cJSON_AddNumberToObject(o, "cols", grid->cols);
    cJSON_AddNumberToObject(o, "rows", grid->rows);
    cJSON_AddNumberToObject(o, "cellWidth", grid->cell_w);
    cJSON_AddNumberToObject(o, "cellHeight", grid->cell_h);
    cJSON_AddNumberToObject(o, "originX", grid->x0);
    cJSON_AddNumberToObject(o, "originY", grid->y0);
    cJSON_AddNumberToObject(o, "width", grid->width);
    cJSON_AddNumberToObject(o, "height", grid->height);
    cJSON_AddNumberToObject(o, "confidence", grid->confidence);
    cJSON_AddNumberToObject(o, "ageMs", (double)age_ms);
}

esp_err_t api_screen_text_get(httpd_req_t *req)
{
    if (!kvm_auth_check(req)) {
        return kvm_auth_challenge(req);
    }

    /* Asking is what unlocks a wide mode: the device reads narrow ones by
     * itself, and a 1080p console only while somebody wants it. */
    screentext_request();

    /* A 240x68 grid is 33 KB and the text off it another 49; both are big,
     * short-lived and never touched by an interrupt, so they belong in PSRAM
     * rather than in the internal heap the TLS sessions live in. */
    screentext_grid_t *grid = heap_caps_malloc(sizeof(*grid), MALLOC_CAP_SPIRAM);
    if (!grid) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    uint32_t age_ms = 0;

    /*
     * Wait for the first reading of a mode nothing was reading a moment ago.
     *
     * A narrow mode is already being read every time the picture settles, so
     * whatever is on file is current and there is nothing to wait for. A wide
     * one was being skipped until the request above, so answering "no text"
     * straight away would answer about a screen nobody had looked at - and the
     * operator pressing Copy would be told there is nothing to copy while the
     * scan they just triggered was still coming. One picture has to settle
     * (300 ms) and be read, so wait about a second, then give up honestly.
     */
    kvm_video_status_t vs;
    capture_status_get(&vs);
    const bool wakes_on_ask = vs.signal && screentext_mode_supported(vs.hres, vs.vres) &&
                              !screentext_mode_unprompted(vs.hres, vs.vres);
    for (int waited = 0; !screentext_latest(grid, &age_ms); waited += 100) {
        if (!wakes_on_ask || waited >= 1200) {
            free(grid);
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, NULL, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    char *text = screentext_utf8_alloc(grid);
    if (!text) {
        free(grid);
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }

    cJSON *o = cJSON_CreateObject();
    if (o) {
        json_add_geometry(o, grid, age_ms);
        cJSON_AddStringToObject(o, "text", text);
        json_add_highlight(o, grid);
        json_add_alert(o);
    }
    free(text);
    free(grid);
    if (!o) {
        return send_json_error(req, "500 Internal Server Error", "out of memory");
    }
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json_owned(req, json);
}
