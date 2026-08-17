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
#include "kvm_settings.h"
#include "kvm_thermal.h"
#include "kvm_ts.h"
#include "wifi.h"

#define TAG "kvm_display"

#if CONFIG_KVM_ENABLE_DISPLAY

/* The panels the firmware knows how to drive. Add a driver's extern here and a
 * matching "disp_type" choice to make it selectable. */
extern const kvm_display_driver_t kvm_display_ssd1306;
extern const kvm_display_driver_t kvm_display_sh1106;
extern const kvm_display_driver_t kvm_display_gc9a01;

static const kvm_display_driver_t *const s_drivers[] = {
    &kvm_display_ssd1306,
    &kvm_display_sh1106,
    &kvm_display_gc9a01,
};

#define RENDER_TICK_MS 1000 /* how often the driver is handed fresh telemetry */

static const kvm_display_driver_t *pick_driver(void)
{
    /* disp_type is an enum; its value is the choice index, and the choices are
     * kept in the same order as this driver table (see the settings table). */
    const size_t n = sizeof(s_drivers) / sizeof(s_drivers[0]);
    const int idx = (int)kvm_setting_int("disp_type");
    return (idx >= 0 && (size_t)idx < n) ? s_drivers[idx] : s_drivers[0];
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
}

static void display_task(void *arg)
{
    (void)arg;
    const kvm_display_driver_t *drv = NULL;
    void *ctx = NULL;
    int fails = 0;

    for (;;) {
        const bool want = kvm_setting_bool("disp_enable");
        const kvm_display_driver_t *sel = pick_driver();

        /* Drop the panel if switched off or the chosen driver changed. */
        if (ctx && (!want || sel != drv)) {
            drv->detach(ctx);
            ctx = NULL;
        }
        if (!want) {
            if (drv) {
                kvm_cap_report(KVM_CAP_DISPLAY, false, "switched off in settings");
                drv = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        drv = sel;

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

        vTaskDelay(pdMS_TO_TICKS(RENDER_TICK_MS));
    }
}

void kvm_display_init(void)
{
    /* Pinned to core 1: the H.264 encoder saturates core 0, and the display's
     * blocking I2C writes must not fight it there or they starve and time out. */
    if (xTaskCreatePinnedToCore(display_task, "kvm_disp", 4096, NULL, tskIDLE_PRIORITY + 2, NULL,
                                1) != pdPASS) {
        ESP_LOGW(TAG, "could not start the display task");
    }
}

#else /* !CONFIG_KVM_ENABLE_DISPLAY */

void kvm_display_init(void)
{
}

#endif
