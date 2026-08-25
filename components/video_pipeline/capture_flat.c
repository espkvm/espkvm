/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Noticing that the screen has become one flat colour and stayed that way.
 *
 * Reading the screen as characters covers a BIOS, a boot menu, an installer, a
 * Linux console - anything drawn by a character generator. It cannot cover what
 * a modern Windows stop screen is: a graphical page in a proportional font at
 * whatever resolution the driver picked. There is no grid to cut it into, and
 * OCR of a proportional font is a different kind of thing entirely.
 *
 * But that screen has a shape a few hundred samples can see: nearly all of it is
 * one colour, and it does not change again. So does a machine that has blanked
 * its output, a frozen desktop that has painted itself black, and a graphical
 * session that died into a solid background. None of those can be read, and all
 * of them are worth knowing about, so this reports the shape rather than
 * guessing at the cause: the screen is one colour, and it has been for N
 * seconds. What that means is the operator's call.
 *
 * The cost is a few hundred reads of a frame that is already held and in cache -
 * the same moment the text reader uses - and no allocation at all.
 */
#include "capture_priv.h"
#include "capture_flat_scan.h"
#include "capture_pixfmt.h"

#include "esp_timer.h"

/* Below this the screen has merely been repainted; a stop screen stays. */
#define FLAT_SETTLE_MS 3000u

/*
 * When the screen went flat, in milliseconds since boot, or 0 for "it is not".
 *
 * Written by the capture task and read by whoever asks for the video status -
 * the web server, the MQTT timer - so it is deliberately 32 bits: a 64-bit
 * value is two loads on this core and a reader can catch one half of an update.
 * Milliseconds wrap after 49 days, which unsigned subtraction handles as long
 * as nothing tries to compare the two directly.
 */
static volatile uint32_t s_flat_since_ms;

static inline uint32_t now_ms(void)
{
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    /* 0 is the "not flat" sentinel, so the one millisecond that collides with
       it borrows the next one rather than reading as "no". */
    return ms ? ms : 1u;
}

void capture_flat_tick(capture_ctx_t *c, const void *frame)
{
    if (!capture_flat_is_flat(frame, (size_t)c->hres * c->vres,
                              (uint8_t)capture_pixfmt_bytes())) {
        s_flat_since_ms = 0;
        return;
    }
    if (s_flat_since_ms == 0) {
        s_flat_since_ms = now_ms();
    }
}

void capture_flat_forget(void)
{
    s_flat_since_ms = 0;
}

uint32_t capture_flat_ms(void)
{
    const uint32_t since = s_flat_since_ms;
    if (since == 0) {
        return 0;
    }
    const uint32_t held = now_ms() - since;
    /* A screen that has only just gone flat is a repaint, not a state. */
    return held < FLAT_SETTLE_MS ? 0 : held;
}
