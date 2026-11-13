/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A menu on the status screen, worked with the buttons on the box. A button
 * whose press or hold is set to "menu" opens it. While it is open, any button's
 * short press moves to the next item and a hold picks it. An item that cuts
 * power or restarts something asks for a second hold, and a press there says
 * no. The last item is Back, and the menu also closes by itself after a while
 * without a press.
 *
 * The menu is drawn as a display notice: one item at a time, its name large,
 * its place in the list under it, and the place as the progress gauge. Every
 * display driver already draws notices, so the menu works on every panel and
 * over any page without a line of driver code.
 */
#include "kvm_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "kvm_caps.h"
#include "kvm_display.h"
#include "kvm_sched.h"
#include "kvm_settings.h"

static const char *TAG = "menu";

#define IDLE_MS 20000 /* closes by itself after this long without a press */

typedef struct {
    const char *label;  /* fits the notice title: 15 characters at most */
    const char *action; /* a kvm_action_run action, "" for Back */
    bool confirm;       /* asks for a second hold: it cuts power or restarts */
} item_t;

#define MAX_ITEMS 16
static item_t s_items[MAX_ITEMS];
static int s_count;
static int s_at = -1; /* -1 = closed */
static int64_t s_last_ms;
static bool s_confirming; /* the item shown waits for a second hold */

static void add(const char *label, const char *action, bool confirm)
{
    if (s_count < MAX_ITEMS) {
        s_items[s_count++] = (item_t){label, action, confirm};
    }
}

/* Only what this device can do now: no ATX items without the wiring, no
   Wake-on-LAN without a MAC. */
static void build(void)
{
    s_count = 0;
    add("Power on (USB)", "usbwake", false);
    if (kvm_cap_available(KVM_CAP_WOL) && kvm_setting_str("pwr_wol_mac")[0]) {
        add("Wake on LAN", "wol", false);
    }
    if (kvm_cap_active(KVM_CAP_ATX)) {
        add("Press power", "power", false);
        add("Press reset", "reset", true);
        add("Force off", "poweroff", true);
    }
    add("Screenshot", "screenshot", false);
    add("Save clip", "clip", false);
    add("Screen off", "displaytoggle", false);
    if (kvm_cap_available(KVM_CAP_WIFI)) {
        /* Both restart the device into another network mode. */
        add("Hotspot on/off", "hotspot", true);
        add("Next network", "netnext", true);
    }
    add("Restart ESP-KVM", "restart", true);
    add("Back", "", false);
}

static void draw(void)
{
    char detail[40]; /* the notice keeps 23 characters, "12/16  hold to pick" is 18 */
    if (s_confirming) {
        kvm_display_notice(s_items[s_at].label, "hold again: sure?", 100, IDLE_MS + 2000);
        return;
    }
    snprintf(detail, sizeof(detail), "%d/%d  hold to pick", s_at + 1, s_count);
    /* The TTL is a backstop: kvm_menu_tick closes the menu on time anyway. */
    kvm_display_notice(s_items[s_at].label, detail, (s_at + 1) * 100 / s_count, IDLE_MS + 2000);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

bool kvm_menu_is_open(void)
{
    return s_at >= 0;
}

void kvm_menu_open(void)
{
    build();
    s_at = 0;
    s_confirming = false;
    s_last_ms = now_ms();
    ESP_LOGI(TAG, "open, %d items", s_count);
    draw();
}

static void close_menu(void)
{
    s_at = -1;
    kvm_display_notice_clear();
}

void kvm_menu_next(void)
{
    if (s_at < 0) {
        return;
    }
    /* A press while asked to confirm says no, and stays on the item. */
    if (s_confirming) {
        s_confirming = false;
    } else {
        s_at = (s_at + 1) % s_count;
    }
    s_last_ms = now_ms();
    draw();
}

void kvm_menu_pick(void)
{
    if (s_at < 0) {
        return;
    }
    const item_t it = s_items[s_at];
    if (it.confirm && !s_confirming) {
        s_confirming = true;
        s_last_ms = now_ms();
        draw();
        return;
    }
    s_confirming = false;
    close_menu();
    if (!it.action[0]) {
        ESP_LOGI(TAG, "closed with Back");
        return;
    }
    ESP_LOGI(TAG, "picked %s", it.label);
    /* Say how it went, except for Screen off, which would only light it again.
       Restart never comes back to say anything. */
    if (strcmp(it.action, "usbwake") == 0) {
        kvm_display_notice(it.label, "waiting...", -1, 5000);
    }
    char why[64] = "";
    const bool ok = kvm_action_run("menu", it.action, NULL, "menu", why, sizeof(why));
    if (strcmp(it.action, "displaytoggle") != 0) {
        kvm_display_notice(it.label, ok ? "done" : (why[0] ? why : "did not work"), -1, ok ? 2000 : 4000);
    }
}

void kvm_menu_tick(void)
{
    if (s_at >= 0 && now_ms() - s_last_ms >= IDLE_MS) {
        ESP_LOGI(TAG, "closed after %d s without a press", IDLE_MS / 1000);
        close_menu();
    }
}
