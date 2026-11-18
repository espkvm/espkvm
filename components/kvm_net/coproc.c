/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Updating the Wi-Fi co-processor from the P4. Boards ship their C6 or C5 with
 * whatever esp-hosted the maker flashed. The DFRobot FireBeetle 2, for one,
 * carries an October 2024 pre-release that cannot report a version and has only
 * the slow SDIO mode (#63). This build carries an image made from the same
 * esp_hosted as the host (tools/build-coproc.sh) and writes it through the
 * co-processor's own OTA commands, so no one needs a UART adapter.
 *
 * Old builds: OTABegin/Write/End have kept their ids since 2024, but OTAActivate
 * came in 2.6, and a build before that never answers it. Those builds switch
 * the boot slot themselves at OTAEnd. Either way the P4 restarts at once,
 * which resets the co-processor and brings the link up in the new mode.
 *
 * The write goes to the slot that is not running, so a cut in the middle leaves
 * the old firmware in place.
 */
#include "coproc.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "coproc";

#if CONFIG_KVM_WIFI && defined(KVM_COPROC_VERSION)

#include "esp_hosted.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_SLAVE_IDF_TARGET_ESP32C5
extern const uint8_t img_start[] asm("_binary_esp32c5_bin_start");
extern const uint8_t img_end[] asm("_binary_esp32c5_bin_end");
#define IMG_CHIP 0x0017 /* esp_app_format.h ESP_CHIP_ID_ESP32C5 */
#else
extern const uint8_t img_start[] asm("_binary_esp32c6_bin_start");
extern const uint8_t img_end[] asm("_binary_esp32c6_bin_end");
#define IMG_CHIP 0x000D /* ESP_CHIP_ID_ESP32C6 */
#endif

/* The size the 2024 builds' own OTA commands used; the host allows 1536. */
#define CHUNK 1400

static bool s_running;
static bool s_has_ver;
static unsigned s_ver[3];
static volatile kvm_coproc_state_t s_state;
static volatile int s_percent;
static char s_msg[96];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_state(kvm_coproc_state_t st, int pct, const char *msg)
{
    portENTER_CRITICAL(&s_lock);
    s_state = st;
    s_percent = pct;
    strlcpy(s_msg, msg ? msg : "", sizeof(s_msg));
    portEXIT_CRITICAL(&s_lock);
}

static void bundled_ver(unsigned v[3])
{
    v[0] = v[1] = v[2] = 0;
    sscanf(KVM_COPROC_VERSION, "%u.%u.%u", &v[0], &v[1], &v[2]);
}

static bool newer(const unsigned a[3], const unsigned b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

void kvm_coproc_on_up(void)
{
    s_running = true;
    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK && (v.major1 || v.minor1 || v.patch1)) {
        s_has_ver = true;
        s_ver[0] = v.major1;
        s_ver[1] = v.minor1;
        s_ver[2] = v.patch1;
        ESP_LOGI(TAG, "co-processor runs esp-hosted %u.%u.%u, this build carries %s", s_ver[0],
                 s_ver[1], s_ver[2], KVM_COPROC_VERSION);
    } else {
        ESP_LOGW(TAG, "co-processor does not report a version (an early esp-hosted); "
                      "this build carries %s", KVM_COPROC_VERSION);
    }
}

void kvm_coproc_status(kvm_coproc_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->bundled = true;
    out->running = s_running;
    strlcpy(out->bundled_fw, KVM_COPROC_VERSION, sizeof(out->bundled_fw));
    if (s_has_ver) {
        snprintf(out->fw, sizeof(out->fw), "%u.%u.%u", s_ver[0], s_ver[1], s_ver[2]);
    }
    unsigned b[3];
    bundled_ver(b);
    out->update = s_running && (!s_has_ver || newer(b, s_ver));
    portENTER_CRITICAL(&s_lock);
    out->state = s_state;
    out->percent = s_percent;
    strlcpy(out->msg, s_msg, sizeof(out->msg));
    portEXIT_CRITICAL(&s_lock);
}

static void fail(const char *what, esp_err_t err)
{
    char m[96];
    snprintf(m, sizeof(m), "%s failed (%s); the old firmware still runs", what, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", m);
    set_state(KVM_COPROC_FAILED, 0, m);
}

static void update_task(void *arg)
{
    (void)arg;
    const size_t len = img_end - img_start;
    ESP_LOGW(TAG, "installing esp-hosted %s into the co-processor (%u bytes)", KVM_COPROC_VERSION,
             (unsigned)len);

    /* Erases the whole slot first, which takes seconds; the host waits 30 s. */
    set_state(KVM_COPROC_UPDATING, 0, "Erasing");
    esp_err_t err = esp_hosted_cp_ota_begin();
    if (err != ESP_OK) {
        fail("Erase", err);
        vTaskDelete(NULL);
    }
    for (size_t off = 0; off < len; off += CHUNK) {
        const size_t n = (len - off < CHUNK) ? len - off : CHUNK;
        err = esp_hosted_cp_ota_write(img_start + off, n);
        if (err != ESP_OK) {
            fail("Write", err);
            vTaskDelete(NULL);
        }
        set_state(KVM_COPROC_UPDATING, (int)((off + n) * 100 / len), "Writing");
    }
    err = esp_hosted_cp_ota_end();
    if (err != ESP_OK) {
        fail("Check", err); /* the co-processor validates the image here */
        vTaskDelete(NULL);
    }
    /* 2.6 and later wait for this; earlier builds switched at OTAEnd and would
     * never answer it. */
    if (s_has_ver && (s_ver[0] > 2 || (s_ver[0] == 2 && s_ver[1] >= 6))) {
        err = esp_hosted_cp_ota_activate();
        if (err != ESP_OK) {
            fail("Activate", err);
            vTaskDelete(NULL);
        }
    }
    ESP_LOGW(TAG, "co-processor firmware written; restarting");
    set_state(KVM_COPROC_DONE, 100, "Written, restarting");
    /* Restart before the co-processor does: early builds reboot 5 s after
     * OTAEnd, and esp-hosted answers a lost link with abort(). The boot slot is
     * already switched, and the P4's start resets the co-processor through its
     * EN pin. The short wait lets the console see "done". */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t kvm_coproc_update_start(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE; /* the co-processor is only up in a Wi-Fi mode */
    }
    /* An app image header: magic 0xE9 first, the chip id at byte 12. */
    if (img_end - img_start < 24 || img_start[0] != 0xE9 ||
        (img_start[12] | (img_start[13] << 8)) != IMG_CHIP) {
        return ESP_ERR_INVALID_ARG; /* a build mistake, not something to write */
    }
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_state == KVM_COPROC_UPDATING || s_state == KVM_COPROC_DONE;
    if (!busy) {
        s_state = KVM_COPROC_UPDATING;
        s_percent = 0;
        strlcpy(s_msg, "Starting", sizeof(s_msg));
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(update_task, "coproc_ota", 4096, NULL, 5, NULL) != pdPASS) {
        set_state(KVM_COPROC_FAILED, 0, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* no co-processor image in this build */

void kvm_coproc_on_up(void)
{
}

void kvm_coproc_status(kvm_coproc_status_t *out)
{
    memset(out, 0, sizeof(*out));
}

esp_err_t kvm_coproc_update_start(void)
{
    (void)TAG;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
