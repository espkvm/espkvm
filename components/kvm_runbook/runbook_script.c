/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The runbook parser. The grammar is the console's macro grammar plus the
 * waits, and the key names are the same ones, so a macro pasted in here runs.
 */
#include "runbook_script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MOD_LCTRL 0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT 0x04
#define MOD_LGUI 0x08

typedef struct {
    const char *name;
    uint8_t value;
} named_t;

static const named_t k_modifiers[] = {
    {"ctrl", MOD_LCTRL},  {"control", MOD_LCTRL}, {"shift", MOD_LSHIFT}, {"alt", MOD_LALT},
    {"option", MOD_LALT}, {"gui", MOD_LGUI},      {"win", MOD_LGUI},     {"cmd", MOD_LGUI},
    {"super", MOD_LGUI},  {"meta", MOD_LGUI},
};

static const named_t k_keys[] = {
    {"enter", 0x28},     {"return", 0x28},     {"esc", 0x29},        {"escape", 0x29},
    {"backspace", 0x2a}, {"tab", 0x2b},        {"space", 0x2c},      {"minus", 0x2d},
    {"equal", 0x2e},     {"capslock", 0x39},   {"printscreen", 0x46}, {"prtsc", 0x46},
    {"sysrq", 0x46},     {"scrolllock", 0x47}, {"pause", 0x48},      {"insert", 0x49},
    {"ins", 0x49},       {"home", 0x4a},       {"pageup", 0x4b},     {"pgup", 0x4b},
    {"delete", 0x4c},    {"del", 0x4c},        {"end", 0x4d},        {"pagedown", 0x4e},
    {"pgdn", 0x4e},      {"right", 0x4f},      {"left", 0x50},       {"down", 0x51},
    {"up", 0x52},
};

static bool lookup(const named_t *table, size_t n, const char *name, uint8_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            *out = table[i].value;
            return true;
        }
    }
    return false;
}

/* a-z, 0-9 and f1-f12 by rule; the rest by table. */
static bool key_usage(const char *name, uint8_t *out)
{
    const size_t len = strlen(name);
    if (len == 1 && name[0] >= 'a' && name[0] <= 'z') {
        *out = (uint8_t)(0x04 + (name[0] - 'a'));
        return true;
    }
    if (len == 1 && name[0] >= '1' && name[0] <= '9') {
        *out = (uint8_t)(0x1e + (name[0] - '1'));
        return true;
    }
    if (len == 1 && name[0] == '0') {
        *out = 0x27;
        return true;
    }
    if (name[0] == 'f' && len >= 2 && len <= 3) {
        char *end;
        const long f = strtol(name + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) {
            *out = (uint8_t)(0x3a + (f - 1));
            return true;
        }
    }
    return lookup(k_keys, sizeof(k_keys) / sizeof(k_keys[0]), name, out);
}

static void fail(char *err, size_t cap, uint16_t line, const char *what)
{
    if (err && cap) {
        snprintf(err, cap, "line %u: %s", (unsigned)line, what);
    }
}

/* "ctrl+alt+f2" -> modifier bits and one usage. The messages match the
   console's, so the editor and the device disagree about nothing. */
static bool parse_combo(const char *combo, uint8_t *mod, uint8_t *code, char *err, size_t cap,
                        uint16_t line)
{
    *mod = 0;
    *code = 0;
    char buf[64];
    if (strlen(combo) >= sizeof(buf)) {
        fail(err, cap, line, "that chord is too long");
        return false;
    }
    strcpy(buf, combo);
    /* Split on '+' by hand: strtok_r is POSIX, and this file runs on a host too. */
    char *tok = buf;
    while (tok) {
        char *plus = strchr(tok, '+');
        if (plus) {
            *plus = '\0';
        }
        char *next = plus ? plus + 1 : NULL;
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        size_t n = strlen(tok);
        while (n && (tok[n - 1] == ' ' || tok[n - 1] == '\t')) {
            tok[--n] = '\0';
        }
        if (n) {
            for (size_t i = 0; i < n; i++) {
                tok[i] = (char)tolower((unsigned char)tok[i]);
            }
            uint8_t v;
            if (lookup(k_modifiers, sizeof(k_modifiers) / sizeof(k_modifiers[0]), tok, &v)) {
                *mod |= v;
            } else if (key_usage(tok, &v)) {
                if (*code) {
                    fail(err, cap, line, "more than one non-modifier key in the chord");
                    return false;
                }
                *code = v;
            } else {
                char what[RB_ERR_MAX];
                snprintf(what, sizeof(what), "unknown key \"%s\"", tok);
                fail(err, cap, line, what);
                return false;
            }
        }
        tok = next;
    }
    /* A lone modifier is a real keypress - GUI opens the Start menu - and the
       report path already sends a modifier with no key. Refuse only an empty
       chord. */
    if (!*code && !*mod) {
        fail(err, cap, line, "no key in the chord");
        return false;
    }
    return true;
}

static bool parse_number(const char *s, long lo, long hi, uint32_t *out)
{
    if (!*s) {
        return false;
    }
    char *end;
    const long v = strtol(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_native(const char *src, rb_script_t *out, char *err, size_t err_cap)
{
    out->count = 0;
    if (err && err_cap) {
        err[0] = '\0';
    }
    uint16_t line_no = 0;
    const char *p = src;
    while (*p) {
        line_no++;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *line = p;
        p = nl ? nl + 1 : p + len;

        /* Trim both ends. */
        while (len && (line[0] == ' ' || line[0] == '\t' || line[0] == '\r')) {
            line++;
            len--;
        }
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) {
            len--;
        }
        if (!len || line[0] == '#') {
            continue;
        }
        if (out->count >= RB_MAX_STEPS) {
            fail(err, err_cap, line_no, "more than 64 steps");
            return false;
        }
        if (len >= RB_ARG_MAX + 8) {
            fail(err, err_cap, line_no, "that line is too long");
            return false;
        }

        /* verb, then the rest of the line. */
        char verb[12];
        size_t v = 0;
        while (v < len && line[v] != ' ' && line[v] != '\t') {
            if (v >= sizeof(verb) - 1) {
                fail(err, err_cap, line_no, "unknown command");
                return false;
            }
            verb[v] = (char)tolower((unsigned char)line[v]);
            v++;
        }
        verb[v] = '\0';
        const char *rest = line + v;
        size_t rlen = len - v;
        while (rlen && (rest[0] == ' ' || rest[0] == '\t')) {
            rest++;
            rlen--;
        }
        char arg[RB_ARG_MAX];
        if (rlen >= sizeof(arg)) {
            fail(err, err_cap, line_no, "that line is too long");
            return false;
        }
        memcpy(arg, rest, rlen);
        arg[rlen] = '\0';

        rb_step_t *st = &out->steps[out->count];
        memset(st, 0, sizeof(*st));
        st->line = line_no;

        if (strcmp(verb, "key") == 0) {
            st->kind = RB_KEY;
            if (!parse_combo(arg, &st->mod, &st->code, err, err_cap, line_no)) {
                return false;
            }
        } else if (strcmp(verb, "type") == 0) {
            st->kind = RB_TYPE;
            if (!rlen) {
                fail(err, err_cap, line_no, "nothing to type");
                return false;
            }
            for (size_t i = 0; i < rlen; i++) {
                uint8_t usage;
                bool shift;
                if (!rb_ascii_usage(arg[i], &usage, &shift)) {
                    char what[RB_ERR_MAX];
                    snprintf(what, sizeof(what), "cannot type \"%c\" - US layout, ASCII only",
                             (unsigned char)arg[i] < 0x80 ? arg[i] : '?');
                    fail(err, err_cap, line_no, what);
                    return false;
                }
            }
        } else if (strcmp(verb, "delay") == 0) {
            st->kind = RB_DELAY;
            if (!parse_number(arg, 1, 60000, &st->value)) {
                fail(err, err_cap, line_no, "delay wants 1..60000 milliseconds");
                return false;
            }
        } else if (strcmp(verb, "timeout") == 0) {
            st->kind = RB_TIMEOUT;
            if (!parse_number(arg, 1, 3600, &st->value)) {
                fail(err, err_cap, line_no, "timeout wants 1..3600 seconds");
                return false;
            }
        } else if (strcmp(verb, "record") == 0) {
            /* "record", "record <seconds>" or "record stop". It starts and goes on
             * to the next line: the recording runs on its own. */
            if (!rlen) {
                st->kind = RB_RECORD;
            } else if (strcmp(arg, "stop") == 0) {
                st->kind = RB_RECORD_STOP;
            } else {
                st->kind = RB_RECORD;
                if (!parse_number(arg, 1, 86400, &st->value)) {
                    fail(err, err_cap, line_no, "record wants nothing, \"stop\" or 1..86400 seconds");
                    return false;
                }
            }
        } else if (strcmp(verb, "timelapse") == 0) {
            /* "timelapse <every>" or "timelapse <every> <seconds>"; "record stop"
             * ends it. Like record, it runs on its own. */
            st->kind = RB_TIMELAPSE;
            char first[16] = "";
            const char *length = strchr(arg, ' ');
            size_t first_len = length ? (size_t)(length - arg) : strlen(arg);
            length = length ? length + 1 : "";
            if (first_len >= sizeof(first)) {
                first_len = 0; /* too long to be a number */
            }
            memcpy(first, arg, first_len);
            first[first_len] = '\0';
            uint32_t every = 0;
            if (!parse_number(first, 1, 3600, &every) ||
                (*length && !parse_number(length, 1, 604800, &st->value))) {
                fail(err, err_cap, line_no,
                     "timelapse wants 1..3600 seconds between frames, then how long it runs");
                return false;
            }
            st->every = (uint16_t)every;
        } else if (strcmp(verb, "screenshot") == 0) {
            st->kind = RB_SCREENSHOT;
            if (rlen) {
                fail(err, err_cap, line_no, "screenshot takes nothing after it");
                return false;
            }
        } else if (strcmp(verb, "hdmi") == 0) {
            /* HDMI-CEC to the source: "hdmi standby", "hdmi wake", "hdmi key up".
             * The key name is checked when it runs: the list lives with CEC. */
            st->kind = RB_HDMI;
            for (size_t i = 0; i < rlen; i++) {
                arg[i] = (char)tolower((unsigned char)arg[i]);
            }
            const bool key = strncmp(arg, "key ", 4) == 0 && arg[4];
            if (strcmp(arg, "standby") != 0 && strcmp(arg, "wake") != 0 && !key) {
                fail(err, err_cap, line_no, "hdmi wants standby, wake or key <name>");
                return false;
            }
        } else if (strcmp(verb, "wait") == 0 || strcmp(verb, "gone") == 0) {
            st->kind = verb[0] == 'w' ? RB_WAIT : RB_GONE;
            if (!rlen) {
                fail(err, err_cap, line_no, "which phrase?");
                return false;
            }
            if (rlen >= RB_PHRASE_MAX) {
                fail(err, err_cap, line_no, "a phrase is at most 63 characters");
                return false;
            }
            /* "wait area Name" names a screen area, which may be called anything. */
            const bool area = rlen > 5 && strncasecmp(arg, "area ", 5) == 0;
            for (size_t i = 0; i < rlen && !area; i++) {
                if ((unsigned char)arg[i] >= 0x80) {
                    fail(err, err_cap, line_no, "a phrase is ASCII only - that is all the screen reader knows");
                    return false;
                }
            }
        } else {
            char what[RB_ERR_MAX];
            snprintf(what, sizeof(what), "unknown command \"%s\"", verb);
            fail(err, err_cap, line_no, what);
            return false;
        }
        memcpy(st->arg, arg, rlen + 1);
        out->count++;
    }
    if (!out->count) {
        fail(err, err_cap, 1, "the runbook has no steps");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ *
 * DuckyScript
 *
 * A second way in, so the payloads people already have run here. Hak5's
 * DuckyScript is UPPERCASE verbs, one per line: STRING types, DELAY waits,
 * REM is a comment, and a line of modifiers and a key (GUI r, CTRL ALT DELETE)
 * is a chord. It has no way to wait for the screen, so it maps cleanly onto
 * the macro half of our language and never onto wait/gone.
 *
 * The whole thing is translated to our native lines and then parsed as those,
 * so there is one engine and one set of limits. Detection is by case: our
 * verbs are lower case, DuckyScript's are upper, so a script is DuckyScript
 * when its first real line begins with a DuckyScript keyword.
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *ducky;
    const char *native;
} keymap_t;

/* DuckyScript modifier -> our modifier name. */
static const keymap_t k_ducky_mods[] = {
    {"CTRL", "ctrl"}, {"CONTROL", "ctrl"}, {"SHIFT", "shift"}, {"ALT", "alt"},
    {"GUI", "gui"},   {"WINDOWS", "gui"},  {"COMMAND", "gui"},
};

/* DuckyScript named key -> our key name. Single letters and digits are handled
   by rule, and F1..F12 too, so only the named keys are here. */
static const keymap_t k_ducky_keys[] = {
    {"ENTER", "enter"},         {"ESCAPE", "esc"},      {"ESC", "esc"},
    {"TAB", "tab"},             {"SPACE", "space"},     {"BACKSPACE", "backspace"},
    {"DELETE", "delete"},       {"DEL", "delete"},      {"INSERT", "insert"},
    {"HOME", "home"},           {"END", "end"},         {"PAGEUP", "pageup"},
    {"PAGEDOWN", "pagedown"},   {"UP", "up"},           {"UPARROW", "up"},
    {"DOWN", "down"},           {"DOWNARROW", "down"},  {"LEFT", "left"},
    {"LEFTARROW", "left"},      {"RIGHT", "right"},     {"RIGHTARROW", "right"},
    {"CAPSLOCK", "capslock"},   {"SCROLLLOCK", "scrolllock"},
    {"PRINTSCREEN", "printscreen"}, {"PAUSE", "pause"}, {"BREAK", "pause"},
};

static const char *map_lookup(const keymap_t *t, size_t n, const char *word)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(t[i].ducky, word) == 0) {
            return t[i].native;
        }
    }
    return NULL;
}

static const char *ducky_mod(const char *word)
{
    return map_lookup(k_ducky_mods, sizeof(k_ducky_mods) / sizeof(k_ducky_mods[0]), word);
}

/* A DuckyScript key token -> our native name, into @p out. A single letter or
   digit becomes itself lower-cased; F1..F12 pass through lower-cased. */
static bool ducky_key(const char *word, char *out, size_t cap)
{
    const size_t len = strlen(word);
    if (len == 1 && (isalpha((unsigned char)word[0]) || isdigit((unsigned char)word[0]))) {
        out[0] = (char)tolower((unsigned char)word[0]);
        out[1] = '\0';
        return true;
    }
    if ((word[0] == 'F' || word[0] == 'f') && (len == 2 || len == 3)) {
        char *end;
        const long f = strtol(word + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) {
            snprintf(out, cap, "f%ld", f);
            return true;
        }
    }
    const char *n = map_lookup(k_ducky_keys, sizeof(k_ducky_keys) / sizeof(k_ducky_keys[0]), word);
    if (n) {
        snprintf(out, cap, "%s", n);
        return true;
    }
    return false;
}

/* Split a line into its first word and the rest (both trimmed). */
static void first_word(const char *line, char *word, size_t word_cap, const char **rest)
{
    size_t i = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t') {
        if (i < word_cap - 1) {
            word[i] = line[i];
        }
        i++;
    }
    word[i < word_cap ? i : word_cap - 1] = '\0';
    const char *r = line + i;
    while (*r == ' ' || *r == '\t') {
        r++;
    }
    *rest = r;
}

static bool is_ducky_command(const char *w)
{
    static const char *const cmds[] = {"REM", "STRING", "STRINGLN", "DELAY",
                                       "DEFAULT_DELAY", "DEFAULTDELAY", "REPEAT"};
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strcmp(cmds[i], w) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether @p src should be read as DuckyScript rather than our own language.
   True when the first non-blank, non-comment line begins with a DuckyScript
   keyword - which, being upper case, can never be one of our lower-case verbs. */
static bool is_ducky(const char *src)
{
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *line = p;
        p = nl ? nl + 1 : p + len;
        while (len && (*line == ' ' || *line == '\t' || *line == '\r')) {
            line++;
            len--;
        }
        if (!len) {
            continue;
        }
        char w[16];
        const char *rest;
        char one[64];
        size_t n = len < sizeof(one) - 1 ? len : sizeof(one) - 1;
        memcpy(one, line, n);
        one[n] = '\0';
        first_word(one, w, sizeof(w), &rest);
        char keybuf[8];
        return is_ducky_command(w) || ducky_mod(w) != NULL ||
               map_lookup(k_ducky_keys, sizeof(k_ducky_keys) / sizeof(k_ducky_keys[0]), w) != NULL ||
               ((w[0] == 'F') && ducky_key(w, keybuf, sizeof(keybuf)));
    }
    return false;
}

/* A growing native-text buffer with an overflow flag, so one length check at
   the end covers every append. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ovf;
} sink_t;

static void emit(sink_t *s, const char *line)
{
    const size_t n = strlen(line);
    if (s->len + n + 2 > s->cap) {
        s->ovf = true;
        return;
    }
    memcpy(s->buf + s->len, line, n);
    s->len += n;
    s->buf[s->len++] = '\n';
    s->buf[s->len] = '\0';
}

/* Turn one DuckyScript line into native lines. @p def_delay, when set, is
   emitted as a `delay` after a key or a string, the way DEFAULT_DELAY works. */
static bool ducky_line(const char *line, sink_t *s, uint32_t def_delay, char *err, size_t err_cap,
                       uint16_t no)
{
    char verb[16];
    const char *rest;
    first_word(line, verb, sizeof(verb), &rest);

    if (strcmp(verb, "REM") == 0 || strcmp(verb, "DEFAULT_DELAY") == 0 ||
        strcmp(verb, "DEFAULTDELAY") == 0) {
        return true; /* handled by the caller, or a comment */
    }
    if (strcmp(verb, "STRING") == 0 || strcmp(verb, "STRINGLN") == 0) {
        if (*rest) {
            for (const char *c = rest; *c; c++) {
                if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) {
                    fail(err, err_cap, no, "STRING is US layout, ASCII only");
                    return false;
                }
            }
            char out[RB_ARG_MAX + 8];
            snprintf(out, sizeof(out), "type %s", rest);
            emit(s, out);
        }
        if (strcmp(verb, "STRINGLN") == 0) {
            emit(s, "key enter");
        }
        if (def_delay) {
            char d[24];
            snprintf(d, sizeof(d), "delay %u", (unsigned)def_delay);
            emit(s, d);
        }
        return true;
    }
    if (strcmp(verb, "DELAY") == 0) {
        uint32_t ms;
        if (!parse_number(rest, 1, 60000, &ms)) {
            fail(err, err_cap, no, "DELAY wants 1..60000 milliseconds");
            return false;
        }
        char d[24];
        snprintf(d, sizeof(d), "delay %u", (unsigned)ms);
        emit(s, d);
        return true;
    }

    /* Anything else is a key or a chord: modifiers and at most one key. */
    char chord[96];
    size_t clen = 0;
    chord[0] = '\0';
    char key[16] = {0};
    char tokbuf[96];
    snprintf(tokbuf, sizeof(tokbuf), "%s", line);
    char *cur = tokbuf;
    while (*cur) {
        while (*cur == ' ' || *cur == '\t') {
            cur++;
        }
        if (!*cur) {
            break;
        }
        char *tok = cur;
        while (*cur && *cur != ' ' && *cur != '\t') {
            cur++;
        }
        if (*cur) {
            *cur++ = '\0';
        }
        const char *m = ducky_mod(tok);
        if (m) {
            clen += (size_t)snprintf(chord + clen, sizeof(chord) - clen, "%s%s", clen ? "+" : "", m);
            continue;
        }
        if (key[0]) {
            fail(err, err_cap, no, "more than one key in the line");
            return false;
        }
        if (!ducky_key(tok, key, sizeof(key))) {
            char what[RB_ERR_MAX];
            snprintf(what, sizeof(what), "unknown key \"%.60s\"", tok);
            fail(err, err_cap, no, what);
            return false;
        }
    }
    char out[128];
    if (key[0]) {
        snprintf(out, sizeof(out), "key %s%s%s", chord, clen ? "+" : "", key);
    } else if (clen) {
        snprintf(out, sizeof(out), "key %s", chord); /* a lone modifier, e.g. GUI */
    } else {
        fail(err, err_cap, no, "empty line reached the key handler");
        return false;
    }
    emit(s, out);
    if (def_delay) {
        char d[24];
        snprintf(d, sizeof(d), "delay %u", (unsigned)def_delay);
        emit(s, d);
    }
    return true;
}

/* Translate a whole DuckyScript into native text, malloc'd (caller frees). */
static char *ducky_to_native(const char *src, char *err, size_t err_cap)
{
    const size_t cap = strlen(src) * 3 + 256;
    sink_t s = {.buf = malloc(cap), .cap = cap};
    if (!s.buf) {
        fail(err, err_cap, 0, "out of memory");
        return NULL;
    }
    s.buf[0] = '\0';

    uint32_t def_delay = 0;
    char prev[RB_ARG_MAX + 16] = {0}; /* the last real line, for REPEAT */
    uint16_t no = 0;
    const char *p = src;
    while (*p) {
        no++;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[RB_ARG_MAX + 16];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl ? nl + 1 : p + len;

        char *l = line;
        while (*l == ' ' || *l == '\t' || *l == '\r') {
            l++;
        }
        size_t e = strlen(l);
        while (e && (l[e - 1] == ' ' || l[e - 1] == '\t' || l[e - 1] == '\r')) {
            l[--e] = '\0';
        }
        if (!e) {
            continue;
        }

        char verb[16];
        const char *rest;
        first_word(l, verb, sizeof(verb), &rest);

        if (strcmp(verb, "REM") == 0) {
            continue;
        }
        if (strcmp(verb, "DEFAULT_DELAY") == 0 || strcmp(verb, "DEFAULTDELAY") == 0) {
            if (!parse_number(rest, 0, 60000, &def_delay)) {
                fail(err, err_cap, no, "DEFAULT_DELAY wants 0..60000 milliseconds");
                free(s.buf);
                return NULL;
            }
            continue;
        }
        if (strcmp(verb, "REPEAT") == 0) {
            uint32_t times;
            if (!parse_number(rest, 1, 1000, &times)) {
                fail(err, err_cap, no, "REPEAT wants 1..1000");
                free(s.buf);
                return NULL;
            }
            if (!prev[0]) {
                fail(err, err_cap, no, "REPEAT with nothing before it");
                free(s.buf);
                return NULL;
            }
            for (uint32_t i = 0; i < times; i++) {
                if (!ducky_line(prev, &s, def_delay, err, err_cap, no)) {
                    free(s.buf);
                    return NULL;
                }
            }
            continue;
        }

        if (!ducky_line(l, &s, def_delay, err, err_cap, no)) {
            free(s.buf);
            return NULL;
        }
        snprintf(prev, sizeof(prev), "%s", l);
    }

    if (s.ovf) {
        fail(err, err_cap, 0, "the script is too long once expanded");
        free(s.buf);
        return NULL;
    }
    return s.buf;
}

bool rb_parse(const char *src, rb_script_t *out, char *err, size_t err_cap)
{
    if (is_ducky(src)) {
        char *native = ducky_to_native(src, err, err_cap);
        if (!native) {
            out->count = 0;
            return false;
        }
        const bool ok = parse_native(native, out, err, err_cap);
        free(native);
        return ok;
    }
    return parse_native(src, out, err, err_cap);
}

bool rb_ascii_usage(char c, uint8_t *usage, bool *shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        *usage = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *usage = (uint8_t)(0x04 + (c - 'A'));
        *shift = true;
        return true;
    }
    if (c >= '1' && c <= '9') {
        *usage = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    /* Unshifted then shifted, each pair one physical key on a US keyboard. */
    static const char k_plain[] = "0 \n\t-=[]\\;'`,./";
    static const uint8_t k_plain_usage[] = {0x27, 0x2C, 0x28, 0x2B, 0x2D, 0x2E, 0x2F, 0x30,
                                            0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    static const char k_shifted[] = "_+{}|:\"~<>?!@#$%^&*()";
    static const uint8_t k_shifted_usage[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34,
                                              0x35, 0x36, 0x37, 0x38, 0x1E, 0x1F, 0x20,
                                              0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
    const char *hit = c ? strchr(k_plain, c) : NULL;
    if (hit) {
        *usage = k_plain_usage[hit - k_plain];
        return true;
    }
    hit = c ? strchr(k_shifted, c) : NULL;
    if (hit) {
        *usage = k_shifted_usage[hit - k_shifted];
        *shift = true;
        return true;
    }
    return false;
}

const char *rb_kind_name(rb_kind_t kind)
{
    switch (kind) {
    case RB_KEY: return "key";
    case RB_TYPE: return "type";
    case RB_DELAY: return "delay";
    case RB_TIMEOUT: return "timeout";
    case RB_WAIT: return "wait";
    case RB_GONE: return "gone";
    case RB_RECORD: return "record";
    case RB_RECORD_STOP: return "record stop";
    case RB_SCREENSHOT: return "screenshot";
    case RB_TIMELAPSE: return "timelapse";
    case RB_HDMI: return "hdmi";
    }
    return "?";
}
