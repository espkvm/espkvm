/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Status-display core: gather telemetry into one shared struct and hand it to the
 * configured driver on a steady tick. Knows nothing about any panel's registers,
 * transport or layout - that is the driver's job, under drivers/. One long-lived
 * task owns the panel's whole lifecycle, so there is no cross-task access to it:
 * it polls the settings and attaches, re-attaches on a driver change, or detaches,
 * all by itself.
 */
#include "kvm_display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "capture.h"
#include "kvm_caps.h"
#include "kvm_display_driver.h"
#include "kvm_ipv6.h"
#include "kvm_panels.h"
#include "kvm_settings.h"
#include "kvm_thermal.h"
#include "kvm_ts.h"
#include "wifi.h"

#define TAG "kvm_display"

#if CONFIG_KVM_ENABLE_DISPLAY

/* The drivers, indexed by kvm_panel_drv_t. A new panel needs a driver here, an
 * entry in k_panels[] and a matching "disp_type" choice. */
extern const kvm_display_driver_t kvm_display_ssd1306;
extern const kvm_display_driver_t kvm_display_sh1106;
extern const kvm_display_driver_t kvm_display_gc9a01;

static const kvm_display_driver_t *const s_drivers[] = {
    &kvm_display_ssd1306,
    &kvm_display_sh1106,
    &kvm_display_gc9a01,
};

#define RENDER_TICK_MS 1000 /* how often the driver is handed fresh telemetry */

/*
 * The notice: a caller-owned takeover of the panel, written from another task.
 *
 * It is deliberately a value rather than a callback into the caller - the
 * display task must never run someone else's code while it holds the panel, and
 * a caller in a tight polling loop (the reset button) must never be slowed down
 * by an SPI write. So the writer drops a small struct and moves on, and the task
 * repaints when it next wakes - immediately, because the write pokes it.
 */
static struct {
    char title[sizeof(((kvm_display_status_t *)0)->notice)];
    char detail[sizeof(((kvm_display_status_t *)0)->notice_detail)];
    int8_t pct;
    bool active;
    int64_t expires_us; /* 0 = no expiry */
} s_notice;
static SemaphoreHandle_t s_notice_lock;
static TaskHandle_t s_task;

void kvm_display_notice(const char *title, const char *detail, int pct, uint32_t ttl_ms)
{
    if (!s_notice_lock) {
        return; /* the feature is off, or this ran before kvm_display_init() */
    }
    if (pct > 100) {
        pct = 100;
    } else if (pct < 0) {
        pct = -1;
    }

    xSemaphoreTake(s_notice_lock, portMAX_DELAY);
    /* Repaint only on a real change: the reset button calls this every 50 ms,
     * and a full round-LCD frame is 115 KB over SPI. */
    const bool changed = !s_notice.active || s_notice.pct != (int8_t)pct ||
                         strcmp(s_notice.title, title ? title : "") != 0 ||
                         strcmp(s_notice.detail, detail ? detail : "") != 0;
    snprintf(s_notice.title, sizeof(s_notice.title), "%s", title ? title : "");
    snprintf(s_notice.detail, sizeof(s_notice.detail), "%s", detail ? detail : "");
    s_notice.pct = (int8_t)pct;
    s_notice.active = true;
    s_notice.expires_us = ttl_ms ? esp_timer_get_time() + (int64_t)ttl_ms * 1000 : 0;
    xSemaphoreGive(s_notice_lock);

    if (changed && s_task) {
        xTaskNotifyGive(s_task);
    }
}

void kvm_display_notice_clear(void)
{
    if (!s_notice_lock) {
        return;
    }
    xSemaphoreTake(s_notice_lock, portMAX_DELAY);
    const bool was = s_notice.active;
    s_notice.active = false;
    xSemaphoreGive(s_notice_lock);
    if (was && s_task) {
        xTaskNotifyGive(s_task);
    }
}

/** Copy the live notice into @p st, dropping it if its TTL has run out. */
static void take_notice(kvm_display_status_t *st)
{
    if (!s_notice_lock) {
        return;
    }
    xSemaphoreTake(s_notice_lock, portMAX_DELAY);
    if (s_notice.active && s_notice.expires_us && esp_timer_get_time() > s_notice.expires_us) {
        s_notice.active = false;
    }
    if (s_notice.active) {
        snprintf(st->notice, sizeof(st->notice), "%s", s_notice.title);
        snprintf(st->notice_detail, sizeof(st->notice_detail), "%s", s_notice.detail);
        st->notice_pct = s_notice.pct;
    }
    xSemaphoreGive(s_notice_lock);
}

/*
 * Build the Wi-Fi join string a phone camera understands. The format is fixed:
 *   WIFI:T:<WPA|nopass>;S:<ssid>;P:<pass>;;
 *
 * Inside it, the five characters \ ; , : and " are structural, so any of them
 * appearing in an SSID or passphrase has to be backslash-escaped or the scanner
 * reads the field as ending early. Our own hotspot name never contains one, but
 * the passphrase is the operator's, and a stray semicolon there would otherwise
 * produce a QR code that silently joins the wrong network name.
 *
 * An "open" hotspot is not the same string with an empty password - it uses
 * T:nopass and omits P: entirely, which is what the format expects and what
 * phones actually act on.
 */
static void escape_qr_field(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < cap; p++) {
        if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"') {
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

static void build_join_qr(char *out, size_t cap, const char *ssid, const char *pass)
{
    char e_ssid[sizeof(((kvm_display_status_t *)0)->ssid) * 2];
    escape_qr_field(e_ssid, sizeof(e_ssid), ssid);

    /* Matches wifi.c: a passphrase shorter than WPA2's minimum is not used at
     * all, and the hotspot comes up open - so the code must say so too. */
    if (pass && strlen(pass) >= 8) {
        char e_pass[132];
        escape_qr_field(e_pass, sizeof(e_pass), pass);
        snprintf(out, cap, "WIFI:T:WPA;S:%s;P:%s;;", e_ssid, e_pass);
    } else {
        snprintf(out, cap, "WIFI:T:nopass;S:%s;;", e_ssid);
    }
}

static void gather(kvm_display_status_t *st)
{
    memset(st, 0, sizeof(*st));

    const char *host = kvm_setting_str("net_hostname");
    snprintf(st->hostname, sizeof(st->hostname), "%s",
             (host && host[0]) ? host : CONFIG_KVM_MDNS_HOSTNAME);

    kvm_wifi_status_t w;
    kvm_wifi_status(&w);
    st->ap_mode = (w.mode == KVM_NET_WIFI_AP);
    snprintf(st->link, sizeof(st->link), "%s",
             w.mode == KVM_NET_WIFI_AP    ? "AP mode"
             : w.mode == KVM_NET_WIFI_STA ? "Wi-Fi"
                                          : "Ethernet");
    if (w.ssid[0]) {
        snprintf(st->ssid, sizeof(st->ssid), "%s", w.ssid);
    }
    if (st->ap_mode && w.ssid[0]) {
        const char *pass = kvm_setting_str("ap_pass");
        build_join_qr(st->join_qr, sizeof(st->join_qr), w.ssid, pass);
        /* Matches wifi.c and the QR: below WPA2's minimum the hotspot comes up
         * open, and there is no passphrase to show. */
        if (pass && strlen(pass) >= 8) {
            /* The precision, rather than a plain %s: the passphrase is a
               setting string of unknown length, and at -O2 gcc calls that a
               possible truncation. Cutting it here is deliberate. */
            snprintf(st->ap_pass, sizeof(st->ap_pass), "%.*s",
                     (int)sizeof(st->ap_pass) - 1, pass);
        }
    }

    esp_netif_t *nif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
        esp_ip4addr_ntoa(&ip.ip, st->ip, sizeof(st->ip));
    } else if (kvm_ipv6_routable(NULL, 0)) {
        /* IPv6-only: the address itself is far too long for this panel, so show
         * the name that resolves to it instead - which is what someone standing
         * in front of the device would type anyway. */
        snprintf(st->ip, sizeof(st->ip), "%s.local", st->hostname);
    }

    kvm_ts_status_t ts;
    kvm_ts_status(&ts);
    if (ts.address[0]) {
        snprintf(st->ts_ip, sizeof(st->ts_ip), "%s", ts.address);
    }

    kvm_video_status_t v;
    capture_status_get(&v);
    st->video_signal = v.signal;
    st->hres = (uint16_t)v.hres;
    st->vres = (uint16_t)v.vres;
    st->fps = (uint16_t)(v.fps_x100 / 100);
    snprintf(st->codec, sizeof(st->codec), "%s", kvm_setting_int("vid_codec") == 1 ? "H.264" : "MJPEG");

    st->temp_c = (int)(kvm_thermal_celsius() + 0.5f);
    st->heap_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    st->heap_total_kb = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
    st->psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    st->psram_total_kb = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    st->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(st->version, sizeof(st->version), "%s", app ? app->version : "?");

    st->notice_pct = -1;
    take_notice(st);
}

static void display_task(void *arg)
{
    (void)arg;
    const kvm_panel_t *panel = NULL;
    const kvm_display_driver_t *drv = NULL;
    void *ctx = NULL;
    int fails = 0;

    for (;;) {
        const bool want = kvm_setting_bool("disp_enable");
        const kvm_panel_t *sel = kvm_panel_selected();

        /* Drop the panel if switched off or another one was picked. Compare
           panels, not drivers: a new size needs re-initialising too. */
        if (ctx && (!want || sel != panel)) {
            drv->detach(ctx);
            ctx = NULL;
        }
        if (!want) {
            if (drv) {
                kvm_cap_report(KVM_CAP_DISPLAY, false, "switched off in settings");
                drv = NULL;
                panel = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        panel = sel;
        drv = s_drivers[sel->drv];

        if (!ctx) {
            esp_err_t err = drv->attach(&ctx);
            if (err != ESP_OK) {
                ctx = NULL;
                kvm_cap_report(KVM_CAP_DISPLAY, false,
                               err == ESP_ERR_NOT_FOUND ? "no I2C OLED detected (0x3C/0x3D)"
                                                        : "display not ready");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            kvm_cap_report(KVM_CAP_DISPLAY, true, NULL);
            ESP_LOGI(TAG, "%s attached", drv->label);
        }

        kvm_display_status_t st;
        gather(&st);
        if (drv->render(ctx, &st) != ESP_OK) {
            /* A single failed write is almost always transient - the shared I2C
             * bus busy while the encoder pegs the other core - so keep the panel
             * and retry. Only give up (and re-detect) after several in a row,
             * which is what an actually-unplugged panel looks like. */
            if (++fails >= 5) {
                drv->detach(ctx);
                ctx = NULL;
                fails = 0;
                kvm_cap_report(KVM_CAP_DISPLAY, false, "display stopped responding");
            }
        } else {
            fails = 0;
        }

        /* Sleep out the tick, but let a notice cut it short: waiting on the
         * notification rather than the clock is what makes a progress ring
         * follow a button in real time instead of at 1 Hz. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RENDER_TICK_MS));
    }
}

void kvm_display_init(void)
{
    /* Pinned to core 1: the H.264 encoder saturates core 0, and the display's
     * blocking I2C writes must not fight it there or they starve and time out. */
    s_notice_lock = xSemaphoreCreateMutex();
    if (!s_notice_lock) {
        ESP_LOGW(TAG, "no memory for the notice lock; notices will be dropped");
    }
    if (xTaskCreatePinnedToCore(display_task, "kvm_disp", 4096, NULL, tskIDLE_PRIORITY + 2, &s_task,
                                1) != pdPASS) {
        ESP_LOGW(TAG, "could not start the display task");
        s_task = NULL;
    }
}

#else /* !CONFIG_KVM_ENABLE_DISPLAY */

void kvm_display_init(void)
{
}

/* Callers say what is happening and do not care whether a panel exists; a build
 * with no display support swallows it rather than making every call site ask. */
void kvm_display_notice(const char *title, const char *detail, int pct, uint32_t ttl_ms)
{
    (void)title;
    (void)detail;
    (void)pct;
    (void)ttl_ms;
}

void kvm_display_notice_clear(void)
{
}

#endif
