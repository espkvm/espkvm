/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/*
 * 12 KB, which is roughly a hundred lines - a whole start-up sequence and a good
 * deal of what came after it.
 *
 * The size is fixed at build time and the ring never grows: the oldest bytes are
 * overwritten by the newest, so this can neither fill up nor be rotated. That it
 * comes out of RTC memory rather than the main heap is the point. Internal RAM
 * on this chip is about half a megabyte, shared by TLS, USB, the network stack,
 * the SD card and the video encoder - taking a log buffer from there would be
 * taking it from the encoder, which we have already watched fail. The RTC pool
 * is 31 KB and otherwise almost unused here.
 */
#define RING_BYTES 12288
/* One formatted line, on the caller's stack - so it must stay small: the
 * system event task runs on 2.3 KB. (Not LINE_MAX: limits.h has that.) */
#define LOG_LINE_MAX 160
#define RING_MAGIC 0x4C4F4731 /* "LOG1" */

/*
 * NOT initialised at start-up, which is the whole trick: a software restart -
 * including the one the bootloader performs when it rolls an update back - runs
 * straight past this and leaves the previous run's log in place.
 */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t head;    /* where the next byte goes */
    uint32_t filled;  /* bytes held, up to RING_BYTES */
    char buf[RING_BYTES];
} s_ring;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_chain;
static bool s_ready;

static void ring_write(const char *text, size_t len)
{
    if (!len) {
        return;
    }
    if (len > RING_BYTES) { /* keep the tail of an over-long line */
        text += len - RING_BYTES;
        len = RING_BYTES;
    }
    taskENTER_CRITICAL(&s_mux);
    const size_t head = s_ring.head;
    const size_t first = (len < RING_BYTES - head) ? len : (RING_BYTES - head);
    memcpy(s_ring.buf + head, text, first);
    if (len > first) {
        memcpy(s_ring.buf, text + first, len - first);
    }
    s_ring.head = (uint32_t)((head + len) % RING_BYTES);
    s_ring.filled = (uint32_t)((s_ring.filled + len < RING_BYTES) ? s_ring.filled + len
                                                                 : RING_BYTES);
    taskEXIT_CRITICAL(&s_mux);
}

/*
 * Runs wherever a log line is written - any task, and in principle an interrupt.
 * So: format on the caller's stack, copy, return. No allocation, no mutex, and
 * nothing that touches flash or PSRAM, both of which can be unavailable at the
 * moment the line is written (the cache is off while flash is being written,
 * which is precisely when an update is in progress and the log matters most).
 */
static int log_hook(const char *fmt, va_list ap)
{
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, ap);
    const int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        ring_write(line, (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1);
    }
    /* The wire still gets everything, unchanged: a serial console remains the
     * better tool when someone has one. */
    return s_chain ? s_chain(fmt, ap) : 0;
}

void kvm_log_init(void)
{
    if (s_ready) {
        return;
    }
    /* A cold start leaves this full of whatever the RAM held. Anything but an
     * intact header, or a head that points outside the buffer, means there is no
     * previous log to keep. */
    if (s_ring.magic != RING_MAGIC || s_ring.head >= RING_BYTES || s_ring.filled > RING_BYTES) {
        memset(&s_ring, 0, sizeof(s_ring));
        s_ring.magic = RING_MAGIC;
    }
    s_ready = true;
    s_chain = esp_log_set_vprintf(log_hook);

    /* One continuous stream across restarts needs a mark where each run starts,
     * or reading it back is guesswork. */
    const esp_app_desc_t *app = esp_app_get_description();
    char mark[96];
    const int n = snprintf(mark, sizeof(mark), "\n--- boot: %s ---\n", app ? app->version : "?");
    if (n > 0) {
        ring_write(mark, (size_t)n < sizeof(mark) ? (size_t)n : sizeof(mark) - 1);
    }
}

size_t kvm_log_read(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    /*
     * Only the two indices are read with interrupts off. Copying 12 KB inside a
     * critical section would hold them off for hundreds of microseconds, and on
     * this device that is dropped camera frames and stuttering USB - a log
     * download is not worth a glitch in the picture.
     *
     * The cost of letting go is that a line written during the copy can come out
     * torn. For a diagnostic dump that is a fair trade; for anything else it
     * would not be.
     */
    taskENTER_CRITICAL(&s_mux);
    const size_t filled = s_ring.filled;
    const size_t head = s_ring.head;
    taskEXIT_CRITICAL(&s_mux);

    /* Oldest first; when the ring has wrapped that is head, not zero. */
    size_t start = (filled < RING_BYTES) ? 0 : head;
    size_t want = filled;
    if (want > cap - 1) { /* keep the newest end - the part that says what broke */
        start = (start + (want - (cap - 1))) % RING_BYTES;
        want = cap - 1;
    }
    const size_t first = (want < RING_BYTES - start) ? want : (RING_BYTES - start);
    memcpy(out, s_ring.buf + start, first);
    if (want > first) {
        memcpy(out + first, s_ring.buf, want - first);
    }
    out[want] = '\0';
    return want;
}

size_t kvm_log_size(void)
{
    return s_ring.filled;
}

size_t kvm_log_capacity(void)
{
    return RING_BYTES;
}
