/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reports in, subtitle lines out.
 */
#include <stdio.h>
#include <string.h>

#include "keylog.h"

static int failures;
static char cues[32][240];
static long long starts[32], ends[32];
static int n;

static void on_cue(void *user, int64_t s, int64_t e, const char *text)
{
    (void)user;
    if (n < 32) {
        snprintf(cues[n], sizeof(cues[n]), "%s", text);
        starts[n] = s;
        ends[n] = e;
    }
    n++;
}

static void expect(const char *name, const char *const *want, int count)
{
    int ok = n == count;
    for (int i = 0; ok && i < count; i++) {
        ok = strcmp(cues[i], want[i]) == 0;
    }
    if (!ok) {
        failures++;
        printf("FAIL %s: got %d cue(s):\n", name, n);
        for (int i = 0; i < n && i < 32; i++) {
            printf("    [%s]\n", cues[i]);
        }
        printf("  wanted:\n");
        for (int i = 0; i < count; i++) {
            printf("    [%s]\n", want[i]);
        }
    }
}

#define MS 1000LL
static keylog_t k;

static void tap(int64_t t, uint8_t mod, uint8_t key)
{
    uint8_t down[6] = {key}, up[6] = {0};
    keylog_keyboard(&k, t, mod, down);
    keylog_keyboard(&k, t + 30 * MS, 0, up);
}

static void type_us(int64_t t, const char *s)
{
    for (; *s; s++, t += 100 * MS) {
        char c = *s;
        if (c >= 'a' && c <= 'z') {
            tap(t, 0, (uint8_t)(0x04 + c - 'a'));
        } else if (c >= 'A' && c <= 'Z') {
            tap(t, 0x02, (uint8_t)(0x04 + c - 'A'));
        } else if (c == ' ') {
            tap(t, 0, 0x2c);
        }
    }
}

int main(void)
{
    /* Full: a word, then Enter closes the line. */
    n = 0;
    keylog_init(&k, KEYLOG_FULL, true, "en_us", on_cue, NULL);
    type_us(0, "root");
    tap(400 * MS, 0, 0x28);
    keylog_flush(&k);
    expect("typed word", (const char *const[]){"Typed: root\xe2\x86\xb5"}, 1);
    if (n == 1 && (starts[0] != 0 || ends[0] != 400 * MS + 1500 * MS)) {
        failures++;
        printf("FAIL typed word timing: %lld..%lld\n", starts[0], ends[0]);
    }

    /* Masked: the same word hides its letters, keeps its length. */
    n = 0;
    keylog_init(&k, KEYLOG_MASKED, true, "en_us", on_cue, NULL);
    type_us(0, "root");
    tap(400 * MS, 0, 0x28);
    keylog_flush(&k);
    expect("masked word", (const char *const[]){"Typed: \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x86\xb5"}, 1);

    /* Russian layout: the keys that type "ghbdtn" on US type "привет". */
    n = 0;
    keylog_init(&k, KEYLOG_FULL, true, "ru_ru", on_cue, NULL);
    type_us(0, "ghbdtn");
    keylog_flush(&k);
    expect("russian", (const char *const[]){"Typed: \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"}, 1);

    /* German: ´ then e is é, ´ then z (the Y key) is the accent and z, and
     * ^ then space is ^ alone. */
    n = 0;
    keylog_init(&k, KEYLOG_FULL, true, "de_de", on_cue, NULL);
    tap(0, 0, 0x2e);                 /* dead ´ */
    tap(100 * MS, 0, 0x08);          /* e */
    tap(200 * MS, 0, 0x2e);          /* dead ´ */
    tap(300 * MS, 0, 0x1c);          /* z */
    tap(400 * MS, 0, 0x35);          /* dead ^ */
    tap(500 * MS, 0, 0x2c);          /* space */
    keylog_flush(&k);
    expect("german dead keys", (const char *const[]){"Typed: \xc3\xa9\xc2\xb4z^"}, 1);

    /* Shortcuts are named, even in masked mode; repeats are counted. */
    n = 0;
    keylog_init(&k, KEYLOG_MASKED, true, "en_us", on_cue, NULL);
    tap(0, 0x05, 0x4c);              /* Ctrl+Alt+Del */
    tap(3000 * MS, 0, 0x3b);         /* F2 */
    for (int i = 0; i < 5; i++) {
        tap(6000 * MS + i * 150 * MS, 0, 0x51); /* Down x5 */
    }
    tap(9000 * MS, 0x02, 0x2b);      /* Shift+Tab */
    tap(12000 * MS, 0x01, 0x06);     /* Ctrl+C */
    keylog_flush(&k);
    expect("shortcuts", (const char *const[]){"Ctrl+Alt+Delete", "F2", "Down x5", "Shift+Tab", "Ctrl+C"}, 5);

    /* A text run is broken by a gap, and Backspace stays inside the run. */
    n = 0;
    keylog_init(&k, KEYLOG_FULL, true, "en_us", on_cue, NULL);
    type_us(0, "ab");
    tap(250 * MS, 0, 0x2a);
    type_us(5000 * MS, "c");
    keylog_tick(&k, 9000 * MS);
    expect("gap and backspace", (const char *const[]){"Typed: ab\xe2\x86\x90", "Typed: c"}, 2);

    /* Win pressed and released alone; Shift alone says nothing. */
    n = 0;
    keylog_init(&k, KEYLOG_MASKED, true, "en_us", on_cue, NULL);
    uint8_t none[6] = {0};
    keylog_keyboard(&k, 0, 0x08, none);
    keylog_keyboard(&k, 50 * MS, 0, none);
    keylog_keyboard(&k, 3000 * MS, 0x02, none);
    keylog_keyboard(&k, 3050 * MS, 0, none);
    keylog_flush(&k);
    expect("modifier alone", (const char *const[]){"Win"}, 1);

    /* Held key: one press, not one per report. */
    n = 0;
    keylog_init(&k, KEYLOG_FULL, true, "en_us", on_cue, NULL);
    uint8_t held[6] = {0x04};
    for (int i = 0; i < 10; i++) {
        keylog_keyboard(&k, i * 10 * MS, 0, held);
    }
    keylog_keyboard(&k, 200 * MS, 0, none);
    keylog_flush(&k);
    expect("held key", (const char *const[]){"Typed: a"}, 1);

    /* Clicks, with and without a position, and switched off. */
    n = 0;
    keylog_init(&k, KEYLOG_MASKED, true, "en_us", on_cue, NULL);
    keylog_mouse(&k, 0, 0x01, 812, 440);
    keylog_mouse(&k, 50 * MS, 0x00, 812, 440);
    keylog_mouse(&k, 3000 * MS, 0x02, -1, -1);
    keylog_mouse(&k, 3050 * MS, 0x00, -1, -1);
    keylog_consumer(&k, 6000 * MS, 0xe9);
    keylog_consumer(&k, 6050 * MS, 0);
    keylog_flush(&k);
    expect("clicks", (const char *const[]){"Left click (812, 440)", "Right click", "Volume up"}, 3);
    n = 0;
    keylog_init(&k, KEYLOG_MASKED, false, "en_us", on_cue, NULL);
    keylog_mouse(&k, 0, 0x01, 1, 1);
    keylog_flush(&k);
    expect("clicks off", NULL, 0);

    /* Off records nothing. */
    n = 0;
    keylog_init(&k, KEYLOG_OFF, true, "en_us", on_cue, NULL);
    type_us(0, "secret");
    keylog_flush(&k);
    expect("off", NULL, 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("keylog: typing, masking, layouts, shortcuts, clicks - all as expected\n");
    return 0;
}
