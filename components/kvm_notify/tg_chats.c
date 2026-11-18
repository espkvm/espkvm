/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tg_chats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A name longer than this is cut. Telegram allows 255 characters. */
#define NAME_RAW_MAX 128

/* End of the object that starts at @p p (on the '{'), or NULL. */
static const char *object_end(const char *p)
{
    int depth = 0;
    bool in_str = false;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == '"') {
                in_str = false;
            }
        } else if (*p == '"') {
            in_str = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

/* Where the value of "key" starts inside [s, e), or NULL. A quote inside a
   string is always escaped, so "key": cannot match inside a value. */
static const char *find_key(const char *s, const char *e, const char *key)
{
    char pat[32];
    const int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    for (const char *p = s; p + n <= e; p++) {
        if (memcmp(p, pat, (size_t)n) == 0) {
            return p + n;
        }
    }
    return NULL;
}

/* Copy a string value as it stands (still JSON-escaped) to @p out, cut at
   @p limit bytes without splitting an escape or a UTF-8 character.
   Returns bytes written, or -1 if the value is not a string. */
static int copy_raw(const char *v, const char *e, char *out, size_t limit)
{
    if (!v || v >= e || *v != '"') {
        return -1;
    }
    size_t o = 0;
    for (const char *p = v + 1; p < e && *p != '"';) {
        size_t step = 1;
        const unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            step = p[1] == 'u' ? 6 : 2;
        } else if (c >= 0xf0) {
            step = 4;
        } else if (c >= 0xe0) {
            step = 3;
        } else if (c >= 0xc0) {
            step = 2;
        }
        if (p + step > e || o + step > limit) {
            break;
        }
        memcpy(out + o, p, step);
        o += step;
        p += step;
    }
    out[o] = '\0';
    return (int)o;
}

int tg_chats_from_updates(const char *json, char *out, size_t cap, int max_chats)
{
    int64_t seen[32];
    if (max_chats > (int)(sizeof(seen) / sizeof(seen[0]))) {
        max_chats = (int)(sizeof(seen) / sizeof(seen[0]));
    }
    int count = 0;
    size_t o = 0;
    if (cap < 3) {
        return -1;
    }
    out[o++] = '[';

    for (const char *p = strstr(json, "\"chat\":{"); p && count < max_chats;
         p = strstr(p + 1, "\"chat\":{")) {
        const char *s = p + 7;
        const char *e = object_end(s);
        if (!e) {
            break; /* a reply cut short */
        }
        const char *idv = find_key(s, e, "id");
        if (!idv) {
            continue;
        }
        char *end = NULL;
        const long long id = strtoll(idv, &end, 10);
        if (end == idv) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < count; i++) {
            dup = dup || seen[i] == id;
        }
        if (dup) {
            continue;
        }

        char type[24] = "";
        char name[NAME_RAW_MAX * 2 + 2] = "";
        (void)copy_raw(find_key(s, e, "type"), e, type, sizeof(type) - 1);
        if (copy_raw(find_key(s, e, "title"), e, name, NAME_RAW_MAX) <= 0) {
            int n = copy_raw(find_key(s, e, "first_name"), e, name, NAME_RAW_MAX);
            if (n > 0) {
                char last[NAME_RAW_MAX + 1];
                if (copy_raw(find_key(s, e, "last_name"), e, last, NAME_RAW_MAX) > 0) {
                    name[n++] = ' ';
                    strcpy(name + n, last);
                }
            } else {
                (void)copy_raw(find_key(s, e, "username"), e, name, NAME_RAW_MAX);
            }
        }

        const int w = snprintf(out + o, cap - o, "%s{\"id\":\"%lld\",\"type\":\"%s\",\"name\":\"%s\"}",
                               count ? "," : "", id, type, name);
        if (w < 0 || (size_t)w >= cap - o - 1) {
            return -1;
        }
        o += (size_t)w;
        seen[count++] = id;
    }
    out[o++] = ']';
    out[o] = '\0';
    return count;
}

bool tg_bot_username(const char *json, char *out, size_t cap)
{
    out[0] = '\0';
    const char *e = json + strlen(json);
    return cap > 1 && copy_raw(find_key(json, e, "username"), e, out, cap - 1) > 0;
}
