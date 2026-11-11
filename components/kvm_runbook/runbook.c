/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The runbook engine: one task, made for the run and gone at its end.
 *
 * Keys go straight to the HID queue, the way the console's own keystrokes do,
 * and the waits poll the screen-text store. While a wait stands, the task keeps
 * asking for a reading so a wide (1080p) console is scanned at all - the
 * capture side reads those only on request.
 */
#include "runbook.h"
#include "kvm_regions.h"

#include "kvm_record.h"
#include "kvm_cec.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_caps.h"
#include "kvm_settings.h"
#include "screentext.h"
#include "screentext_store.h"
#include "usb_hid.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "runbook";

#define TASK_STACK 6144
#define TASK_PRIO 5
/* A chord is held this long and released for as long, the same as the console. */
#define CHORD_MS 40
/* How often a wait looks at the screen. The reader itself runs at 250 ms when
   asked, so anything quicker only burns the poll. */
#define POLL_MS 200
#define DEFAULT_TIMEOUT_S 60

static SemaphoreHandle_t s_lock;
static runbook_status_t s_status;
static volatile bool s_stop;
static volatile bool s_running;
static int64_t s_started_us;
/* Both in PSRAM for the life of a run: the script is ~11 KB, the grid ~10 KB. */
static rb_script_t *s_script;
static screentext_grid_t *s_grid;

static void status_set(runbook_state_t state, const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    if (message) {
        strlcpy(s_status.message, message, sizeof(s_status.message));
    }
    xSemaphoreGive(s_lock);
}

static void status_step(uint16_t step, const rb_step_t *st)
{
    char line[RUNBOOK_LINE_MAX];
    snprintf(line, sizeof(line), "%s %s", rb_kind_name(st->kind),
             (st->kind == RB_DELAY || st->kind == RB_TIMEOUT) ? "" : st->arg);
    if (st->kind == RB_DELAY || st->kind == RB_TIMEOUT || (st->kind == RB_RECORD && st->value)) {
        snprintf(line, sizeof(line), "%s %u", rb_kind_name(st->kind), (unsigned)st->value);
    } else if (st->kind == RB_TIMELAPSE) {
        snprintf(line, sizeof(line), st->value ? "timelapse %u %u" : "timelapse %u", (unsigned)st->every,
                 (unsigned)st->value);
    } else if (st->kind == RB_RECORD || st->kind == RB_RECORD_STOP || st->kind == RB_SCREENSHOT) {
        snprintf(line, sizeof(line), "%s", rb_kind_name(st->kind));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.step = step;
    strlcpy(s_status.line, line, sizeof(s_status.line));
    xSemaphoreGive(s_lock);
}

/* Sleep in slices so a stop is felt within a slice. False when stopped. */
static bool sleep_ms(uint32_t ms)
{
    while (ms) {
        if (s_stop) {
            return false;
        }
        const uint32_t slice = ms < 100 ? ms : 100;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    return true;
}

static bool press(uint8_t mod, uint8_t code)
{
    const uint8_t down[6] = {code, 0, 0, 0, 0, 0};
    const uint8_t up[6] = {0};
    usb_hid_keyboard(mod, down);
    if (!sleep_ms(CHORD_MS)) {
        return false;
    }
    usb_hid_keyboard(0, up);
    return sleep_ms(CHORD_MS);
}

static bool type_text(const char *text)
{
    /* The HID worker paces the reports; this pacing is what keeps a long line
       inside its queue, and it is the paste setting so the two agree. */
    int gap = kvm_setting_int("type_delay");
    if (gap < 1) {
        gap = 1;
    }
    const uint8_t up[6] = {0};
    for (const char *p = text; *p; p++) {
        uint8_t usage;
        bool shift;
        if (!rb_ascii_usage(*p, &usage, &shift)) {
            continue; /* the parser refused these already */
        }
        const uint8_t down[6] = {usage, 0, 0, 0, 0, 0};
        usb_hid_keyboard(shift ? 0x02 : 0, down);
        usb_hid_keyboard(0, up);
        if (!sleep_ms((uint32_t)gap)) {
            return false;
        }
    }
    return true;
}

/* 0 = satisfied, -1 = timed out (msg says so), -2 = stopped. */
/* "wait area Name" / "gone area Name": a screen area instead of a phrase. A
   moment (a change) is waited for afresh; a state just has to hold. */
static int wait_area(const rb_step_t *st, const char *name, uint32_t timeout_s, char *msg, size_t cap)
{
    const bool want = st->kind == RB_WAIT;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    kvm_region_status_t a;
    if (!kvm_regions_find(name, &a)) {
        snprintf(msg, cap, "line %u: there is no screen area called \"%s\"", (unsigned)st->line, name);
        return -1;
    }
    const uint32_t from = a.events;
    kvm_regions_waiting(true); /* looks go on with the console closed */
    int r;
    for (;;) {
        if (s_stop) {
            r = -2;
            break;
        }
        if (!kvm_regions_find(name, &a)) {
            snprintf(msg, cap, "line %u: the screen area \"%s\" was removed", (unsigned)st->line, name);
            r = -1;
            break;
        }
        const bool met = want ? (kvm_regions_is_moment(&a) ? a.events != from : a.active) : !a.active;
        if (met) {
            r = 0;
            break;
        }
        if (esp_timer_get_time() > deadline) {
            snprintf(msg, cap, "line %u: area \"%s\" %s after %u s", (unsigned)st->line, name,
                     want ? "did not fire" : "is still on", (unsigned)timeout_s);
            r = -1;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    kvm_regions_waiting(false);
    return r;
}

static int wait_phrase(const rb_step_t *st, uint32_t timeout_s, char *msg, size_t cap)
{
    if (strncasecmp(st->arg, "area ", 5) == 0 && st->arg[5]) {
        return wait_area(st, st->arg + 5, timeout_s, msg, cap);
    }
    const bool want = st->kind == RB_WAIT;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    uint32_t seen = UINT32_MAX;
    bool has = false;
    for (;;) {
        if (s_stop) {
            return -2;
        }
        screentext_request();
        const uint32_t seq = screentext_seq();
        if (seq != seen) {
            seen = seq;
            /* No reading at all - a picture, or nothing yet - counts as "not on
               screen": a phrase cannot be there, and one that was is gone. */
            has = screentext_latest(s_grid, NULL) && screentext_has(s_grid, st->arg);
        }
        if (has == want) {
            return 0;
        }
        if (esp_timer_get_time() > deadline) {
            snprintf(msg, cap, "line %u: \"%s\" %s after %u s", (unsigned)st->line, st->arg,
                     want ? "did not appear" : "is still on the screen", (unsigned)timeout_s);
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static void run_task(void *arg)
{
    (void)arg;
    const rb_script_t *s = s_script;
    uint32_t timeout_s = DEFAULT_TIMEOUT_S;
    runbook_state_t end = RUNBOOK_DONE;
    char msg[RB_ERR_MAX] = "done";
    char name[RUNBOOK_NAME_MAX];
    strlcpy(name, s_status.name, sizeof(name));

    for (uint16_t i = 0; i < s->count; i++) {
        const rb_step_t *st = &s->steps[i];
        status_step((uint16_t)(i + 1), st);
        ESP_LOGI(TAG, "%s: step %u/%u: %s", name, (unsigned)(i + 1), (unsigned)s->count,
                 s_status.line);
        bool ok = true;
        switch (st->kind) {
        case RB_KEY:
            ok = press(st->mod, st->code);
            break;
        case RB_TYPE:
            ok = type_text(st->arg);
            break;
        case RB_DELAY:
            ok = sleep_ms(st->value);
            break;
        case RB_TIMEOUT:
            timeout_s = st->value;
            break;
        case RB_RECORD: {
            char why[96];
            if (kvm_record_start(st->value, why, sizeof(why)) != ESP_OK) {
                /* A runbook meant to leave a recording behind should say it could not. */
                snprintf(msg, sizeof(msg), "line %u: no recording: %.64s", (unsigned)st->line, why);
                end = RUNBOOK_FAILED;
                ok = false;
            }
            break;
        }
        case RB_TIMELAPSE: {
            char why[96];
            if (kvm_record_start_timelapse(st->every, st->value, why, sizeof(why)) != ESP_OK) {
                snprintf(msg, sizeof(msg), "line %u: no timelapse: %.64s", (unsigned)st->line, why);
                end = RUNBOOK_FAILED;
                ok = false;
            }
            break;
        }
        case RB_RECORD_STOP:
            kvm_record_stop("stopped by a runbook");
            break;
        case RB_HDMI: {
            esp_err_t err;
            uint8_t code = 0;
            if (strcmp(st->arg, "standby") == 0) {
                err = kvm_cec_standby(-1);
            } else if (strcmp(st->arg, "wake") == 0) {
                err = kvm_cec_wake(-1);
            } else if (kvm_cec_key_from_name(st->arg + 4, &code)) {
                err = kvm_cec_key(-1, code);
            } else {
                snprintf(msg, sizeof(msg), "line %u: no such remote key \"%.32s\"", (unsigned)st->line,
                         st->arg + 4);
                end = RUNBOOK_FAILED;
                ok = false;
                break;
            }
            if (err != ESP_OK) {
                snprintf(msg, sizeof(msg), "line %u: HDMI-CEC: %s", (unsigned)st->line,
                         err == ESP_ERR_NOT_FOUND ? "no device on the line" : "CEC is off or busy");
                end = RUNBOOK_FAILED;
                ok = false;
            }
            break;
        }
        case RB_SCREENSHOT: {
            char file[64], why[96];
            if (kvm_record_screenshot(file, sizeof(file), why, sizeof(why)) != ESP_OK) {
                snprintf(msg, sizeof(msg), "line %u: no screenshot: %.64s", (unsigned)st->line, why);
                end = RUNBOOK_FAILED;
                ok = false;
            }
            break;
        }
        case RB_WAIT:
        case RB_GONE: {
            const int r = wait_phrase(st, timeout_s, msg, sizeof(msg));
            if (r == -1) {
                end = RUNBOOK_FAILED;
            }
            ok = r == 0;
            break;
        }
        }
        if (!ok) {
            if (end != RUNBOOK_FAILED) {
                end = RUNBOOK_STOPPED;
                snprintf(msg, sizeof(msg), "stopped at line %u", (unsigned)st->line);
            }
            break;
        }
    }
    /* Nothing may stay held on the target, whichever way the run ended. */
    usb_hid_release_all();

    const uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.elapsed_ms = elapsed;
    xSemaphoreGive(s_lock);
    status_set(end, msg);
    if (end == RUNBOOK_DONE) {
        ESP_LOGI(TAG, "%s: done in %u ms", name, (unsigned)elapsed);
    } else {
        ESP_LOGW(TAG, "%s: %s", name, msg);
    }

    free(s_grid);
    s_grid = NULL;
    free(s_script);
    s_script = NULL;
    s_running = false;
    vTaskDelete(NULL);
}

/* Find @p name in the setting and hand back its script, malloc'd. */
static char *load_script(const char *name)
{
    cJSON *list = cJSON_Parse(kvm_setting_str("runbooks_json"));
    char *script = NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *jn = cJSON_GetObjectItem(item, "name");
        const cJSON *js = cJSON_GetObjectItem(item, "script");
        if (cJSON_IsString(jn) && cJSON_IsString(js) && strcmp(jn->valuestring, name) == 0) {
            script = strdup(js->valuestring);
            break;
        }
    }
    cJSON_Delete(list);
    return script;
}

void runbook_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_status.state = RUNBOOK_IDLE;
    strlcpy(s_status.message, "nothing has run", sizeof(s_status.message));
    kvm_cap_report(KVM_CAP_RUNBOOK, true, NULL);
}

esp_err_t runbook_start(const char *name, const char *source, char *err, size_t err_cap)
{
    if (err && err_cap) {
        err[0] = '\0';
    }
    if (!name || !*name || strlen(name) >= RUNBOOK_NAME_MAX) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Claimed under the lock: four callers (web, MQTT, schedule, button) can
     * start one at the same moment, and both would pass a bare check. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_running;
    s_running = true;
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!usb_hid_ready()) {
        s_running = false;
        return ESP_ERR_NOT_SUPPORTED;
    }
    char *src = load_script(name);
    if (!src) {
        s_running = false;
        return ESP_ERR_NOT_FOUND;
    }
    rb_script_t *script = heap_caps_calloc(1, sizeof(*script), MALLOC_CAP_SPIRAM);
    screentext_grid_t *grid = heap_caps_calloc(1, sizeof(*grid), MALLOC_CAP_SPIRAM);
    if (!script || !grid) {
        free(src);
        free(script);
        free(grid);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    char why[RB_ERR_MAX];
    const bool parsed = rb_parse(src, script, why, sizeof(why));
    free(src);
    if (!parsed) {
        ESP_LOGW(TAG, "%s: will not run: %s", name, why);
        if (err && err_cap) {
            strlcpy(err, why, err_cap);
        }
        free(script);
        free(grid);
        s_running = false;
        return ESP_ERR_INVALID_ARG;
    }

    s_script = script;
    s_grid = grid;
    s_stop = false;
    s_started_us = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = RUNBOOK_RUNNING;
    strlcpy(s_status.name, name, sizeof(s_status.name));
    s_status.step = 0;
    s_status.steps = script->count;
    s_status.line[0] = '\0';
    strlcpy(s_status.message, "running", sizeof(s_status.message));
    s_status.elapsed_ms = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s: started by %s, %u steps", name, source ? source : "?",
             (unsigned)script->count);

    if (xTaskCreate(run_task, "runbook", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        s_running = false;
        s_script = NULL;
        s_grid = NULL;
        free(script);
        free(grid);
        status_set(RUNBOOK_FAILED, "could not start the runbook task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t runbook_stop(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop = true;
    return ESP_OK;
}

bool runbook_busy(void)
{
    return s_running;
}

uint32_t runbook_seq(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t seq = ((uint32_t)s_status.state << 16) | s_status.step;
    xSemaphoreGive(s_lock);
    return seq;
}

void runbook_get_status(runbook_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (s_running) {
        out->elapsed_ms = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000);
    }
    xSemaphoreGive(s_lock);
}
