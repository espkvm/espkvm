/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The mouse jiggler.
 *
 * A machine left alone locks its screen or goes to sleep, and then the thing you
 * were watching is behind a password - or the KVM's picture goes black and the
 * fan you were listening to stops. A pointer that twitches now and then keeps
 * the target awake, which is the oldest trick in this business and still the one
 * people ask for.
 *
 * It lives on the device, not in the console. The point of it is a machine
 * nobody is sitting in front of, so a jiggler that dies with the browser tab
 * would miss the case it exists for.
 *
 * Two things it must not do. It must not move anything: one pixel out and one
 * pixel back leaves the cursor where it was, and relative motion is used rather
 * than absolute so it cannot fight the console's own pointer mapping. And it
 * must not fight the operator - a nudge in the middle of someone dragging a
 * window is worse than a locked screen.
 */
#include "usb_hid.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "kvm_settings.h"

static const char *TAG = "jiggler";

/*
 * The clock ticks every second whatever the interval is, and the interval is
 * read on each tick. So changing the setting takes effect immediately, with no
 * timer to tear down and rebuild - and a tick that decides to do nothing is a
 * comparison and a return.
 */
#define TICK_US (1000 * 1000)

/*
 * The last input timestamp this has already accounted for.
 *
 * Our own nudge lands in the same place as the operator's mouse, so the clock
 * alone cannot tell them apart - what separates them is that we know the value
 * we left. Anything later is somebody real.
 */
static int64_t s_seen;
static int64_t s_next_us;
static uint32_t s_nudges;

uint32_t usb_hid_jiggler_nudges(void)
{
    return s_nudges;
}

static void tick(void *arg)
{
    (void)arg;

    const int32_t every_s = kvm_setting_int("jiggle_s");
    if (every_s <= 0) {
        s_next_us = 0; /* switched off: start the wait afresh when it comes back */
        return;
    }
    if (!usb_hid_ready()) {
        return; /* nothing attached to nudge */
    }

    const int64_t now = esp_timer_get_time();
    const int64_t every_us = (int64_t)every_s * 1000000;

    /*
     * Stand aside while the operator is using it.
     *
     * Our own nudge lands in the same timestamp, so it cannot be told from real
     * input by the clock alone - hence remembering the value we left. Anything
     * later than that is somebody real, and the wait starts again from them:
     * a target being used is awake already, which is the whole point.
     */
    const int64_t last = usb_hid_last_input_us();
    if (last != s_seen) {
        /*
         * Somebody real used it, so the wait starts again from them - a target
         * being used is awake already. Remember the value, or every later tick
         * would read the same input as new and the nudge would never come:
         * clearing this instead of recording it is exactly that bug, and it
         * left the jiggler silent for good after the operator's first move.
         */
        s_seen = last;
        s_next_us = last + every_us;
        return;
    }
    if (s_next_us == 0) {
        s_next_us = now + every_us;
        return;
    }
    if (now < s_next_us) {
        return;
    }

    /*
     * Not at a sleeping target. Input now wakes one, and a jiggler is for
     * keeping a machine awake through a session, not for overruling somebody
     * who put it to sleep on purpose. Wait for it to come back instead.
     */
    if (usb_hid_target_suspended()) {
        s_next_us = now + every_us;
        return;
    }

    usb_hid_mouse_rel(0, 1, 0, 0, 0);
    usb_hid_mouse_rel(0, -1, 0, 0, 0);
    s_seen = usb_hid_last_input_us();
    s_next_us = now + every_us;
    s_nudges++;
}

void usb_hid_jiggler_start(void)
{
    static esp_timer_handle_t s_timer;
    if (s_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "jiggler",
        /* The work is two queue pushes a minute at most, so it can run on the
           timer task rather than earning a task of its own. */
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&args, &s_timer) != ESP_OK ||
        esp_timer_start_periodic(s_timer, TICK_US) != ESP_OK) {
        ESP_LOGW(TAG, "could not start the jiggler timer");
        s_timer = NULL;
        return;
    }
    ESP_LOGI(TAG, "ready (jiggle_s=%d)", (int)kvm_setting_int("jiggle_s"));
}
