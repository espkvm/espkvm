/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device profiles over the real settings store and table, on a fake NVS.
 * Each boot() is a forked child, so a restart starts from what is stored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "kvm_profiles.h"
#include "kvm_settings.h"
#include "nvs.h"

static int failures;

#define CHECK(cond)                                                 \
    do {                                                            \
        if (!(cond)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                             \
        }                                                           \
    } while (0)

/* What the real build gets from kvm_caps.c and kvm_board. */
const char *kvm_cap_key(kvm_cap_t cap)
{
    (void)cap;
    return "cap";
}

const char *kvm_board_reserved_by(int gpio)
{
    (void)gpio;
    return NULL;
}

static int s_notified;
static char s_last_key[32];

static void on_change(const char *key, void *user)
{
    (void)user;
    s_notified++;
    strncpy(s_last_key, key, sizeof(s_last_key) - 1);
}

static void boot(void)
{
    CHECK(kvm_settings_init() == ESP_OK);
    CHECK(kvm_settings_subscribe(on_change, NULL) == ESP_OK);
    s_notified = 0;
}

static int run(void (*phase)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        boot();
        phase();
        fflush(stdout);
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

static int stored_i32(const char *ns, const char *key, int32_t *out)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t e = nvs_get_i32(h, key, out);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static cJSON *profiles(void)
{
    char *s = kvm_profiles_json();
    cJSON *o = cJSON_Parse(s);
    free(s);
    return o;
}

static uint8_t create(const char *body, bool *reboot)
{
    uint8_t id = 0;
    char err[96];
    esp_err_t e = kvm_profiles_create(body, &id, reboot, err, sizeof(err));
    if (e != ESP_OK) {
        printf("create %s: %s\n", body, err);
    }
    CHECK(e == ESP_OK);
    return id;
}

/* No profile made: settings live where they always did, nothing new in NVS. */
static void fresh(void)
{
    CHECK(kvm_setting_int("mouse_mode") == 0);
    CHECK(kvm_setting_set_int("mouse_mode", 1) == ESP_OK);
    int32_t v = -1;
    CHECK(stored_i32("kvm", "mouse_mode", &v) == 0 && v == 1);
    CHECK(fake_nvs_keys("kvm_prof") == -1);
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);

    cJSON *p = profiles();
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p, "profiles")) == 1);
    /* Every preset names values the table takes. */
    const cJSON *pre;
    cJSON_ArrayForEach(pre, cJSON_GetObjectItem(p, "presets"))
    {
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(pre, "set")) > 0);
    }
    cJSON_Delete(p);
}

static void deck(void)
{
    bool reboot = true;
    uint8_t id = create("{\"from\":\"steamdeck\"}", &reboot);
    CHECK(id == 1);
    CHECK(!reboot);
    CHECK(kvm_profiles_active() == 1 && kvm_profiles_manual());
    CHECK(kvm_setting_int("target_os") == 3);
    /* target_os and the hidden buttons changed; mouse_mode and the desk did not
       (the defaults already). */
    CHECK(s_notified == 2);
    CHECK(strcmp(kvm_setting_str("ui_hidden"), "netlog,serial") == 0);

    /* A profile key goes to the profile, anything else to Default. */
    CHECK(kvm_setting_set_int("mouse_sens", 250) == ESP_OK);
    int32_t v;
    CHECK(stored_i32("kvm", "mouse_sens", &v) == -1);
    CHECK(kvm_setting_set_int("jiggle_s", 30) == ESP_OK);
    CHECK(stored_i32("kvm", "jiggle_s", &v) == 0 && v == 30);
    /* The same value as in effect makes no value of the profile's own. */
    CHECK(kvm_setting_set_int("scroll_inv", 0) == ESP_OK);

    cJSON *p = profiles();
    const cJSON *set = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(p, "profiles"), 1), "set");
    CHECK(cJSON_GetObjectItem(set, "mouse_sens") != NULL);
    CHECK(cJSON_GetObjectItem(set, "mouse_mode")->valueint == 1);
    CHECK(cJSON_GetObjectItem(set, "scroll_inv") == NULL);
    cJSON_Delete(p);
}

/* After a restart the active profile is in effect before anyone reads. */
static void deck_again(void)
{
    CHECK(kvm_profiles_active() == 1);
    CHECK(kvm_setting_int("target_os") == 3);
    CHECK(kvm_setting_int("mouse_sens") == 250);
    CHECK(kvm_setting_int("jiggle_s") == 30);

    bool reboot = false;
    uint8_t id = create("{\"from\":\"switch\",\"name\":\"Switch\"}", &reboot);
    CHECK(id == 2);
    CHECK(reboot); /* usb_pad moved */
    CHECK(kvm_setting_int("usb_pad") == 2);
    /* Values the Switch profile does not hold come from Default. */
    CHECK(kvm_setting_int("target_os") == 0);
    CHECK(kvm_setting_int("mouse_mode") == 1);
}

static void delete_and_select(void)
{
    bool reboot;
    CHECK(kvm_profiles_delete(KVM_PROFILE_DEFAULT, &reboot) == ESP_ERR_NOT_ALLOWED);
    CHECK(kvm_profiles_delete(2, &reboot) == ESP_OK);
    CHECK(reboot);
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);
    CHECK(kvm_setting_int("usb_pad") == 0);
    CHECK(kvm_profiles_delete(2, &reboot) == ESP_ERR_NOT_FOUND);

    CHECK(kvm_profiles_select(1, true, &reboot) == ESP_OK);
    char err[96];
    CHECK(kvm_profiles_update("{\"id\":1,\"unset\":[\"target_os\"],\"set\":{\"ptr_engage\":\"hover\"}}",
                              &reboot, err, sizeof(err)) == ESP_OK);
    CHECK(kvm_setting_int("target_os") == 0);
    CHECK(kvm_setting_int("ptr_engage") == 1);

    /* Refused whole: one bad value leaves the profile as it was. */
    CHECK(kvm_profiles_update("{\"id\":1,\"set\":{\"ptr_engage\":0,\"jiggle_s\":5}}", &reboot, err,
                              sizeof(err)) == ESP_ERR_INVALID_ARG);
    CHECK(kvm_setting_int("ptr_engage") == 1);
    CHECK(kvm_profiles_update("{\"id\":1,\"set\":{\"mouse_mode\":\"sideways\"}}", &reboot, err,
                              sizeof(err)) == ESP_ERR_INVALID_ARG);
    CHECK(kvm_profiles_update("{\"id\":0,\"name\":\"x\"}", &reboot, err, sizeof(err)) ==
          ESP_ERR_NOT_ALLOWED);

    /* Default's own value is under the profile, not lost. */
    CHECK(kvm_profiles_select(KVM_PROFILE_DEFAULT, true, &reboot) == ESP_OK);
    CHECK(kvm_setting_int("mouse_sens") != 250);
    CHECK(kvm_setting_int("ptr_engage") == 0);
}

static void pick(void)
{
    bool reboot;
    uint8_t tv = create("{\"from\":\"androidtv\",\"select\":false}", &reboot);
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);

    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"STEAM DECK", "", "linux"}) == 1);
    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"", "e0319e", ""}) == 1);
    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"", "", "linux"}) == KVM_PROFILE_DEFAULT);
    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"", "", "android"}) == tv);
    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"SHIELD", "", "android"}) == tv);
    /* A stick seen over CEC only, and a Xiaomi that is not one. */
    uint8_t stick = create("{\"from\":\"mitvstick\",\"select\":false}", &reboot);
    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"Mi TV Stick", "00ec0a", ""}) == stick);
    CHECK(kvm_profiles_pick(&(kvm_prof_seen_t){"Mi Soundbar", "00ec0a", ""}) == KVM_PROFILE_DEFAULT);
    CHECK(kvm_profiles_delete(stick, &reboot) == ESP_OK);

    /* "current" from Default is empty; from a profile, a copy of its values. */
    CHECK(create("{\"name\":\"blank\",\"select\":false}", &reboot) != 0);
    CHECK(kvm_profiles_select(1, true, &reboot) == ESP_OK);
    uint8_t copy = create("{\"name\":\"copy\"}", &reboot);
    CHECK(kvm_profiles_active() == copy);
    CHECK(kvm_setting_int("mouse_sens") == 250);

    /* Default as it is under a profile: what an export writes. */
    char *def = kvm_settings_default_values_json();
    cJSON *dv = cJSON_Parse(def);
    free(def);
    CHECK(cJSON_GetObjectItem(dv, "mouse_sens")->valueint != 250);
    cJSON_Delete(dv);

    /* An import gives a new profile its values in one go. */
    uint8_t imp = create("{\"name\":\"imp\",\"from\":\"empty\",\"select\":false,"
                         "\"set\":{\"mouse_mode\":\"relative\",\"usb_pad\":3}}",
                         &reboot);
    cJSON *p = profiles();
    const cJSON *it;
    int found = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(p, "profiles"))
    {
        if (cJSON_GetObjectItem(it, "id")->valueint == imp) {
            const cJSON *set = cJSON_GetObjectItem(it, "set");
            found = cJSON_GetObjectItem(set, "mouse_mode")->valueint == 1 &&
                    cJSON_GetObjectItem(set, "usb_pad")->valueint == 3;
        }
    }
    CHECK(found);
    cJSON_Delete(p);
    CHECK(kvm_profiles_delete(imp, &reboot) == ESP_OK);
}

/* The device picks for itself, once per device, and a choice by hand holds. */
static void auto_pick(void)
{
    const kvm_prof_seen_t deck = {"Steam Deck", "e0319e", "linux"};
    const char *deck_fp = "Steam Deck|e0319e|linux";
    CHECK(kvm_profiles_manual());
    kvm_profiles_note_seen(&deck, deck_fp);
    CHECK(kvm_profiles_device_settled(&deck, deck_fp));
    CHECK(kvm_profiles_active() == 1 && !kvm_profiles_manual());
    CHECK(!kvm_profiles_device_settled(&deck, deck_fp));

    bool reboot;
    CHECK(kvm_profiles_select(3, true, &reboot) == ESP_OK); /* "blank" */
    CHECK(!kvm_profiles_device_settled(&deck, deck_fp));
    CHECK(kvm_profiles_active() == 3);

    const kvm_prof_seen_t tv = {"", "", "android"};
    kvm_profiles_note_seen(&tv, "||android");
    CHECK(kvm_profiles_device_settled(&tv, "||android"));
    CHECK(kvm_profiles_active() == 2); /* tv was made second, in pick() */

    cJSON *p = profiles();
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(p, "seen"), "usbOs")->valuestring, "android") == 0);
    cJSON_Delete(p);

    /* The same device after a restart: a profile made for it since is picked,
       a choice by hand is not undone. */
    const kvm_prof_seen_t stick = {"Mi TV Stick", "00ec0a", ""};
    const char *stick_fp = "Mi TV Stick|00ec0a|";
    kvm_profiles_note_seen(&stick, stick_fp);
    CHECK(kvm_profiles_device_settled(&stick, stick_fp));
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);
    uint8_t sid = 0;
    CHECK(kvm_profiles_create("{\"from\":\"mitvstick\",\"select\":false}", &sid, &reboot, NULL, 0) == ESP_OK);
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);
    CHECK(kvm_profiles_device_settled(&stick, stick_fp));
    CHECK(kvm_profiles_active() == sid && !kvm_profiles_manual());
    CHECK(!kvm_profiles_device_settled(&stick, stick_fp));
    CHECK(kvm_profiles_select(KVM_PROFILE_DEFAULT, true, &reboot) == ESP_OK);
    CHECK(!kvm_profiles_device_settled(&stick, stick_fp));
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);
    CHECK(kvm_profiles_delete(sid, &reboot) == ESP_OK);
    CHECK(kvm_profiles_select(2, false, &reboot) == ESP_OK); /* as reset() expects */

    CHECK(kvm_profiles_restart_allowed());
    CHECK(kvm_profiles_restart_allowed());
    CHECK(!kvm_profiles_restart_allowed());
    kvm_profiles_uptime_ok();
    CHECK(kvm_profiles_restart_allowed());

    /* Restarts for a Switch, then the stick put in its place: a person
       swapping sources, so the count starts again. The same Switch with
       another USB guess (what its own gamepad mode does) is not. */
    kvm_profiles_uptime_ok();
    const kvm_prof_seen_t sw = {"NintendoSwitch", "0009bf", "android"};
    kvm_profiles_note_seen(&sw, "NintendoSwitch|0009bf|android");
    (void)kvm_profiles_device_settled(&sw, "NintendoSwitch|0009bf|android");
    CHECK(kvm_profiles_restart_allowed());
    CHECK(kvm_profiles_restart_allowed());
    CHECK(!kvm_profiles_restart_allowed());
    const kvm_prof_seen_t sw2 = {"NintendoSwitch", "0009bf", "linux"};
    kvm_profiles_note_seen(&sw2, "NintendoSwitch|0009bf|linux");
    (void)kvm_profiles_device_settled(&sw2, "NintendoSwitch|0009bf|linux");
    CHECK(!kvm_profiles_restart_allowed());
    const kvm_prof_seen_t none = {"", "", "linux"};
    kvm_profiles_note_seen(&none, "||linux");
    (void)kvm_profiles_device_settled(&none, "||linux");
    CHECK(!kvm_profiles_restart_allowed());
    const kvm_prof_seen_t st = {"Mi TV Stick", "00ec0a", ""};
    kvm_profiles_note_seen(&st, "Mi TV Stick|00ec0a|");
    (void)kvm_profiles_device_settled(&st, "Mi TV Stick|00ec0a|");
    CHECK(kvm_profiles_restart_allowed());
    kvm_profiles_uptime_ok();
    bool rb;
    CHECK(kvm_profiles_select(2, false, &rb) == ESP_OK); /* as reset() expects */
}

static void reset(void)
{
    char id_before[KVM_INSTALL_ID_LEN + 1], id_after[KVM_INSTALL_ID_LEN + 1];
    kvm_install_id(id_before);
    CHECK(strlen(id_before) == KVM_INSTALL_ID_LEN);
    CHECK(kvm_profiles_active() != KVM_PROFILE_DEFAULT);
    CHECK(kvm_settings_reset() == ESP_OK);
    kvm_install_id(id_after);
    CHECK(strcmp(id_before, id_after) == 0); /* a reset device is not a new one */
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);
    CHECK(fake_nvs_keys("kvm_prof") <= 0);
    CHECK(kvm_setting_int("mouse_mode") == 0);
    cJSON *p = profiles();
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p, "profiles")) == 1);
    cJSON_Delete(p);
}

static void after_reset(void)
{
    CHECK(kvm_profiles_active() == KVM_PROFILE_DEFAULT);
    CHECK(kvm_setting_int("mouse_sens") != 250);
}

/* What "suggest" says in the list now, or "" for nothing. */
static const char *suggested(cJSON *p)
{
    const cJSON *s = cJSON_GetObjectItem(p, "suggest");
    return cJSON_IsString(s) ? s->valuestring : "";
}

static bool welcome_on(void)
{
    cJSON *p = profiles();
    const bool on = cJSON_IsTrue(cJSON_GetObjectItem(p, "welcome"));
    cJSON_Delete(p);
    return on;
}

/* The version the overview was last closed on, "" never. */
static bool tour_seen(const char *want)
{
    cJSON *p = profiles();
    const cJSON *v = cJSON_GetObjectItem(p, "tourSeen");
    const bool same = cJSON_IsString(v) && strcmp(v->valuestring, want) == 0;
    cJSON_Delete(p);
    return same;
}

/* The first-run question and the offer for an attached device. */
static void asks(void)
{
    CHECK(!welcome_on());
    kvm_profiles_welcome();
    CHECK(welcome_on());

    /* The overview is owed until the console says it was shown, on a device
       that never had the question as well. */
    char tour_err[96];
    CHECK(tour_seen(""));
    CHECK(kvm_profiles_dismiss("{\"tour\":\"0.76.0\"}", tour_err, sizeof(tour_err)) == ESP_OK);
    CHECK(tour_seen("0.76.0"));
    CHECK(kvm_profiles_dismiss("{\"tour\":true}", tour_err, sizeof(tour_err)) == ESP_ERR_INVALID_ARG);
    CHECK(kvm_profiles_dismiss("{\"tour\":\"0123456789012345678901234\"}", tour_err, sizeof(tour_err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(tour_seen("0.76.0"));
    CHECK(welcome_on());

    const kvm_prof_seen_t deck = {"Steam Deck", "e0319e", "linux"};
    kvm_profiles_note_seen(&deck, "Steam Deck|e0319e|linux");
    cJSON *p = profiles();
    CHECK(strcmp(suggested(p), "steamdeck") == 0);
    cJSON_Delete(p);

    /* The USB guess alone offers nothing: a phone says android too. */
    const kvm_prof_seen_t phone = {"", "", "android"};
    kvm_profiles_note_seen(&phone, "||android");
    p = profiles();
    CHECK(strcmp(suggested(p), "") == 0);
    cJSON_Delete(p);

    /* Said no to the Deck: not offered again. */
    char err[96];
    CHECK(kvm_profiles_dismiss("{\"preset\":\"steamdeck\"}", err, sizeof(err)) == ESP_OK);
    CHECK(kvm_profiles_dismiss("{\"preset\":\"nope\"}", err, sizeof(err)) == ESP_ERR_INVALID_ARG);
    kvm_profiles_note_seen(&deck, "Steam Deck|e0319e|linux");
    p = profiles();
    CHECK(strcmp(suggested(p), "") == 0);
    cJSON_Delete(p);

    /* A Switch is, until it has a profile of its own. */
    const kvm_prof_seen_t sw = {"NintendoSwitch", "0009bf", "linux"};
    kvm_profiles_note_seen(&sw, "NintendoSwitch|0009bf|linux");
    p = profiles();
    CHECK(strcmp(suggested(p), "switch") == 0);
    cJSON_Delete(p);
    bool reboot;
    (void)create("{\"from\":\"switch\",\"select\":false}", &reboot);
    p = profiles();
    CHECK(strcmp(suggested(p), "") == 0);
    cJSON_Delete(p);
}

/* The answers survive a restart; a reset forgets them. */
static void asks_after(void)
{
    CHECK(welcome_on());
    const kvm_prof_seen_t deck = {"Steam Deck", "e0319e", "linux"};
    kvm_profiles_note_seen(&deck, "Steam Deck|e0319e|linux");
    cJSON *p = profiles();
    CHECK(strcmp(suggested(p), "") == 0);
    cJSON_Delete(p);
    char err[96];
    CHECK(kvm_profiles_dismiss("{\"welcome\":true}", err, sizeof(err)) == ESP_OK);
    CHECK(!welcome_on());
    CHECK(tour_seen("0.76.0"));

    kvm_profiles_welcome();
    CHECK(kvm_settings_reset() == ESP_OK);
    CHECK(!welcome_on());
    CHECK(tour_seen(""));
    p = profiles();
    CHECK(strcmp(suggested(p), "steamdeck") == 0);
    cJSON_Delete(p);
}

/* A table saved before "New firmware" existed: seeded below, before boot. */
static void seed_old_table(void)
{
    nvs_handle_t h;
    CHECK(nvs_open("kvm", NVS_READWRITE, &h) == ESP_OK);
    (void)nvs_erase_key(h, "nev_release");
    CHECK(nvs_set_i32(h, "notify_tg_ev", 63) == ESP_OK);  /* everything that existed */
    CHECK(nvs_set_i32(h, "notify_push_ev", 0) == ESP_OK); /* every box cleared */
    (void)nvs_erase_key(h, "notify_hook_ev");               /* never saved */
    nvs_commit(h);
    nvs_close(h);
}

static void release_row(void)
{
    CHECK(kvm_setting_int("notify_tg_ev") == 127);  /* the new row joins a channel in use */
    CHECK(kvm_setting_int("notify_push_ev") == 0);  /* a cleared channel stays cleared */
    CHECK(kvm_setting_int("notify_hook_ev") & (1 << 6)); /* the default has it */
    int32_t v = 0;
    CHECK(stored_i32("kvm", "notify_tg_ev", &v) == 0 && v == 127);
    /* Once only: a box cleared after the migration stays cleared. */
    CHECK(kvm_setting_set_int("notify_tg_ev", 63) == ESP_OK);
}

static void release_row_again(void)
{
    CHECK(kvm_setting_int("notify_tg_ev") == 63);
}

int main(void)
{
    fake_nvs_clear();
    int bad = 0;
    bad += run(fresh);
    bad += run(deck);
    bad += run(deck_again);
    bad += run(delete_and_select);
    bad += run(pick);
    bad += run(auto_pick);
    bad += run(reset);
    bad += run(after_reset);
    bad += run(asks);
    bad += run(asks_after);
    seed_old_table();
    bad += run(release_row);
    bad += run(release_row_again);
    if (bad) {
        printf("profiles: %d phase(s) failed\n", bad);
        return 1;
    }
    printf("profiles: all passed\n");
    return 0;
}
