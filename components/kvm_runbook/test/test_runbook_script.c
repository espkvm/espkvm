/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 * The parser's tests: every verb, every refusal, and the US typing table
 * against the one the console's paste path uses.
 */
#include "runbook_script.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);               \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

static rb_script_t s;
static char err[RB_ERR_MAX];

int main(void)
{
    /* Every verb, comments, blank lines, odd spacing and case. */
    CHECK(rb_parse("# enter the setup\n"
                   "  KEY  Ctrl + Alt + Del \r\n"
                   "\n"
                   "timeout 90\n"
                   "wait Press F2\n"
                   "key f2\n"
                   "gone Loading\n"
                   "type root\n"
                   "delay 250\n",
                   &s, err, sizeof(err)));
    CHECK(s.count == 7);
    CHECK(s.steps[0].kind == RB_KEY && s.steps[0].mod == 0x05 && s.steps[0].code == 0x4c);
    CHECK(s.steps[0].line == 2);
    CHECK(strcmp(s.steps[0].arg, "Ctrl + Alt + Del") == 0);
    CHECK(s.steps[1].kind == RB_TIMEOUT && s.steps[1].value == 90);
    CHECK(s.steps[2].kind == RB_WAIT && strcmp(s.steps[2].arg, "Press F2") == 0);
    CHECK(s.steps[3].kind == RB_KEY && s.steps[3].mod == 0 && s.steps[3].code == 0x3b);
    CHECK(s.steps[4].kind == RB_GONE && strcmp(s.steps[4].arg, "Loading") == 0);
    CHECK(s.steps[5].kind == RB_TYPE && strcmp(s.steps[5].arg, "root") == 0);
    CHECK(s.steps[6].kind == RB_DELAY && s.steps[6].value == 250 && s.steps[6].line == 9);

    /* Key names by rule: letters, digits, function keys, and the table. */
    CHECK(rb_parse("key a\nkey z\nkey 0\nkey 9\nkey f1\nkey f12\nkey shift+tab\nkey win+r", &s,
                   err, sizeof(err)));
    CHECK(s.steps[0].code == 0x04 && s.steps[1].code == 0x1d);
    CHECK(s.steps[2].code == 0x27 && s.steps[3].code == 0x26);
    CHECK(s.steps[4].code == 0x3a && s.steps[5].code == 0x45);
    CHECK(s.steps[6].mod == 0x02 && s.steps[6].code == 0x2b);
    CHECK(s.steps[7].mod == 0x08 && s.steps[7].code == 0x15);

    /* Refusals, each with the line it is on. */
    CHECK(!rb_parse("key f13", &s, err, sizeof(err)) && strcmp(err, "line 1: unknown key \"f13\"") == 0);
    /* A lone modifier is a valid keypress (GUI opens Start). */
    CHECK(rb_parse("key gui", &s, err, sizeof(err)) && s.steps[0].mod == 0x08 && s.steps[0].code == 0);
    CHECK(rb_parse("key ctrl+shift", &s, err, sizeof(err)) && s.steps[0].mod == 0x03 && s.steps[0].code == 0);
    CHECK(!rb_parse("key a+b", &s, err, sizeof(err)) && strstr(err, "more than one") != NULL);
    CHECK(!rb_parse("type", &s, err, sizeof(err)) && strcmp(err, "line 1: nothing to type") == 0);
    CHECK(!rb_parse("type caf\xc3\xa9", &s, err, sizeof(err)) && strstr(err, "cannot type") != NULL);
    CHECK(!rb_parse("delay 0", &s, err, sizeof(err)) && strstr(err, "delay wants") != NULL);
    CHECK(!rb_parse("delay 60001", &s, err, sizeof(err)) && strstr(err, "delay wants") != NULL);
    CHECK(!rb_parse("delay soon", &s, err, sizeof(err)) && strstr(err, "delay wants") != NULL);
    CHECK(!rb_parse("timeout 0", &s, err, sizeof(err)) && strstr(err, "timeout wants") != NULL);
    CHECK(!rb_parse("wait", &s, err, sizeof(err)) && strcmp(err, "line 1: which phrase?") == 0);
    CHECK(!rb_parse("\n\nfrobnicate now", &s, err, sizeof(err)) &&
          strcmp(err, "line 3: unknown command \"frobnicate\"") == 0);
    CHECK(!rb_parse("", &s, err, sizeof(err)) && strstr(err, "no steps") != NULL);
    CHECK(!rb_parse("# only a comment\n", &s, err, sizeof(err)) && strstr(err, "no steps") != NULL);
    char long_phrase[80] = "wait ";
    memset(long_phrase + 5, 'x', 64);
    long_phrase[69] = '\0';
    CHECK(!rb_parse(long_phrase, &s, err, sizeof(err)) && strstr(err, "at most 63") != NULL);
    CHECK(!rb_parse("wait ok\xc3\xa9", &s, err, sizeof(err)) && strstr(err, "ASCII only") != NULL);
    /* A screen area may have any name. */
    CHECK(rb_parse("wait area \xd0\xa1\xd0\xb1\xd0\xbe\xd1\x80\xd0\xba\xd0\xb0\ngone area Build", &s, err, sizeof(err)));
    CHECK(s.count == 2 && s.steps[0].kind == RB_WAIT && strncmp(s.steps[0].arg, "area ", 5) == 0);
    CHECK(s.steps[1].kind == RB_GONE && strcmp(s.steps[1].arg, "area Build") == 0);

    /* The step cap. */
    char many[RB_MAX_STEPS * 8 + 16];
    many[0] = '\0';
    for (int i = 0; i < RB_MAX_STEPS + 1; i++) {
        strcat(many, "key a\n");
    }
    CHECK(!rb_parse(many, &s, err, sizeof(err)) && strcmp(err, "line 65: more than 64 steps") == 0);
    many[RB_MAX_STEPS * 6] = '\0';
    CHECK(rb_parse(many, &s, err, sizeof(err)) && s.count == RB_MAX_STEPS);

    /* The typing table: every printable ASCII byte has a key, the pairs agree
       on the physical key, and the control bytes do not type. */
    for (int c = 0x20; c < 0x7f; c++) {
        uint8_t u;
        bool sh;
        CHECK(rb_ascii_usage((char)c, &u, &sh));
    }
    uint8_t u1, u2;
    bool s1, s2;
    CHECK(rb_ascii_usage('a', &u1, &s1) && rb_ascii_usage('A', &u2, &s2) && u1 == u2 && !s1 && s2);
    CHECK(rb_ascii_usage('1', &u1, &s1) && rb_ascii_usage('!', &u2, &s2) && u1 == u2 && !s1 && s2);
    CHECK(rb_ascii_usage('/', &u1, &s1) && rb_ascii_usage('?', &u2, &s2) && u1 == 0x38 && u2 == 0x38);
    CHECK(rb_ascii_usage('\n', &u1, &s1) && u1 == 0x28);
    CHECK(rb_ascii_usage('\t', &u1, &s1) && u1 == 0x2b);
    CHECK(!rb_ascii_usage('\r', &u1, &s1));
    CHECK(!rb_ascii_usage('\0', &u1, &s1));
    CHECK(!rb_ascii_usage((char)0xc3, &u1, &s1));

    CHECK(strcmp(rb_kind_name(RB_GONE), "gone") == 0);

    /* DuckyScript: detected by its upper-case verbs and translated to native. */
    CHECK(rb_parse("REM open a terminal\n"
                   "DELAY 500\n"
                   "GUI r\n"
                   "DELAY 200\n"
                   "STRING cmd\n"
                   "ENTER\n"
                   "CTRL ALT DELETE\n",
                   &s, err, sizeof(err)));
    CHECK(s.count == 6);
    CHECK(s.steps[0].kind == RB_DELAY && s.steps[0].value == 500);
    CHECK(s.steps[1].kind == RB_KEY && s.steps[1].mod == 0x08 && s.steps[1].code == 0x15); /* gui+r */
    CHECK(s.steps[2].kind == RB_DELAY && s.steps[2].value == 200);
    CHECK(s.steps[3].kind == RB_TYPE && strcmp(s.steps[3].arg, "cmd") == 0);
    CHECK(s.steps[4].kind == RB_KEY && s.steps[4].mod == 0 && s.steps[4].code == 0x28); /* enter */
    CHECK(s.steps[5].kind == RB_KEY && s.steps[5].mod == 0x05 && s.steps[5].code == 0x4c); /* ctrl+alt+del */

    /* STRINGLN adds an Enter; a lone GUI is the Windows key; DEFAULT_DELAY drops
       a pause after each command; REPEAT re-runs the line before it. */
    CHECK(rb_parse("DEFAULT_DELAY 100\nGUI\nSTRINGLN hi\nDOWN\nREPEAT 2\n", &s, err, sizeof(err)));
    /* GUI, delay, type hi, key enter, delay, key down, delay, then REPEAT 2 of
       the DOWN line = key down, delay, key down, delay. */
    CHECK(s.count == 11);
    CHECK(s.steps[0].kind == RB_KEY && s.steps[0].mod == 0x08 && s.steps[0].code == 0);
    CHECK(s.steps[1].kind == RB_DELAY && s.steps[1].value == 100);
    CHECK(s.steps[2].kind == RB_TYPE && strcmp(s.steps[2].arg, "hi") == 0);
    CHECK(s.steps[3].kind == RB_KEY && s.steps[3].code == 0x28);
    CHECK(s.steps[5].kind == RB_KEY && s.steps[5].code == 0x51); /* down */
    CHECK(s.steps[7].code == 0x51 && s.steps[9].code == 0x51);   /* the two repeats */

    /* Ducky refusals keep the source line number. */
    CHECK(!rb_parse("STRING ok\nWiggle now\n", &s, err, sizeof(err)) &&
          strcmp(err, "line 2: unknown key \"Wiggle\"") == 0);
    CHECK(!rb_parse("DELAY soon", &s, err, sizeof(err)) && strstr(err, "DELAY wants") != NULL);
    CHECK(!rb_parse("REPEAT 2", &s, err, sizeof(err)) && strstr(err, "nothing before it") != NULL);
    CHECK(!rb_parse("STRING caf\xc3\xa9", &s, err, sizeof(err)) && strstr(err, "ASCII only") != NULL);

    /* A native script that happens to use upper-case verbs is still native:
       KEY/TYPE are not DuckyScript keywords, so detection leaves them alone. */
    CHECK(rb_parse("KEY ctrl+c\nTYPE hello\n", &s, err, sizeof(err)) && s.count == 2 &&
          s.steps[0].kind == RB_KEY && s.steps[1].kind == RB_TYPE);

    /* Recording and screenshots: start (for a length, or by the setting), stop. */
    CHECK(rb_parse("record\nrecord 300\nscreenshot\nrecord stop\n", &s, err, sizeof(err)));
    CHECK(s.count == 4);
    CHECK(s.steps[0].kind == RB_RECORD && s.steps[0].value == 0);
    CHECK(s.steps[1].kind == RB_RECORD && s.steps[1].value == 300);
    CHECK(s.steps[2].kind == RB_SCREENSHOT);
    CHECK(s.steps[3].kind == RB_RECORD_STOP);
    CHECK(!rb_parse("record soon", &s, err, sizeof(err)) && strstr(err, "record wants") != NULL);
    CHECK(rb_parse("timelapse 10\ntimelapse 30 7200\n", &s, err, sizeof(err)));
    CHECK(s.count == 2 && s.steps[0].kind == RB_TIMELAPSE && s.steps[0].every == 10 && s.steps[0].value == 0);
    CHECK(s.steps[1].every == 30 && s.steps[1].value == 7200);
    CHECK(!rb_parse("timelapse", &s, err, sizeof(err)) && strstr(err, "timelapse wants") != NULL);
    CHECK(!rb_parse("timelapse 0", &s, err, sizeof(err)) && strstr(err, "timelapse wants") != NULL);
    CHECK(!rb_parse("timelapse 10 soon", &s, err, sizeof(err)) && strstr(err, "timelapse wants") != NULL);
    CHECK(!rb_parse("record 0", &s, err, sizeof(err)) && strstr(err, "record wants") != NULL);
    CHECK(!rb_parse("screenshot now", &s, err, sizeof(err)) && strstr(err, "takes nothing") != NULL);

    /* HDMI-CEC: standby, wake, a remote key; the key name is checked when it runs. */
    CHECK(rb_parse("hdmi standby\nHDMI Wake\nhdmi key up\n", &s, err, sizeof(err)));
    CHECK(s.count == 3 && s.steps[0].kind == RB_HDMI && strcmp(s.steps[1].arg, "wake") == 0);
    CHECK(strcmp(s.steps[2].arg, "key up") == 0);
    CHECK(!rb_parse("hdmi", &s, err, sizeof(err)) && strstr(err, "hdmi wants") != NULL);
    CHECK(!rb_parse("hdmi key", &s, err, sizeof(err)) && strstr(err, "hdmi wants") != NULL);
    CHECK(!rb_parse("hdmi reboot", &s, err, sizeof(err)) && strstr(err, "hdmi wants") != NULL);

    printf(g_fail ? "%d FAILED\n" : "runbook script: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
