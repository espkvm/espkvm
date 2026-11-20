/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
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
 *
 * Virtual media opens a second list: the images on the card, the rescue image
 * and Eject. A hold offers the one shown to the target, through the "media"
 * action, so the console sees the choice at once.
 */
#include "kvm_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kvm_atx.h"
#include "kvm_caps.h"
#include "kvm_display.h"
#include "kvm_display_driver.h"
#include "kvm_sched.h"
#include "kvm_settings.h"
#include "kvm_storage.h"

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

/* The media list, while it is open. In PSRAM: up to 2 KB for a few seconds. */
#define MEDIA_MAX 32
#define MEDIA_NAME 64 /* msc_image holds 63 characters */
typedef struct {
    char name[MEDIA_NAME]; /* the "media" action's arg: a file, "@rescue", "" for Eject */
} media_item_t;
static media_item_t *s_media; /* NULL = the main menu is shown */
static int s_media_count;

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
    kvm_kvmsw_status_t sw;
    kvm_kvmsw_status(&sw);
    if (sw.enabled) {
        add("Next machine", "machine", false);
    }
    if (kvm_setting_bool("msc_enable")) {
        add("Virtual media", "@media", false);
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

/* How a colour panel draws a list entry: a CD, a USB stick, the empty drive. */
static uint8_t media_icon(const char *name, bool in)
{
    if (s_at == s_media_count - 1) {
        return KVM_DISP_ICON_NONE; /* Back */
    }
    if (!name[0]) {
        return KVM_DISP_ICON_EJECT;
    }
    return (kvm_storage_image_is_cdrom(name) ? KVM_DISP_ICON_CD : KVM_DISP_ICON_STICK) |
           (in ? KVM_DISP_ICON_IN : 0) | (strcmp(name, "@rescue") == 0 ? KVM_DISP_ICON_RESCUE : 0);
}

static void draw_media(void)
{
    const char *name = s_media[s_at].name;
    const bool in = strcmp(kvm_setting_str("msc_image"), name) == 0;
    char title[24], detail[40];
    if (s_at == s_media_count - 1) {
        snprintf(title, sizeof(title), "Back");
    } else if (!name[0]) {
        snprintf(title, sizeof(title), "Eject");
    } else if (strcmp(name, "@rescue") == 0) {
        snprintf(title, sizeof(title), "%sRescue image", in ? "*" : "");
    } else {
        /* The notice title keeps 15 characters: the start of the name there,
           and the end of a long one on the line under it. */
        snprintf(title, sizeof(title), "%s%.*s", in ? "*" : "", in ? 14 : 15, name);
    }
    const size_t len = strlen(name);
    if (name[0] != '@' && len > 15 && s_at != s_media_count - 1) {
        snprintf(detail, sizeof(detail), "%d/%d ..%s", s_at + 1, s_media_count,
                 name + (len > 14 ? len - 14 : 0));
    } else {
        snprintf(detail, sizeof(detail), "%d/%d  %s", s_at + 1, s_media_count,
                 s_at == s_media_count - 1 ? "hold to go back"
                 : !name[0]                ? "hold to eject"
                 : in                      ? "mounted"
                                           : "hold to mount");
    }
    /* A colour panel draws the entry as a disc or a stick with the whole name
       on it, what holding does and the count under it. */
    kvm_disp_pic_t pic = {.icon = media_icon(name, in)};
    snprintf(pic.label, sizeof(pic.label), "%.39s",
             !name[0] ? "Eject" : strcmp(name, "@rescue") == 0 ? "Rescue image" : name);
    snprintf(pic.line1, sizeof(pic.line1), "%s",
             !name[0] ? "hold to eject" : in ? "mounted" : "hold to mount");
    snprintf(pic.line2, sizeof(pic.line2), "%d/%d", s_at + 1, s_media_count);
    kvm_display_notice_pic(title, detail, (s_at + 1) * 100 / s_media_count, IDLE_MS + 2000,
                           pic.icon ? &pic : NULL);
}

static void media_add(const char *name, uint64_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    /* Leave room for Eject, Rescue and Back. A name too long for the setting
       could not be offered anyway. */
    if (s_media_count < MEDIA_MAX - 3 && strlen(name) < MEDIA_NAME) {
        snprintf(s_media[s_media_count++].name, MEDIA_NAME, "%s", name);
    }
}

static void media_close(void)
{
    heap_caps_free(s_media);
    s_media = NULL;
    s_media_count = 0;
}

/* Replace the main menu with the media list: Eject, the card's images, the
   rescue image, Back. */
static bool media_open(void)
{
    s_media = heap_caps_calloc(MEDIA_MAX, sizeof(media_item_t), MALLOC_CAP_SPIRAM);
    if (!s_media) {
        return false;
    }
    s_media_count = 1; /* [0] is Eject, its name "" */
    const int files = kvm_storage_list_images(media_add, NULL);
    kvm_rescue_t r;
    kvm_storage_rescue_status(&r);
    if (r.has_image) {
        snprintf(s_media[s_media_count++].name, MEDIA_NAME, "@rescue");
    }
    s_media_count++; /* Back, its name "" too: told apart by being last */
    ESP_LOGI(TAG, "media list: %d file(s)%s", files < 0 ? 0 : files, r.has_image ? " + rescue" : "");
    s_at = 0;
    return true;
}

static void draw(void)
{
    char detail[40]; /* the notice keeps 23 characters, "12/16  hold to pick" is 18 */
    if (s_media) {
        draw_media();
        return;
    }
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
    media_close();
    build();
    s_at = 0;
    s_confirming = false;
    s_last_ms = now_ms();
    ESP_LOGI(TAG, "open, %d items", s_count);
    draw();
}

static void close_menu(void)
{
    media_close();
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
        s_at = (s_at + 1) % (s_media ? s_media_count : s_count);
    }
    s_last_ms = now_ms();
    draw();
}

void kvm_menu_pick(void)
{
    if (s_at < 0) {
        return;
    }
    if (s_media) {
        if (s_at == s_media_count - 1) { /* Back: the main menu, on Virtual media */
            media_close();
            for (s_at = 0; s_at < s_count && strcmp(s_items[s_at].action, "@media") != 0; s_at++) {
            }
            s_last_ms = now_ms();
            draw();
            return;
        }
        char img[MEDIA_NAME];
        snprintf(img, sizeof(img), "%s", s_media[s_at].name);
        /* Closed without clearing the panel: the entry stays up while the card
           is opened, until the result below replaces it. Cleared, the status
           page flashed in between. */
        media_close();
        s_at = -1;
        char why[64] = "";
        const bool ok = kvm_action_run("menu", "media", img, "menu", why, sizeof(why));
        ESP_LOGI(TAG, "media %s: %s", img[0] ? img : "(eject)", ok ? "done" : why);
        if (!ok) {
            kvm_display_notice(img[0] ? "Mounted" : "Ejected", why[0] ? why : "did not work", -1, 4000);
        } else if (!img[0]) {
            const kvm_disp_pic_t pic = {.icon = KVM_DISP_ICON_EJECT, .label = "Ejected",
                                        .line1 = "the drive is empty"};
            kvm_display_notice_pic("Ejected", "the drive is empty", -1, 3000, &pic);
        } else {
            /* The image by name with "mounted" under it; a colour panel spins
               the disc up. */
            const bool rescue = strcmp(img, "@rescue") == 0;
            char title[16];
            snprintf(title, sizeof(title), "%.15s", rescue ? "Rescue image" : img);
            kvm_disp_pic_t pic = {
                .icon = (kvm_storage_image_is_cdrom(img) ? KVM_DISP_ICON_CD : KVM_DISP_ICON_STICK) |
                        KVM_DISP_ICON_IN | (rescue ? KVM_DISP_ICON_RESCUE : 0),
                .line1 = "mounted"};
            snprintf(pic.label, sizeof(pic.label), "%.39s", rescue ? "Rescue image" : img);
            kvm_display_notice_pic(title, "mounted", -1, 3000, &pic);
        }
        return;
    }
    const item_t it = s_items[s_at];
    if (strcmp(it.action, "@media") == 0) {
        s_last_ms = now_ms();
        if (!media_open()) {
            close_menu();
            kvm_display_notice(it.label, "out of memory", -1, 4000);
            return;
        }
        draw();
        return;
    }
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
